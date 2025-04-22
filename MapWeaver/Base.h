#pragma once
#include <string>

// 获取当前exe所在文件夹的temp文件夹路径，如果不存在则会创建
std::string GetTempDirPath();

// 获取当前exe所在文件夹的proj文件夹路径
std::string GetProjDirPath();

// 删除文件
void ForceDeleteFile(const std::string& filePath);

// 文件是否存在
bool FileExists(const std::string& filePath);