#include "MapWeaverCore.h"
#include "GB_Utf8String.h"
#include "GB_Network.h"
#include "GB_Logger.h"

#include "cpl_minixml.h"
#include "cpl_error.h"
#include "cpl_string.h"

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

namespace
{
	static inline std::string GetXmlNodeTagName(const CPLXMLNode* node)
	{
		if (!node || node->eType != CXT_Element || !node->pszValue)
		{
			return "";
		}
		return node->pszValue;
	}

	static inline std::string GetXmlNodeAttribute(const CPLXMLNode* node, const std::string& attributeName)
	{
		if (!node || node->eType != CXT_Element)
		{
			return "";
		}

		const char* value = CPLGetXMLValue(node, attributeName.c_str(), "");
		if (!value)
		{
			return "";
		}

		return value;
	}




	class WmsCapabilitiesParser
	{
	public:
		WmsCapabilitiesParser() : valid(false), parserOptions(), numLayers(-1) {}

		bool Parse(const std::string& capabilitiesXmlUtf8, const WmsParserOptions& options)
		{
			parserOptions = options;
			valid = false;

			if (capabilitiesXmlUtf8.empty())
			{
				GBLOG_WARNING("Empty capabilities XML.");
				return false;
			}

			if (GB_Utf8StartsWith(capabilitiesXmlUtf8, GB_STR("<html>"), false))
			{
				GBLOG_WARNING("Capabilities XML appears to be an HTML page, likely an error response.");
				return false;
			}

			if (capabilitiesXmlUtf8.find('\0') != std::string::npos)
			{
				GBLOG_WARNING("Capabilities XML contains null bytes, which is unexpected and may indicate a malformed response.");
				return false;
			}

			if (!CPLIsUTF8(capabilitiesXmlUtf8.c_str(), static_cast<int>(capabilitiesXmlUtf8.size())))
			{
				GBLOG_WARNING("Capabilities XML does not appear to be valid UTF-8.");
				return false;
			}

			if (!ParseDom(capabilitiesXmlUtf8, capabilities))
			{
				return false;
			}


			return true;
		}



	private:
		bool valid = false;
		WmsParserOptions parserOptions;
		int numLayers = -1;
		WmsCapabilitiesProperty capabilities;

		bool ParseDom(const std::string& capabilitiesXmlUtf8, WmsCapabilitiesProperty& capabilitiesProperty)
		{
			CPLErrorReset();
			CPLPushErrorHandler(CPLQuietErrorHandler);
			CPLXMLNode* xmlTreePtr = CPLParseXMLString(capabilitiesXmlUtf8.c_str());
			CPLPopErrorHandler();
			if (!xmlTreePtr)
			{
				const std::string errorMessage = CPLGetLastErrorMsg();
				GBLOG_WARNING(GB_Utf8Format("Failed to parse capabilities XML. Error message: %s", errorMessage.c_str()));
				return false;
			}

			CPLXMLTreeCloser xmlDom(xmlTreePtr);
			CPLXMLNode* rootNode = xmlDom.getDocumentElement();
			if (!rootNode)
			{
				GBLOG_WARNING("Capabilities XML does not contain a root element.");
				return false;
			}

			// 检查根元素名称
			{
				const std::string rootName = GetXmlNodeTagName(rootNode);
				if (!GB_Utf8Equals(rootName, GB_STR("WMS_Capabilities"), false) && !GB_Utf8Equals(rootName, GB_STR("WMT_MS_Capabilities"), false) &&
					!GB_Utf8Equals(rootName, GB_STR("Capabilities"), false))
				{
					GBLOG_WARNING(GB_Utf8Format("Unexpected root element name in capabilities XML: '%s'.", rootName.c_str()));
					return false;
				}
			}

			capabilitiesProperty.versionUtf8 = GetXmlNodeAttribute(rootNode, GB_STR("version"));

			CPLXMLNode* curNode = rootNode->psChild;
			while (curNode)
			{
				const std::string nodeName = GetXmlNodeTagName(curNode);
				if (GB_Utf8Equals(nodeName, GB_STR("Service"), false) || GB_Utf8Equals(nodeName, GB_STR("ows:ServiceProvider"), false) ||
					GB_Utf8Equals(nodeName, GB_STR("ows:ServiceIdentification"), false))
				{
					ParseService(curNode, capabilitiesProperty.service);
				}




				curNode = curNode->psNext;
			}
			return true;
		}

		void ParseService(const CPLXMLNode* rootNode, WmsServiceProperty& serviceProperty)
		{
			if (!rootNode)
			{
				return;
			}

			CPLXMLNode* curNode = rootNode->psChild;
			while (curNode)
			{
				std::string nodeName = GetXmlNodeTagName(curNode);
				if (GB_Utf8StartsWith(nodeName, GB_STR("wms:"), false) || GB_Utf8StartsWith(nodeName, GB_STR("ows:"), false))
				{
					nodeName = GB_Utf8Substr(nodeName, 4);
				}

				if (GB_Utf8Equals(nodeName, GB_STR("Title"), false))
				{
					const char* aaa = curNode->pszValue;

				}



				curNode = curNode->psNext;
			}






		}



	};




}







bool ParseWmsCapabilities(const std::string& capabilitiesXmlUtf8, WmsCapabilitiesProperty& outCapabilities, const WmsParserOptions& options)
{
	outCapabilities = WmsCapabilitiesProperty();

	WmsCapabilitiesParser parser;
	return parser.Parse(capabilitiesXmlUtf8, options);
}
