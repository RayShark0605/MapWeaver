#include "GeoCrsManager.h"

#include "GeoCrs.h"

#include "GB_FileSystem.h"
#include "GB_Logger.h"
#include "GB_ReadWriteLock.h"
#include "GB_Utf8String.h"

#include <atomic>
#include <cctype>
#include <deque>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// GDAL
#include <cpl_conv.h>
#include <cpl_string.h>
#include <gdal.h>
#include <ogr_srs_api.h>

#ifdef _WIN32
#  include <Windows.h>
#else
#  include <dirent.h>
#  include <limits.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

// GeoBoundingBox 在 GeoCrs 的接口中使用，这里按项目结构包含。
#include "GeoBoundingBox.h"

namespace
{
    struct DefinitionKey
    {
        std::string definitionUtf8 = "";
        bool allowNetworkAccess = false;
        bool allowFileAccess = false;

        bool operator==(const DefinitionKey& other) const
        {
            return allowNetworkAccess == other.allowNetworkAccess &&
                allowFileAccess == other.allowFileAccess &&
                definitionUtf8 == other.definitionUtf8;
        }
    };

    struct DefinitionKeyHasher
    {
        size_t operator()(const DefinitionKey& key) const
        {
            const std::hash<std::string> stringHasher;
            size_t hashValue = stringHasher(key.definitionUtf8);

            // 轻量 hash combine
            hashValue ^= static_cast<size_t>(key.allowNetworkAccess) + 0x9e3779b97f4a7c15ULL + (hashValue << 6) + (hashValue >> 2);
            hashValue ^= static_cast<size_t>(key.allowFileAccess) + 0x9e3779b97f4a7c15ULL + (hashValue << 6) + (hashValue >> 2);
            return hashValue;
        }
    };

    struct ValidAreas
    {
        GeoBoundingBox lonLatArea;
        GeoBoundingBox selfArea;
    };

    // -------------------- 全局状态与缓存 --------------------

    std::atomic_bool g_isInitialized(false);
    std::string g_projDatabaseDirUtf8 = "";
    GB_ReadWriteLock g_initLock;

    GB_ReadWriteLock g_epsgCacheLock;
    std::unordered_map<int, std::shared_ptr<const GeoCrs>> g_epsgCache;

    GB_ReadWriteLock g_wktCacheLock;
    std::unordered_map<std::string, std::shared_ptr<const GeoCrs>> g_wktCache;

    GB_ReadWriteLock g_wktValidityCacheLock;
    std::unordered_map<std::string, bool> g_wktValidityCache;

    GB_ReadWriteLock g_definitionCacheLock;
    std::unordered_map<DefinitionKey, std::shared_ptr<const GeoCrs>, DefinitionKeyHasher> g_definitionCache;

    GB_ReadWriteLock g_validAreaCacheLock;
    std::unordered_map<std::string, ValidAreas> g_validAreaCache;

    std::shared_ptr<const GeoCrs> GetEmptyCrsShared()
    {
        static const std::shared_ptr<const GeoCrs> emptyCrs = std::make_shared<GeoCrs>();
        return emptyCrs;
    }

    // -------------------- 工具函数 --------------------

    bool IsWindowsDriveRootUtf8(const std::string& dirPathUtf8)
    {
        return dirPathUtf8.size() == 3 &&
            std::isalpha(static_cast<unsigned char>(dirPathUtf8[0])) &&
            dirPathUtf8[1] == ':' &&
            dirPathUtf8[2] == '/';
    }

    bool IsUncShareRootUtf8(const std::string& dirPathUtf8)
    {
        if (dirPathUtf8.rfind("//", 0) != 0)
        {
            return false;
        }

        // 形如：//server/share/   —— 认为是 UNC share 根目录，不允许再向上
        const size_t firstSlash = dirPathUtf8.find('/', 2);
        if (firstSlash == std::string::npos)
        {
            return true;
        }

        const size_t secondSlash = dirPathUtf8.find('/', firstSlash + 1);
        if (secondSlash == std::string::npos)
        {
            return true;
        }

        const size_t thirdSlash = dirPathUtf8.find('/', secondSlash + 1);
        return thirdSlash == std::string::npos;
    }

