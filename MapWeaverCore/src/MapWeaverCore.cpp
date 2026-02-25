#include "MapWeaverCore.h"
#include "GB_Utf8String.h"
#include "GB_Network.h"
#include "GB_Logger.h"

bool IsUrlForWMTS(const std::string& urlUtf8)
{
	return GB_Utf8Find(urlUtf8, GB_STR("SERVICE=WMTS"), false) > 0
		&& GB_Utf8Find(urlUtf8, GB_STR("/WMTSCapabilities.xml"), false);
}

bool DownloadWmsCapabilities(const std::string& rawUrlUtf8, std::string& outCapabilitiesXmlUtf8, const GB_NetworkRequestOptions& options)
{
	std::string urlUtf8 = rawUrlUtf8;
	if (!IsUrlForWMTS(urlUtf8))
	{
		urlUtf8 = GB_UrlOperator::SetUrlQueryValue(urlUtf8, GB_STR("SERVICE"), GB_STR("WMS"));
		urlUtf8 = GB_UrlOperator::SetUrlQueryValue(urlUtf8, GB_STR("REQUEST"), GB_STR("GetCapabilities"));
	}

	const GB_NetworkResponse response = GB_RequestUrlData(urlUtf8, options);
	if (!response.ok)
	{
		return false;
	}

	outCapabilitiesXmlUtf8 = response.body;
	return true;
}


