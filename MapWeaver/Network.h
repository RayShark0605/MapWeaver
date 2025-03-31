#pragma once
#include "Common.h"

// 根据url和json请求Capabilities XML文件
bool GetCapabilities(const std::string& url, std::string& content, std::string& receiveInfo, const std::string& requestJson = "", const std::string& proxyUrl = "", const std::string& proxyUserName = "", const std::string& proxyPassword = "");

// 根据url下载图片
bool DownloadImage(const std::string& url, const std::string& filePath, std::string& receiveInfo, const std::string& proxyUrl = "", const std::string& proxyUserName = "", const std::string& proxyPassword = "");
bool DownloadImageMultiThread(const std::string& url, const std::string& filePath, std::string& receiveInfo, const std::string& proxyUrl = "", const std::string& proxyUserName = "", const std::string& proxyPassword = "");

// 转义字符串
std::string EscapeString(const std::string& str);

// 获取字符串的MD5值
std::string GetStringMD5(const std::string& str);