    bool IsRootedPathUtf8(const std::string& pathUtf8)
    {
        if (pathUtf8.empty())
        {
            return false;
        }

        // POSIX: 以 '/' 开头；UNC: 以 '//' 开头；两者都属于“有根路径”
        if (pathUtf8[0] == '/')
        {
            return true;
        }

        // Windows: 盘符 "C:" / "D:" ...
        if (pathUtf8.size() >= 2 &&
            std::isalpha(static_cast<unsigned char>(pathUtf8[0])) &&
            pathUtf8[1] == ':')
        {
            return true;
        }

        return false;
    }

    std::string NormalizeDirPathUtf8(const std::string& inputPathUtf8)
    {
        if (inputPathUtf8.empty())
        {
            return "";
        }

        // 使用 GB_JoinPath 做统一分隔符、消解 "."/".." 等 lexical 规范化，并强制视为“目录”路径。
        // 右侧传入 "." 可确保返回结果末尾带 "/"（即使该目录并不存在）。
        std::string normalized = GB_JoinPath(inputPathUtf8, ".");
        if (!normalized.empty() && normalized.back() != '/')
        {
            normalized.push_back('/');
        }

        return normalized;
    }

    std::string GetParentDirectoryUtf8(const std::string& dirPathUtf8)
    {
        const std::string currentDir = NormalizeDirPathUtf8(dirPathUtf8);
        if (currentDir.empty())
        {
            return "";
        }

        // 对于相对路径，保持“无法继续向上”的语义，避免出现 "./" -> "../" 的漂移。
        if (!IsRootedPathUtf8(currentDir))
        {
            return "";
        }

        // 根目录："/" or "C:/" or "//server/share/"
        if (currentDir == "/" || IsWindowsDriveRootUtf8(currentDir) || IsUncShareRootUtf8(currentDir))
        {
            return "";
        }

        const std::string parentDir = NormalizeDirPathUtf8(GB_JoinPath(currentDir, ".."));
        if (parentDir.empty() || parentDir == currentDir)
        {
            return "";
        }

        return parentDir;
    }

    bool EndsWithIgnoreCaseAscii(const std::string& text, const std::string& suffix)
    {
        if (suffix.size() > text.size())
        {
            return false;
        }

        const size_t offset = text.size() - suffix.size();
        for (size_t i = 0; i < suffix.size(); i++)
        {
            const unsigned char a = static_cast<unsigned char>(text[offset + i]);
            const unsigned char b = static_cast<unsigned char>(suffix[i]);
            if (std::tolower(a) != std::tolower(b))
            {
                return false;
            }
        }
        return true;
    }

    std::string GetCurrentWorkingDirectoryUtf8()
    {
#ifdef _WIN32
        DWORD requiredChars = GetCurrentDirectoryW(0, nullptr);
        if (requiredChars == 0)
        {
            return "";
        }

        std::wstring buffer;
        buffer.resize(static_cast<size_t>(requiredChars));
        const DWORD written = GetCurrentDirectoryW(requiredChars, &buffer[0]);
        if (written == 0)
        {
            return "";
        }

        // written 不包含 '\0'，但 buffer 可能包含
        buffer.resize(static_cast<size_t>(written));
        std::string cwdUtf8 = GB_WStringToUtf8(buffer);
        cwdUtf8 = NormalizeDirPathUtf8(cwdUtf8);
        return cwdUtf8;
#else
        char buffer[PATH_MAX + 1];
        if (getcwd(buffer, sizeof(buffer)) == nullptr)
        {
            return "";
        }

        std::string cwdUtf8(buffer);
        cwdUtf8 = NormalizeDirPathUtf8(cwdUtf8);
        return cwdUtf8;
#endif
    }

