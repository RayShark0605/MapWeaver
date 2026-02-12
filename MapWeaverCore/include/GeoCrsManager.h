#ifndef MAP_WEAVER_GEO_CRS_MANAGER_H
#define MAP_WEAVER_GEO_CRS_MANAGER_H

#include "MapWeaverPort.h"
#include "GeoCrs.h"

#include <cstddef>
#include <memory>
#include <string>

class GeoBoundingBox;
class GeoCrs;

// GeoCrsManager
// - 静态 CRS 管理器：
//   1) 自动定位并初始化 PROJ 数据库（proj.db）搜索路径；
//   2) 基于缓存的 CRS 获取/解析；
//   3) 基于缓存的 WKT 有效性判断；
//   4) 基于缓存的 CRS 有效范围（自身范围、以及 EPSG:4326 范围）计算。
class MAPWEAVERCORE_PORT GeoCrsManager
{
public:
	// 是否已完成（自动或手动）初始化。
	static bool IsInitialized();

	// 获取当前生效的 PROJ 数据库目录（UTF-8，统一用'/'，并保证以'/'结尾）。
	// 若未初始化或未能确定目录，返回空串。
	static std::string GetProjDbDirectoryUtf8();

	// 手动设置 proj.db 所在目录，并立即应用（会清空所有缓存）。
	// - projDatabaseDirUtf8：目录路径或 proj.db 文件路径（UTF-8）。函数内部会尝试定位并检查 "proj.db"。
	// 返回 true 表示设置成功并已应用；false 表示目录无效或未找到 proj.db。
	static bool SetProjDbDirectoryUtf8(const std::string& projDatabaseDirUtf8);

	// 重新执行“自动查找 proj.db”的策略并应用（会清空所有缓存）。
	// 策略：从当前工作目录开始逐级探测常见 PROJ 资源布局；若未找到则向父目录上溯继续查找；
	// 若工作目录获取失败，则以可执行程序目录作为起点。
	// 返回 true 表示找到并应用；false 表示未找到（不会覆盖外部已设置的 PROJ 搜索路径）。
	static bool ReinitializeBySearchingProjDb();

	// 清空内部所有缓存（不改变 PROJ 搜索路径）。
	static void ClearCaches();

	// 常见 EPSG：WGS84 / WebMercator。
	static std::shared_ptr<const GeoCrs> GetWgs84();       // EPSG:4326
	static std::shared_ptr<const GeoCrs> GetWebMercator(); // EPSG:3857

	// 输入 UTF-8 编码的 EPSG Code（例如 "EPSG:4326"）返回对应的 UTF-8 编码 WKT（默认 WKT2_2018）。
	// 若输入非法或无法解析/导出，则返回空串。
	static std::string EpsgCodeToWktUtf8(const std::string& epsgCodeUtf8);

	// 输入 UTF-8 编码的 WKT，尝试返回对应的 UTF-8 编码 EPSG Code（例如 "EPSG:4326"）。
	// 说明：
	//  - 若 WKT 根节点已包含 EPSG 权威码，则直接返回；
	//  - 否则会尝试使用 GDAL 的 AutoIdentifyEPSG() 做安全的推断（依赖 proj.db）。
	// 若输入非法或无法推断，则返回空串。
	static std::string WktToEpsgCodeUtf8(const std::string& wktUtf8);

	// 按 EPSG code 获取（带缓存）。
	static std::shared_ptr<const GeoCrs> GetFromEpsgCached(int epsgCode);

	// 按用户输入定义获取（带缓存）。definitionUtf8 可为 "EPSG:4326"、WKT、PROJJSON 等。
	// allowNetworkAccess / allowFileAccess 的语义与 GeoCrs::CreateFromUserInput 一致。
	static std::shared_ptr<const GeoCrs> GetFromDefinitionCached(const std::string& definitionUtf8, bool allowNetworkAccess = false, bool allowFileAccess = false);

	// 带缓存地判断 WKT 是否有效。
	static bool IsWktValidCached(const std::string& wktUtf8);

	// 带缓存地获取一个 GeoCrs（WKT 解析）。
	// 注意：返回的是共享只读对象；如需可写对象，请拷贝一份。
	static std::shared_ptr<const GeoCrs> GetFromWktCached(const std::string& wktUtf8);

	// 带缓存地获取 WKT 对应坐标系的：
	//  1) EPSG:4326 下的有效范围（经纬度），以及
	//  2) 该坐标系自身坐标系下的有效范围。
	// 返回 true 表示两者都有效；若任一无效则返回 false，但 out 参数仍会被赋值（可能为 Invalid）。
	static bool TryGetValidAreasCached(const std::string& wktUtf8, GeoBoundingBox& outLonLatArea, GeoBoundingBox& outSelfArea);

	static size_t GetCachedEpsgCount();

	static size_t GetCachedWktCount();

	static size_t GetCachedDefinitionCount();

	static size_t GetCachedValidAreaCount();

private:
	static void EnsureInitializedInternal();

	GeoCrsManager() = delete;
	~GeoCrsManager() = delete;
	GeoCrsManager(const GeoCrsManager&) = delete;
	GeoCrsManager& operator=(const GeoCrsManager&) = delete;
};

#endif
