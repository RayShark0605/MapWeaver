#include "MapWeaverCore.h"
#include "GB_Utf8String.h"
#include "GB_Network.h"
#include "GB_Logger.h"

bool IsUrlForWMTS(const std::string& urlUtf8)
{
	// 包含 SERVICE=WMTS 键值对（KVP风格）
	if (GB_Utf8Find(urlUtf8, GB_STR("SERVICE=WMTS"), false) > 0)
	{
		return true;
	}

	// RESTful 风格的 WMTS
	if (GB_Utf8Find(urlUtf8, GB_STR("/WMTSCapabilities.xml"), false) > 0)
	{
		return true;
	}

	// 匹配 "/wmts?", "/wmts/" 
	if (GB_Utf8Find(urlUtf8, GB_STR("/wmts?"), false) > 0 || GB_Utf8Find(urlUtf8, GB_STR("/wmts/"), false) > 0)
	{
		return true;
	}

	// 匹配正好以 "/wmts" 结尾的情况
	if (GB_Utf8EndsWith(urlUtf8, GB_STR("/wmts"), false))
	{
		return true;
	}

	return false;
}

bool DownloadWmsCapabilities(const std::string& rawUrlUtf8, std::string& outCapabilitiesXmlUtf8, const GB_NetworkRequestOptions& options)
{
	std::string urlUtf8 = rawUrlUtf8;
	if (!IsUrlForWMTS(urlUtf8))
	{
		urlUtf8 = GB_UrlOperator::SetUrlQueryValue(urlUtf8, GB_STR("SERVICE"), GB_STR("WMS"));
		urlUtf8 = GB_UrlOperator::SetUrlQueryValue(urlUtf8, GB_STR("REQUEST"), GB_STR("GetCapabilities"));
	}

	GB_NetworkResponse response = GB_RequestUrlData(urlUtf8, options);
	if (response.ok)
	{
		outCapabilitiesXmlUtf8 = response.body;
		return true;
	}

	GBLOG_WARNING(GB_Utf8Format("Failed to download WMS capabilities from URL: '%s'. HTTP status code: %ld. Error message: %s", urlUtf8.c_str(), response.httpStatusCode, response.errorMessageUtf8.c_str()));

	response = GB_RequestUrlData(rawUrlUtf8, options);
	if (response.ok)
	{
		outCapabilitiesXmlUtf8 = response.body;
		return true;
	}

	GBLOG_WARNING(GB_Utf8Format("Failed to download WMS capabilities from original URL: '%s'. HTTP status code: %ld. Error message: %s", rawUrlUtf8.c_str(), response.httpStatusCode, response.errorMessageUtf8.c_str()));

	return false;
}