    std::string FindProjDatabaseDirByExistingProjPaths()
    {
        char** paths = OSRGetPROJSearchPaths();
        if (paths == nullptr)
        {
            return "";
        }

        std::string found;
        for (int i = 0; paths[i] != nullptr; i++)
        {
            const std::string dir = NormalizeDirPathUtf8(paths[i]);
            if (dir.empty())
            {
                continue;
            }

            const std::string projDbPath = GB_JoinPath(dir, "proj.db");
            if (GB_IsFileExists(projDbPath))
            {
                found = dir;
                break;
            }
        }

        CSLDestroy(paths);
        return found;
    }

    struct ProjDbSearchNode
    {
        std::string dirUtf8;
        int depth = 0;
    };

    // 有界深度/数量的“兜底”搜索：用于覆盖少数非典型目录布局，但避免递归扫描整个磁盘导致启动抖动。
    std::string LimitedSearchProjDbUnderDirUtf8(const std::string& startDirUtf8, int maxDepth, size_t maxVisitedDirs)
    {
        const std::string rootDir = NormalizeDirPathUtf8(startDirUtf8);
        if (rootDir.empty())
        {
            return "";
        }

        std::deque<ProjDbSearchNode> queue;
        queue.push_back({ rootDir, 0 });

        size_t visitedDirs = 0;

        while (!queue.empty() && visitedDirs < maxVisitedDirs)
        {
            ProjDbSearchNode node = std::move(queue.front());
            queue.pop_front();
            ++visitedDirs;

            const std::string projDbPath = GB_JoinPath(node.dirUtf8, "proj.db");
            if (GB_IsFileExists(projDbPath))
            {
                return NormalizeDirPathUtf8(node.dirUtf8);
            }

            if (node.depth >= maxDepth)
            {
                continue;
            }

#ifdef _WIN32
            std::wstring pattern = GB_Utf8ToWString(node.dirUtf8);
            if (pattern.empty())
            {
                continue;
            }

            if (pattern.back() != L'\\' && pattern.back() != L'/')
            {
                pattern.push_back(L'\\');
            }
            pattern += L"*";

            WIN32_FIND_DATAW findData;
            HANDLE findHandle = FindFirstFileW(pattern.c_str(), &findData);
            if (findHandle == INVALID_HANDLE_VALUE)
            {
                continue;
            }

            do
            {
                const std::wstring entryNameW = findData.cFileName;
                if (entryNameW == L"." || entryNameW == L"..")
                {
                    continue;
                }

                if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
                {
                    continue;
                }

                // 跳过重解析点（符号链接/目录联接等），避免出现循环或跨盘扫描。
                if ((findData.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
                {
                    continue;
                }

                const std::string entryNameUtf8 = GB_WStringToUtf8(entryNameW);
                if (entryNameUtf8.empty())
                {
                    continue;
                }

                const std::string childDir = NormalizeDirPathUtf8(GB_JoinPath(node.dirUtf8, entryNameUtf8 + "/"));
                if (!childDir.empty())
                {
                    if (visitedDirs + queue.size() >= maxVisitedDirs)
                    {
                        break;
                    }
                    queue.push_back({ childDir, node.depth + 1 });
                }
            } while (FindNextFileW(findHandle, &findData));

            FindClose(findHandle);
#else
            DIR* dir = opendir(node.dirUtf8.c_str());
            if (dir == nullptr)
            {
                continue;
            }

            while (true)
            {
                dirent* entry = readdir(dir);
                if (entry == nullptr)
                {
                    break;
                }

                const std::string entryName = entry->d_name ? entry->d_name : "";
                if (entryName.empty() || entryName == "." || entryName == "..")
                {
                    continue;
                }

                const std::string childPath = GB_JoinPath(node.dirUtf8, entryName);

                struct stat st;
                if (lstat(childPath.c_str(), &st) != 0)
                {
                    continue;
                }

                // 跳过符号链接，避免循环与意外跨目录树。
                if (S_ISLNK(st.st_mode))
                {
                    continue;
                }

                if (!S_ISDIR(st.st_mode))
                {
                    continue;
                }

                const std::string childDir = NormalizeDirPathUtf8(childPath);
                if (!childDir.empty())
                {
                    if (visitedDirs + queue.size() >= maxVisitedDirs)
                    {
                        break;
                    }
                    queue.push_back({ childDir, node.depth + 1 });
                }
            }

            closedir(dir);
#endif
        }

        return "";
    }

    std::string FindProjDatabaseDirBySearching(const std::string& startDirUtf8)
    {
        std::string currentDir = NormalizeDirPathUtf8(startDirUtf8);
        if (currentDir.empty())
        {
            return "";
        }

        // 只做“轻量探测”：在每一级目录中尝试若干常见的 PROJ 资源布局，而不是递归扫描整个子树。
        // 典型布局示例：
        //  - <dir>/proj.db
        //  - <dir>/share/proj/proj.db
        //  - <dir>/proj/proj.db
        //  - <dir>/Library/share/proj/proj.db   (Windows/Conda 常见)
        while (!currentDir.empty())
        {
            const std::string candidates[] =
            {
                currentDir,
                NormalizeDirPathUtf8(GB_JoinPath(currentDir, "share/proj/")),
                NormalizeDirPathUtf8(GB_JoinPath(currentDir, "proj/")),
                NormalizeDirPathUtf8(GB_JoinPath(currentDir, "Library/share/proj/"))
            };

            for (const std::string& candidateDir : candidates)
            {
                if (candidateDir.empty())
                {
                    continue;
                }

                const std::string projDbPath = GB_JoinPath(candidateDir, "proj.db");
                if (GB_IsFileExists(projDbPath))
                {
                    return NormalizeDirPathUtf8(candidateDir);
                }
            }

            const std::string parent = GetParentDirectoryUtf8(currentDir);
            if (parent.empty() || parent == currentDir)
            {
                break;
            }

            currentDir = parent;
        }

        // 兜底：在 startDir 下做一次有界深度搜索，覆盖少数“非典型安装布局”。
        return LimitedSearchProjDbUnderDirUtf8(startDirUtf8, 5, 5000);
    }

    bool ApplyProjDatabaseDirectoryUtf8Internal(const std::string& projDatabaseDirUtf8)
    {
        const std::string dir = NormalizeDirPathUtf8(projDatabaseDirUtf8);
        if (dir.empty())
        {
            return false;
        }

        const std::string projDbPath = GB_JoinPath(dir, "proj.db");
        if (!GB_IsFileExists(projDbPath))
        {
            return false;
        }

        // 兼容：PROJ_DATA 是新变量名，但很多环境仍依赖 PROJ_LIB。
        CPLSetConfigOption("PROJ_LIB", dir.c_str());
        CPLSetConfigOption("PROJ_DATA", dir.c_str());

        const char* const paths[] = { dir.c_str(), nullptr };
        OSRSetPROJSearchPaths(paths);

        return true;
    }

    void ClearCachesInternal()
    {
        {
            GB_WriteLockGuard guard(g_epsgCacheLock);
            g_epsgCache.clear();
        }
        {
            GB_WriteLockGuard guard(g_wktCacheLock);
            g_wktCache.clear();
        }
        {
            GB_WriteLockGuard guard(g_wktValidityCacheLock);
            g_wktValidityCache.clear();
        }
        {
            GB_WriteLockGuard guard(g_definitionCacheLock);
            g_definitionCache.clear();
        }
        {
            GB_WriteLockGuard guard(g_validAreaCacheLock);
            g_validAreaCache.clear();
        }
    }
} // namespace

bool GeoCrsManager::IsInitialized()
{
    return g_isInitialized.load(std::memory_order_acquire);
}

std::string GeoCrsManager::GetProjDbDirectoryUtf8()
{
    EnsureInitializedInternal();

    GB_ReadLockGuard guard(g_initLock);
    return g_projDatabaseDirUtf8;
}

bool GeoCrsManager::SetProjDbDirectoryUtf8(const std::string& projDatabaseDirUtf8)
{
    const std::string dir = NormalizeDirPathUtf8(projDatabaseDirUtf8);
    if (dir.empty())
    {
        GBLOG_WARNING(GB_STR("【GeoCrsManager::SetProjDbDirectoryUtf8】目录为空。"));
        return false;
    }

    if (!ApplyProjDatabaseDirectoryUtf8Internal(dir))
    {
        GBLOG_WARNING(GB_STR("【GeoCrsManager::SetProjDbDirectoryUtf8】未找到 proj.db: ") + dir);
        return false;
    }

    {
        GB_WriteLockGuard guard(g_initLock);
        g_projDatabaseDirUtf8 = dir;
        g_isInitialized.store(true, std::memory_order_release);
    }

    ClearCachesInternal();

    int major = 0;
    int minor = 0;
    int patch = 0;
    OSRGetPROJVersion(&major, &minor, &patch);
    GBLOG_INFO(GB_STR("【GeoCrsManager】已设置 PROJ 数据目录: ") + dir + GB_STR(" (PROJ版本=") +
        std::to_string(major) + GB_STR(".") + std::to_string(minor) + GB_STR(".") + std::to_string(patch) + GB_STR(")"));

    return true;
}

bool GeoCrsManager::ReinitializeBySearchingProjDb()
{
    const std::string cwd = GetCurrentWorkingDirectoryUtf8();
    const std::string exeDir = GB_GetExeDirectory();

    std::string found;

    if (!cwd.empty())
    {
        found = FindProjDatabaseDirBySearching(cwd);
    }
    if (found.empty() && !exeDir.empty())
    {
        found = FindProjDatabaseDirBySearching(exeDir);
    }

    if (found.empty())
    {
        GBLOG_WARNING(GB_STR("【GeoCrsManager::ReinitializeBySearchingProjDb】未找到 proj.db。"));
        return false;
    }

    return SetProjDbDirectoryUtf8(found);
}

void GeoCrsManager::ClearCaches()
{
    ClearCachesInternal();
}

std::shared_ptr<const GeoCrs> GeoCrsManager::GetWgs84()
{
    return GetFromEpsgCached(4326);
}

std::shared_ptr<const GeoCrs> GeoCrsManager::GetWebMercator()
{
    return GetFromEpsgCached(3857);
}

std::shared_ptr<const GeoCrs> GeoCrsManager::GetFromEpsgCached(int epsgCode)
{
    EnsureInitializedInternal();

    if (epsgCode <= 0)
    {
        GBLOG_WARNING(GB_STR("【GeoCrsManager::GetFromEpsgCached】epsgCode 非正: ") + std::to_string(epsgCode));
        return GetEmptyCrsShared();
    }

    {
        GB_ReadLockGuard readGuard(g_epsgCacheLock);
        const auto it = g_epsgCache.find(epsgCode);
        if (it != g_epsgCache.end())
        {
            return it->second;
        }
    }

    std::shared_ptr<const GeoCrs> crs = std::make_shared<GeoCrs>(GeoCrs::CreateFromEpsgCode(epsgCode));

    {
        GB_WriteLockGuard writeGuard(g_epsgCacheLock);
        const auto it = g_epsgCache.find(epsgCode);
        if (it != g_epsgCache.end())
        {
            return it->second;
        }
        g_epsgCache.emplace(epsgCode, crs);
    }

    return crs;
}

std::shared_ptr<const GeoCrs> GeoCrsManager::GetFromDefinitionCached(const std::string& definitionUtf8, bool allowNetworkAccess, bool allowFileAccess)
{
    EnsureInitializedInternal();

    const std::string trimmed = GB_Utf8Trim(definitionUtf8);
    if (trimmed.empty())
    {
        GBLOG_WARNING(GB_STR("【GeoCrsManager::GetFromDefinitionCached】definition 为空。"));
        return GetEmptyCrsShared();
    }

    const DefinitionKey key{ trimmed, allowNetworkAccess, allowFileAccess };

    {
        GB_ReadLockGuard readGuard(g_definitionCacheLock);
        const auto it = g_definitionCache.find(key);
        if (it != g_definitionCache.end())
        {
            return it->second;
        }
    }

    std::shared_ptr<const GeoCrs> crs = std::make_shared<GeoCrs>(GeoCrs::CreateFromUserInput(trimmed, allowNetworkAccess, allowFileAccess));

    {
        GB_WriteLockGuard writeGuard(g_definitionCacheLock);
        const auto it = g_definitionCache.find(key);
        if (it != g_definitionCache.end())
        {
            return it->second;
        }
        g_definitionCache.emplace(key, crs);
    }

    return crs;
}

bool GeoCrsManager::IsWktValidCached(const std::string& wktUtf8)
{
    EnsureInitializedInternal();

    const std::string trimmed = GB_Utf8Trim(wktUtf8);
    if (trimmed.empty())
    {
        return false;
    }

    {
        GB_ReadLockGuard readGuard(g_wktValidityCacheLock);
        const auto it = g_wktValidityCache.find(trimmed);
        if (it != g_wktValidityCache.end())
        {
            return it->second;
        }
    }

    // 若 CRS 缓存已有，也可直接复用。
    {
        GB_ReadLockGuard readGuard(g_wktCacheLock);
        const auto it = g_wktCache.find(trimmed);
        if (it != g_wktCache.end() && it->second)
        {
            const bool valid = it->second->IsValid();
            GB_WriteLockGuard writeGuard(g_wktValidityCacheLock);
            g_wktValidityCache.emplace(trimmed, valid);
            return valid;
        }
    }

    const GeoCrs temp = GeoCrs::CreateFromWkt(trimmed);
    const bool valid = temp.IsValid();

    {
        GB_WriteLockGuard writeGuard(g_wktValidityCacheLock);
        const auto it = g_wktValidityCache.find(trimmed);
        if (it != g_wktValidityCache.end())
        {
            return it->second;
        }
        g_wktValidityCache.emplace(trimmed, valid);
    }

    return valid;
}

std::shared_ptr<const GeoCrs> GeoCrsManager::GetFromWktCached(const std::string& wktUtf8)
{
    EnsureInitializedInternal();

    const std::string trimmed = GB_Utf8Trim(wktUtf8);
    if (trimmed.empty())
    {
        GBLOG_WARNING(GB_STR("【GeoCrsManager::GetFromWktCached】wkt 为空。"));
        return GetEmptyCrsShared();
    }

    {
        GB_ReadLockGuard readGuard(g_wktCacheLock);
        const auto it = g_wktCache.find(trimmed);
        if (it != g_wktCache.end())
        {
            return it->second;
        }
    }

    std::shared_ptr<const GeoCrs> crs = std::make_shared<GeoCrs>(GeoCrs::CreateFromWkt(trimmed));

    {
        GB_WriteLockGuard writeGuard(g_wktCacheLock);
        const auto it = g_wktCache.find(trimmed);
        if (it != g_wktCache.end())
        {
            return it->second;
        }
        g_wktCache.emplace(trimmed, crs);
    }

    // 也同步写入 validity cache（避免重复解析）
    {
        const bool valid = crs ? crs->IsValid() : false;
        GB_WriteLockGuard writeGuard(g_wktValidityCacheLock);
        g_wktValidityCache.emplace(trimmed, valid);
    }

    return crs;
}

bool GeoCrsManager::TryGetValidAreasCached(const std::string& wktUtf8, GeoBoundingBox& outLonLatArea, GeoBoundingBox& outSelfArea)
{
    EnsureInitializedInternal();

    const std::string trimmed = GB_Utf8Trim(wktUtf8);
    if (trimmed.empty())
    {
        outLonLatArea = GeoBoundingBox();
        outSelfArea = GeoBoundingBox();
        return false;
    }

    {
        GB_ReadLockGuard readGuard(g_validAreaCacheLock);
        const auto it = g_validAreaCache.find(trimmed);
        if (it != g_validAreaCache.end())
        {
            outLonLatArea = it->second.lonLatArea;
            outSelfArea = it->second.selfArea;
            return outLonLatArea.IsValid() && outSelfArea.IsValid();
        }
    }

    // 复用 CRS 缓存（优先），避免重复解析。
    std::shared_ptr<const GeoCrs> crs;
    {
        GB_ReadLockGuard readGuard(g_wktCacheLock);
        const auto it = g_wktCache.find(trimmed);
        if (it != g_wktCache.end())
        {
            crs = it->second;
        }
    }

    if (!crs)
    {
        const std::shared_ptr<const GeoCrs> created = std::make_shared<GeoCrs>(GeoCrs::CreateFromWkt(trimmed));

        GB_WriteLockGuard writeGuard(g_wktCacheLock);
        const auto it = g_wktCache.find(trimmed);
        if (it != g_wktCache.end())
        {
            crs = it->second;
        }
        else
        {
            crs = created;
            g_wktCache.emplace(trimmed, crs);
        }
    }

    ValidAreas areas;
    if (crs)
    {
        areas.lonLatArea = crs->GetValidAreaLonLat();
        areas.selfArea = crs->GetValidArea();
    }

    {
        GB_WriteLockGuard writeGuard(g_validAreaCacheLock);
        const auto it = g_validAreaCache.find(trimmed);
        if (it != g_validAreaCache.end())
        {
            outLonLatArea = it->second.lonLatArea;
            outSelfArea = it->second.selfArea;
            return outLonLatArea.IsValid() && outSelfArea.IsValid();
        }

        g_validAreaCache.emplace(trimmed, areas);
    }

    outLonLatArea = areas.lonLatArea;
    outSelfArea = areas.selfArea;
    return outLonLatArea.IsValid() && outSelfArea.IsValid();
}

size_t GeoCrsManager::GetCachedEpsgCount()
{
    GB_ReadLockGuard guard(g_epsgCacheLock);
    return g_epsgCache.size();
}

size_t GeoCrsManager::GetCachedWktCount()
{
    GB_ReadLockGuard guard(g_wktCacheLock);
    return g_wktCache.size();
}

size_t GeoCrsManager::GetCachedDefinitionCount()
{
    GB_ReadLockGuard guard(g_definitionCacheLock);
    return g_definitionCache.size();
}

size_t GeoCrsManager::GetCachedValidAreaCount()
{
    GB_ReadLockGuard guard(g_validAreaCacheLock);
    return g_validAreaCache.size();
}

void GeoCrsManager::EnsureInitializedInternal()
{
    if (g_isInitialized.load(std::memory_order_acquire))
    {
        return;
    }

    GB_WriteLockGuard guard(g_initLock);
    if (g_isInitialized.load(std::memory_order_acquire))
    {
        return;
    }

    // 1) 如果外部已经配置了 PROJ search paths，并且其中能找到 proj.db，则直接采用
    const std::string existingDir = FindProjDatabaseDirByExistingProjPaths();
    if (!existingDir.empty())
    {
        g_projDatabaseDirUtf8 = existingDir;
        g_isInitialized.store(true, std::memory_order_release);
        return;
    }

    // 2) 否则按策略自动查找
    const std::string cwd = GetCurrentWorkingDirectoryUtf8();
    const std::string exeDir = GB_GetExeDirectory();

    std::string found;
    if (!cwd.empty())
    {
        found = FindProjDatabaseDirBySearching(cwd);
    }
    if (found.empty() && !exeDir.empty())
    {
        found = FindProjDatabaseDirBySearching(exeDir);
    }

    if (!found.empty())
    {
        if (ApplyProjDatabaseDirectoryUtf8Internal(found))
        {
            g_projDatabaseDirUtf8 = found;
            g_isInitialized.store(true, std::memory_order_release);
            return;
        }
    }

    // 3) 找不到也视为“初始化完成”：避免每次都进行大范围扫描。
    //    这种情况下，GeoCrs/OGR 会继续使用系统默认的 PROJ 配置（若存在）。
    g_projDatabaseDirUtf8.clear();
    g_isInitialized.store(true, std::memory_order_release);
    GBLOG_WARNING(GB_STR("【GeoCrsManager】未能自动定位 proj.db，将使用系统默认 PROJ 配置。"));
}
