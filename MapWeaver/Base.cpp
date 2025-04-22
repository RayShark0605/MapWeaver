#include "Base.h"
#include <algorithm>
#include <Windows.h>
#include <filesystem>

using namespace std;

string GetTempDirPath()
{
	char exePath[MAX_PATH];
	DWORD length = GetModuleFileNameA(nullptr, exePath, MAX_PATH);
	if (length == 0 || length >= MAX_PATH)
	{
		return "";
	}

	char* lastSlash = strrchr(exePath, '\\');
	if (!lastSlash)
	{
		return "";
	}
	*lastSlash = '\0';

	char tempDir[MAX_PATH];
	if (sprintf_s(tempDir, "%s\\temp", exePath) == -1)
	{
		return "";
	}

	if (!CreateDirectoryA(tempDir, nullptr))
	{
		DWORD error = GetLastError();
		if (error != ERROR_ALREADY_EXISTS)
		{
			return "";
		}
	}

	string result = tempDir;
	replace(result.begin(), result.end(), '\\', '/');
	return result;
}

string GetProjDirPath()
{
	char exePath[MAX_PATH];
	DWORD length = GetModuleFileNameA(nullptr, exePath, MAX_PATH);
	if (length == 0 || length >= MAX_PATH)
	{
		return "";
	}

	char* lastSlash = strrchr(exePath, '\\');
	if (!lastSlash)
	{
		return "";
	}
	*lastSlash = '\0';

	char tempDir[MAX_PATH];
	if (sprintf_s(tempDir, "%s\\proj", exePath) == -1)
	{
		return "";
	}

	string result = tempDir;
	replace(result.begin(), result.end(), '\\', '/');
	return result + "/";
}

void ForceDeleteFile(const string& filePath)
{
	filesystem::path path(filePath);

	// 清除只读等属性，确保 DeleteFile 不会因为文件属性而失败
	::SetFileAttributesW(path.wstring().c_str(), FILE_ATTRIBUTE_NORMAL);

	error_code errorCode;
	filesystem::remove(path, errorCode);
}

bool FileExists(const string& filePath)
{
	filesystem::path path(filePath);
	error_code errorCode;
	return filesystem::exists(path, errorCode);
}
