#pragma once
#include <string>

// 获取当前exe所在文件夹的temp文件夹路径，如果不存在则会创建
std::string GetTempDirPath();

// 获取当前exe所在文件夹的proj文件夹路径
std::string GetProjDirPath();