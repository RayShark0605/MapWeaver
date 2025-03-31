#include "Base.h"
#include <algorithm>
#include <Windows.h>

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
