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

	static inline std::string GetXmlNodeValue(const CPLXMLNode* node)
	{
		if (!node)
		{
			return "";
		}

		if (node->eType == CXT_Text)
		{
			return node->pszValue ? node->pszValue : "";
		}

		const char* value = CPLGetXMLValue(node, nullptr, "");
		if (!value)
		{
			return "";
		}
		return value;
	}

	static CPLXMLNode* FindChildElement(const CPLXMLNode* parent, const std::string& childName)
	{
		if (!parent || parent->eType != CXT_Element)
		{
			return nullptr;
		}

		for (CPLXMLNode* curNode = parent->psChild; curNode != nullptr; curNode = curNode->psNext)
		{
			if (curNode->eType == CXT_Element)
			{
				const std::string nodeName = GetXmlNodeTagName(curNode);
				if (GB_Utf8Equals(nodeName, childName, false))
				{
					return curNode;
				}
			}
		}
		return nullptr;
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

			for (CPLXMLNode* curNode = rootNode->psChild; curNode != nullptr; curNode = curNode->psNext)
			{
				if (curNode->eType != CXT_Element)
				{
					continue;
				}

				const std::string nodeName = GetXmlNodeTagName(curNode);
				if (GB_Utf8Equals(nodeName, GB_STR("Service"), false) || GB_Utf8Equals(nodeName, GB_STR("ows:ServiceProvider"), false) ||
					GB_Utf8Equals(nodeName, GB_STR("ows:ServiceIdentification"), false))
				{
					ParseService(curNode, capabilitiesProperty.service);
				}





			}
			return true;
		}

		void ParseService(const CPLXMLNode* rootNode, WmsServiceProperty& serviceProperty)
		{
			if (!rootNode)
			{
				return;
			}

			for (CPLXMLNode* curNode = rootNode->psChild; curNode != nullptr; curNode = curNode->psNext)
			{
				if (curNode->eType != CXT_Element)
				{
					continue;
				}

				std::string nodeName = GetXmlNodeTagName(curNode);
				if (GB_Utf8StartsWith(nodeName, GB_STR("wms:"), false) || GB_Utf8StartsWith(nodeName, GB_STR("ows:"), false))
				{
					nodeName = GB_Utf8Substr(nodeName, 4);
				}

				if (GB_Utf8Equals(nodeName, GB_STR("Title"), false))
				{
					serviceProperty.titleUtf8 = GetXmlNodeValue(curNode);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("Abstract"), false))
				{
					serviceProperty.abstractUtf8 = GetXmlNodeValue(curNode);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("KeywordList"), false) || GB_Utf8Equals(nodeName, GB_STR("Keywords"), false))
				{
					ParseKeywordList(curNode, serviceProperty.keywordsUtf8);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("OnlineResource"), false))
				{
					ParseOnlineResource(curNode, serviceProperty.onlineResource);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("ContactInformation"), false) || GB_Utf8Equals(nodeName, GB_STR("ServiceContact"), false))
				{
					ParseContactInformation(curNode, serviceProperty.contactInformation);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("Fees"), false))
				{
					serviceProperty.feesUtf8 = GetXmlNodeValue(curNode);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("AccessConstraints"), false))
				{
					serviceProperty.accessConstraintsUtf8 = GetXmlNodeValue(curNode);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("LayerLimit"), false))
				{
					try
					{
						serviceProperty.layerLimit = static_cast<size_t>(std::stoull(GetXmlNodeValue(curNode)));
					}
					catch (const std::exception& e)
					{
						serviceProperty.layerLimit = 0;
					}
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("MaxWidth"), false))
				{
					try
					{
						serviceProperty.maxWidth = static_cast<size_t>(std::stoull(GetXmlNodeValue(curNode)));
					}
					catch (const std::exception& e)
					{
						serviceProperty.maxWidth = 0;
					}
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("MaxHeight"), false))
				{
					try
					{
						serviceProperty.maxHeight = static_cast<size_t>(std::stoull(GetXmlNodeValue(curNode)));
					}
					catch (const std::exception& e)
					{
						serviceProperty.maxHeight = 0;
					}
				}
			}
		}

		void ParseKeywordList(const CPLXMLNode* rootNode, std::vector<std::string>& keywordListProperty)
		{
			if (!rootNode)
			{
				return;
			}

			for (CPLXMLNode* curNode = rootNode->psChild; curNode != nullptr; curNode = curNode->psNext)
			{
				if (curNode->eType != CXT_Element)
				{
					continue;
				}

				std::string nodeName = GetXmlNodeTagName(curNode);
				if (GB_Utf8StartsWith(nodeName, GB_STR("wms:"), false) || GB_Utf8StartsWith(nodeName, GB_STR("ows:"), false))
				{
					nodeName = GB_Utf8Substr(nodeName, 4);
				}

				if (GB_Utf8Equals(nodeName, GB_STR("Keyword"), false))
				{
					keywordListProperty.push_back(GetXmlNodeValue(curNode));
				}
			}
		}

		void ParseOnlineResource(const CPLXMLNode* rootNode, WmsOnlineResourceAttribute& onlineResourceAttribute)
		{
			if (!rootNode)
			{
				return;
			}
			const std::string url = GetXmlNodeAttribute(rootNode, GB_STR("xlink:href"));
			if (url.empty())
			{
				return;
			}
			onlineResourceAttribute.xlinkHrefUtf8 = url;
		}

		void ParseContactInformation(const CPLXMLNode* rootNode, WmsContactInformationProperty& contactInformationProperty)
		{
			if (!rootNode)
			{
				return;
			}

			for (CPLXMLNode* curNode = rootNode->psChild; curNode != nullptr; curNode = curNode->psNext)
			{
				if (curNode->eType != CXT_Element)
				{
					continue;
				}

				std::string nodeName = GetXmlNodeTagName(curNode);
				if (GB_Utf8StartsWith(nodeName, GB_STR("wms:"), false))
				{
					nodeName = GB_Utf8Substr(nodeName, 4);
				}

				if (GB_Utf8Equals(nodeName, GB_STR("ContactPersonPrimary"), false))
				{
					ParseContactPersonPrimary(curNode, contactInformationProperty.personPrimary);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("ContactPosition"), false) || GB_Utf8Equals(nodeName, GB_STR("ows:PositionName"), false))
				{
					contactInformationProperty.positionUtf8 = GetXmlNodeValue(curNode);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("ContactAddress"), false))
				{
					ParseContactAddress(curNode, contactInformationProperty.address);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("ContactVoiceTelephone"), false))
				{
					contactInformationProperty.voiceTelephoneUtf8 = GetXmlNodeValue(curNode);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("ContactFacsimileTelephone"), false))
				{
					contactInformationProperty.facsimileTelephoneUtf8 = GetXmlNodeValue(curNode);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("ContactElectronicMailAddress"), false))
				{
					contactInformationProperty.eMailAddressUtf8 = GetXmlNodeValue(curNode);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("ows:IndividualName"), false))
				{
					contactInformationProperty.personPrimary.contactPersonUtf8 = GetXmlNodeValue(curNode);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("ows:ProviderName"), false))
				{
					contactInformationProperty.personPrimary.contactOrganizationUtf8 = GetXmlNodeValue(curNode);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("ows:ContactInfo"), false))
				{
					const CPLXMLNode* phoneNode = FindChildElement(curNode, GB_STR("ows:Phone"));
					if (phoneNode)
					{
						const CPLXMLNode* voiceNode = FindChildElement(phoneNode, GB_STR("ows:Voice"));
						if (voiceNode)
						{
							contactInformationProperty.voiceTelephoneUtf8 = GetXmlNodeValue(voiceNode);
						}

						const CPLXMLNode* facsimileNode = FindChildElement(phoneNode, GB_STR("ows:Facsimile"));
						if (facsimileNode)
						{
							contactInformationProperty.facsimileTelephoneUtf8 = GetXmlNodeValue(facsimileNode);
						}
					}

					const CPLXMLNode* addressNode = FindChildElement(curNode, GB_STR("ows:Address"));
					if (addressNode)
					{
						const CPLXMLNode* electronicMailAddressNode = FindChildElement(addressNode, GB_STR("ows:ElectronicMailAddress"));
						if (electronicMailAddressNode)
						{
							contactInformationProperty.eMailAddressUtf8 = GetXmlNodeValue(electronicMailAddressNode);
						}

						const CPLXMLNode* deliveryPointNode = FindChildElement(addressNode, GB_STR("ows:DeliveryPoint"));
						if (deliveryPointNode)
						{
							contactInformationProperty.address.addressUtf8 = GetXmlNodeValue(deliveryPointNode);
						}

						const CPLXMLNode* cityNode = FindChildElement(addressNode, GB_STR("ows:City"));
						if (cityNode)
						{
							contactInformationProperty.address.cityUtf8 = GetXmlNodeValue(cityNode);
						}

						const CPLXMLNode* administrativeAreaNode = FindChildElement(addressNode, GB_STR("ows:AdministrativeArea"));
						if (administrativeAreaNode)
						{
							contactInformationProperty.address.stateOrProvinceUtf8 = GetXmlNodeValue(administrativeAreaNode);
						}

						const CPLXMLNode* postalCodeNode = FindChildElement(addressNode, GB_STR("ows:PostalCode"));
						if (postalCodeNode)
						{
							contactInformationProperty.address.postCodeUtf8 = GetXmlNodeValue(postalCodeNode);
						}

						const CPLXMLNode* countryNode = FindChildElement(addressNode, GB_STR("ows:Country"));
						if (countryNode)
						{
							contactInformationProperty.address.countryUtf8 = GetXmlNodeValue(countryNode);
						}
					}
				}
			}
		}

		void ParseContactPersonPrimary(const CPLXMLNode* rootNode, WmsContactPersonPrimaryProperty& contactPersonPrimaryProperty)
		{
			if (!rootNode)
			{
				return;
			}

			for (CPLXMLNode* curNode = rootNode->psChild; curNode != nullptr; curNode = curNode->psNext)
			{
				if (curNode->eType != CXT_Element)
				{
					continue;
				}

				std::string nodeName = GetXmlNodeTagName(curNode);
				if (GB_Utf8StartsWith(nodeName, GB_STR("wms:"), false))
				{
					nodeName = GB_Utf8Substr(nodeName, 4);
				}

				if (GB_Utf8Equals(nodeName, GB_STR("ContactPerson"), false))
				{
					contactPersonPrimaryProperty.contactPersonUtf8 = GetXmlNodeValue(curNode);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("ContactOrganization"), false))
				{
					contactPersonPrimaryProperty.contactOrganizationUtf8 = GetXmlNodeValue(curNode);
				}
			}
		}

		void ParseContactAddress(const CPLXMLNode* rootNode, WmsContactAddressProperty& contactAddressProperty)
		{
			if (!rootNode)
			{
				return;
			}

			for (CPLXMLNode* curNode = rootNode->psChild; curNode != nullptr; curNode = curNode->psNext)
			{
				if (curNode->eType != CXT_Element)
				{
					continue;
				}

				std::string nodeName = GetXmlNodeTagName(curNode);
				if (GB_Utf8StartsWith(nodeName, GB_STR("wms:"), false))
				{
					nodeName = GB_Utf8Substr(nodeName, 4);
				}

				if (GB_Utf8Equals(nodeName, GB_STR("AddressType"), false))
				{
					contactAddressProperty.addressTypeUtf8 = GetXmlNodeValue(curNode);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("Address"), false))
				{
					contactAddressProperty.addressUtf8 = GetXmlNodeValue(curNode);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("City"), false))
				{
					contactAddressProperty.cityUtf8 = GetXmlNodeValue(curNode);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("StateOrProvince"), false))
				{
					contactAddressProperty.stateOrProvinceUtf8 = GetXmlNodeValue(curNode);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("PostCode"), false))
				{
					contactAddressProperty.postCodeUtf8 = GetXmlNodeValue(curNode);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("Country"), false))
				{
					contactAddressProperty.countryUtf8 = GetXmlNodeValue(curNode);
				}
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
