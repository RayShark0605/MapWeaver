#include "MapWeaverCore.h"
#include "GB_Utf8String.h"
#include "GB_Network.h"
#include "GB_Logger.h"
#include "GeoCrsManager.h"
#include "GeoCrsTransform.h"

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
		std::unordered_map<int, int> layerParentIdMap; // layerId -> parentLayerId
		std::unordered_map<int, std::vector<std::string>> layerParentNamesMap; // layerId -> parentLayerNames
		std::unordered_map<std::string, bool> queryableLayerIdCache; // layerId -> isQueryable
		std::vector<WmsLayerProperty> layersSupported;
		std::vector<WmtsTileLayer> tileLayersSupported;
		std::unordered_map<std::string, WmtsTileMatrixSet> tileMatrixSets;
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
				else if (GB_Utf8Equals(nodeName, GB_STR("Capability"), false) || GB_Utf8Equals(nodeName, GB_STR("ows:OperationsMetadata"), false))
				{
					ParseCapability(curNode, capabilitiesProperty.capability);


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
						GBLOG_WARNING(GB_Utf8Format("Failed to parse LayerLimit value: '%s'. Error message: %s", GetXmlNodeValue(curNode).c_str(), e.what()));
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
						GBLOG_WARNING(GB_Utf8Format("Failed to parse MaxWidth value: '%s'. Error message: %s", GetXmlNodeValue(curNode).c_str(), e.what()));
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
						GBLOG_WARNING(GB_Utf8Format("Failed to parse MaxHeight value: '%s'. Error message: %s", GetXmlNodeValue(curNode).c_str(), e.what()));
						serviceProperty.maxHeight = 0;
					}
				}
			}
		}

		void ParseCapability(const CPLXMLNode* rootNode, WmsCapabilityProperty& capabilityProperty)
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

				if (GB_Utf8Equals(nodeName, GB_STR("Request"), false))
				{
					ParseRequest(curNode, capabilityProperty.request);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("Layer"), false))
				{
					WmsLayerProperty layer;
					ParseLayer(curNode, layer);
					capabilityProperty.layers.push_back(std::move(layer));
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("VendorSpecificCapabilities"), false))
				{
					for (CPLXMLNode* vendorNode = curNode->psChild; vendorNode != nullptr; vendorNode = vendorNode->psNext)
					{
						if (vendorNode->eType != CXT_Element)
						{
							continue;
						}

						std::string vendorNodeName = GetXmlNodeTagName(vendorNode);
						if (GB_Utf8StartsWith(vendorNodeName, GB_STR("wms:"), false))
						{
							vendorNodeName = GB_Utf8Substr(vendorNodeName, 4);
						}

						if (GB_Utf8Equals(vendorNodeName, GB_STR("TileSet"), false))
						{
							ParseTileSetProfile(vendorNode);
						}
					}
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("ows:Operation"), false) || GB_Utf8Equals(nodeName, GB_STR("Operation"), false))
				{



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

		void ParseRequest(const CPLXMLNode* rootNode, WmsRequestProperty& requestProperty)
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

				std::string operation = GetXmlNodeTagName(curNode);
				if (GB_Utf8Equals(operation, GB_STR("Operation"), false))
				{
					operation = GetXmlNodeAttribute(curNode, GB_STR("name"));
				}

				if (GB_Utf8StartsWith(operation, GB_STR("GetMap"), false))
				{
					ParseOperationType(curNode, requestProperty.getMap);
				}
				else if (GB_Utf8StartsWith(operation, GB_STR("GetFeatureInfo"), false))
				{
					ParseOperationType(curNode, requestProperty.getFeatureInfo);
				}
				else if (GB_Utf8StartsWith(operation, GB_STR("GetLegendGraphic"), false) || GB_Utf8StartsWith(operation, GB_STR("sld:GetLegendGraphic"), false))
				{
					ParseOperationType(curNode, requestProperty.getLegendGraphic);
				}
			}
		}

		void ParseOperationType(const CPLXMLNode* rootNode, WmsOperationType& operationType)
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

				if (GB_Utf8Equals(nodeName, GB_STR("Format"), false))
				{
					operationType.formatsUtf8.push_back(GetXmlNodeValue(curNode));
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("DCPType"), false))
				{
					WmsDcpTypeProperty dcpType;
					ParseDcpType(curNode, dcpType);
					operationType.dcpTypes.push_back(dcpType);
				}
			}
		}

		void ParseDcpType(const CPLXMLNode* rootNode, WmsDcpTypeProperty& dcpTypeProperty)
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

				const std::string nodeName = GetXmlNodeTagName(curNode);
				if (GB_Utf8Equals(nodeName, GB_STR("HTTP"), false))
				{
					ParseHttp(curNode, dcpTypeProperty.http);
				}
			}
		}

		void ParseHttp(const CPLXMLNode* rootNode, WmsHttpProperty& httpProperty)
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

				if (GB_Utf8Equals(nodeName, GB_STR("Get"), false))
				{
					ParseGet(curNode, httpProperty.get);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("Post"), false))
				{
					ParsePost(curNode, httpProperty.post);
				}
			}
		}

		void ParseGet(const CPLXMLNode* rootNode, WmsGetProperty& getProperty)
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

				if (GB_Utf8Equals(nodeName, GB_STR("OnlineResource"), false))
				{
					ParseOnlineResource(curNode, getProperty.onlineResource);
				}
			}
		}

		void ParsePost(const CPLXMLNode* rootNode, WmsPostProperty& postProperty)
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

				if (GB_Utf8Equals(nodeName, GB_STR("OnlineResource"), false))
				{
					ParseOnlineResource(curNode, postProperty.onlineResource);
				}
			}
		}

		void ParseLayer(const CPLXMLNode* rootNode, WmsLayerProperty& layerProperty, WmsLayerProperty* parentProperty = nullptr)
		{
			if (!rootNode)
			{
				return;
			}

			numLayers++;
			layerProperty.orderId = numLayers;

			const std::string queryableAttribute = GetXmlNodeAttribute(rootNode, GB_STR("queryable"));
			layerProperty.queryable = GB_Utf8Equals(queryableAttribute, GB_STR("1"), false) || GB_Utf8Equals(queryableAttribute, GB_STR("true"), false);

			layerProperty.cascaded = static_cast<int>(GB_ToUInt(GetXmlNodeAttribute(rootNode, GB_STR("cascaded")), 0));

			const std::string opaqueAttribute = GetXmlNodeAttribute(rootNode, GB_STR("opaque"));
			layerProperty.opaque = GB_Utf8Equals(opaqueAttribute, GB_STR("1"), false) || GB_Utf8Equals(opaqueAttribute, GB_STR("true"), false);

			const std::string noSubsetsAttribute = GetXmlNodeAttribute(rootNode, GB_STR("noSubsets"));
			layerProperty.noSubsets = GB_Utf8Equals(noSubsetsAttribute, GB_STR("1"), false) || GB_Utf8Equals(noSubsetsAttribute, GB_STR("true"), false);

			layerProperty.fixedWidth = static_cast<int>(GB_ToUInt(GetXmlNodeAttribute(rootNode, GB_STR("fixedWidth")), 0));
			layerProperty.fixedHeight = static_cast<int>(GB_ToUInt(GetXmlNodeAttribute(rootNode, GB_STR("fixedHeight")), 0));

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

				if (GB_Utf8Equals(nodeName, GB_STR("Layer"), false))
				{
					std::vector<WmsStyleProperty> inheritedStyles{ layerProperty.styles };

					CPLXMLNode* nameNode = FindChildElement(curNode, GB_STR("Name"));
					if (nameNode)
					{
						const std::string layerName = GetXmlNodeValue(nameNode);
						for (WmsStyleProperty& inheritedStyleProperty : inheritedStyles)
						{
							for (WmsLegendUrlProperty& legendUrlProperty : inheritedStyleProperty.legendUrls)
							{
								std::string legendUrl = legendUrlProperty.onlineResource.xlinkHrefUtf8;
								if (legendUrl.find('?') != std::string::npos) // 已经包含查询参数
								{
									std::string originLayerValue = "";
									if (GB_UrlOperator::TryGetUrlQueryValue(legendUrl, GB_STR("layer"), originLayerValue))
									{
										legendUrl = GB_UrlOperator::SetUrlQueryValue(legendUrl, GB_STR("layer"), layerName);
									}
									legendUrlProperty.onlineResource.xlinkHrefUtf8 = legendUrl;
								}
							}
						}
					}

					WmsLayerProperty subLayerProperty;
					subLayerProperty.styles = inheritedStyles;
					subLayerProperty.crsUtf8 = layerProperty.crsUtf8;
					subLayerProperty.boundingBoxes = layerProperty.boundingBoxes;
					subLayerProperty.exGeographicBBox = layerProperty.exGeographicBBox;
					ParseLayer(curNode, subLayerProperty, &layerProperty);
					layerProperty.subLayers.push_back(std::move(subLayerProperty));
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("Name"), false))
				{
					layerProperty.nameUtf8 = GetXmlNodeValue(curNode);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("Title"), false))
				{
					layerProperty.titleUtf8 = GetXmlNodeValue(curNode);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("Abstract"), false))
				{
					layerProperty.abstractUtf8 = GetXmlNodeValue(curNode);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("KeywordList"), false))
				{
					ParseKeywordList(curNode, layerProperty.keywordsUtf8);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("SRS"), false) || GB_Utf8Equals(nodeName, GB_STR("CRS"), false))
				{
					const std::vector<std::string> crsList = GB_Utf8Split(GetXmlNodeValue(curNode), GB_CHAR(' '));
					for (const std::string& crs : crsList)
					{
						if (std::find(layerProperty.crsUtf8.begin(), layerProperty.crsUtf8.end(), crs) == layerProperty.crsUtf8.end())
						{
							// 只添加不重复的 CRS
							layerProperty.crsUtf8.push_back(crs);
						}
					}
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("LatLonBoundingBox"), false))
				{
					try
					{
						const double minX = GB_ToDouble(GB_Utf8Replace(GetXmlNodeAttribute(curNode, GB_STR("minx")), GB_STR(","), GB_STR(".")));
						const double minY = GB_ToDouble(GB_Utf8Replace(GetXmlNodeAttribute(curNode, GB_STR("miny")), GB_STR(","), GB_STR(".")));
						const double maxX = GB_ToDouble(GB_Utf8Replace(GetXmlNodeAttribute(curNode, GB_STR("maxx")), GB_STR(","), GB_STR(".")));
						const double maxY = GB_ToDouble(GB_Utf8Replace(GetXmlNodeAttribute(curNode, GB_STR("maxy")), GB_STR(","), GB_STR(".")));
						layerProperty.exGeographicBBox.Set(minX, minY, maxX, maxY);

						const std::string srsValue = GetXmlNodeAttribute(curNode, GB_STR("SRS"));
						if (!srsValue.empty() && !GB_Utf8Equals(nodeName, GB_STR("CRS:84"), false) && GeoCrsManager::IsDefinitionValidCached(srsValue))
						{
							// 如果 SRS 属性存在且不是 CRS:84，那么就尝试将 LatLonBoundingBox 转换到 CRS:84 坐标系下
							const GeoBoundingBox originalBBox(GB_ToWkt(srsValue), layerProperty.exGeographicBBox);
							GeoBoundingBox targetBBox;
							if (GeoCrsTransform::TransformBoundingBox(originalBBox, GB_ToWkt("CRS:84"), targetBBox) && targetBBox.IsValid())
							{
								layerProperty.exGeographicBBox = targetBBox.rect;
							}
							else
							{
								GBLOG_WARNING(GB_Utf8Format("Failed to transform LatLonBoundingBox from SRS '%s' to 'CRS:84'. Original SRS: '%s'.", srsValue.c_str(), srsValue.c_str()));
							}
						}
					}
					catch (const std::exception& e)
					{
						GBLOG_WARNING(GB_Utf8Format("Failed to parse LatLonBoundingBox attributes. Error message: %s", e.what()));
					}
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("EX_GeographicBoundingBox"), false))
				{
					const CPLXMLNode* westBoundLongitudeNode = nullptr;
					const CPLXMLNode* eastBoundLongitudeNode = nullptr;
					const CPLXMLNode* southBoundLatitudeNode = nullptr;
					const CPLXMLNode* northBoundLatitudeNode = nullptr;
					{
						const std::string originNodeName = GetXmlNodeTagName(curNode);
						if (GB_Utf8Equals(originNodeName, GB_STR("wms:EX_GeographicBoundingBox"), false))
						{
							westBoundLongitudeNode = FindChildElement(curNode, GB_STR("wms:westBoundLongitude"));
							eastBoundLongitudeNode = FindChildElement(curNode, GB_STR("wms:eastBoundLongitude"));
							southBoundLatitudeNode = FindChildElement(curNode, GB_STR("wms:southBoundLatitude"));
							northBoundLatitudeNode = FindChildElement(curNode, GB_STR("wms:northBoundLatitude"));
						}
						else
						{
							westBoundLongitudeNode = FindChildElement(curNode, GB_STR("westBoundLongitude"));
							eastBoundLongitudeNode = FindChildElement(curNode, GB_STR("eastBoundLongitude"));
							southBoundLatitudeNode = FindChildElement(curNode, GB_STR("southBoundLatitude"));
							northBoundLatitudeNode = FindChildElement(curNode, GB_STR("northBoundLatitude"));
						}
					}
					
					if (westBoundLongitudeNode && eastBoundLongitudeNode && southBoundLatitudeNode && northBoundLatitudeNode)
					{
						double minX = 0, minY = 0, maxX = 0, maxY = 0;
						bool minXOK = false, minYOK = false, maxXOK = false, maxYOK = false;
						try
						{
							minX = GB_ToDouble(GB_Utf8Replace(GetXmlNodeValue(westBoundLongitudeNode), GB_STR(","), GB_STR(".")), 0, &minXOK);
							minY = GB_ToDouble(GB_Utf8Replace(GetXmlNodeValue(southBoundLatitudeNode), GB_STR(","), GB_STR(".")), 0, &minYOK);
							maxX = GB_ToDouble(GB_Utf8Replace(GetXmlNodeValue(eastBoundLongitudeNode), GB_STR(","), GB_STR(".")), 0, &maxXOK);
							maxY = GB_ToDouble(GB_Utf8Replace(GetXmlNodeValue(northBoundLatitudeNode), GB_STR(","), GB_STR(".")), 0, &maxYOK);
						}
						catch (const std::exception& e)
						{
							GBLOG_WARNING(GB_Utf8Format("Failed to parse EX_GeographicBoundingBox values. Error message: %s", e.what()));
						}

						if (minXOK && minYOK && maxXOK && maxYOK)
						{
							layerProperty.exGeographicBBox.Set(minX, minY, maxX, maxY);
						}
						else
						{
							GBLOG_WARNING("One or more EX_GeographicBoundingBox values are invalid.");
						}
					}
					else
					{
						GBLOG_WARNING("EX_GeographicBoundingBox element is missing one or more required child elements (westBoundLongitude, eastBoundLongitude, southBoundLatitude, northBoundLatitude).");
					}
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("BoundingBox"), false))
				{
					GeoBoundingBox boundingBox;
					try
					{
						boundingBox.rect.minX = GB_ToDouble(GB_Utf8Replace(GetXmlNodeAttribute(curNode, GB_STR("minx")), GB_STR(","), GB_STR(".")));
						boundingBox.rect.minY = GB_ToDouble(GB_Utf8Replace(GetXmlNodeAttribute(curNode, GB_STR("miny")), GB_STR(","), GB_STR(".")));
						boundingBox.rect.maxX = GB_ToDouble(GB_Utf8Replace(GetXmlNodeAttribute(curNode, GB_STR("maxx")), GB_STR(","), GB_STR(".")));
						boundingBox.rect.maxY = GB_ToDouble(GB_Utf8Replace(GetXmlNodeAttribute(curNode, GB_STR("maxy")), GB_STR(","), GB_STR(".")));
					}
					catch (const std::exception& e)
					{
						GBLOG_WARNING(GB_Utf8Format("Failed to parse BoundingBox attributes. Error message: %s", e.what()));
					}

					const std::string crsValue = GetXmlNodeAttribute(curNode, GB_STR("CRS"));
					const std::string srsValue = GetXmlNodeAttribute(curNode, GB_STR("SRS"));
					if (!crsValue.empty() || !srsValue.empty())
					{
						if (!crsValue.empty())
						{
							boundingBox.wktUtf8 = GB_ToWkt(crsValue);
						}
						else
						{
							boundingBox.wktUtf8 = GB_ToWkt(srsValue);
						}

						if (ShouldInvertAxisOrder(boundingBox.wktUtf8))
						{
							boundingBox.rect.Set(boundingBox.rect.minY, boundingBox.rect.minX, boundingBox.rect.maxY, boundingBox.rect.maxX);
						}

						bool inheritedOverwritten = false;
						for (GeoBoundingBox& existingBoundingBox : layerProperty.boundingBoxes)
						{
							if (GB_Utf8Equals(existingBoundingBox.wktUtf8, boundingBox.wktUtf8, false))
							{
								existingBoundingBox.rect = boundingBox.rect;
								inheritedOverwritten = true;
							}
						}
						if (!inheritedOverwritten)
						{
							layerProperty.boundingBoxes.push_back(boundingBox);
						}
					}
					else
					{
						GBLOG_WARNING("BoundingBox element is missing both CRS and SRS attributes, so the bounding box will be ignored.");
					}
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("Dimension"), false))
				{
					WmsDimensionProperty dimensionProperty;
					ParseDimension(curNode, dimensionProperty);
					layerProperty.dimensions.push_back(dimensionProperty);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("Extent"), false))
				{
					ParseExtent(curNode, layerProperty.dimensions);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("Attribution"), false))
				{
					// TODO...
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("AuthorityURL"), false))
				{
					// TODO...
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("Identifier"), false))
				{
					// TODO...
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("MetadataURL"), false))
				{
					WmsMetadataUrlProperty metadataUrlProperty;
					ParseMetadataUrl(curNode, metadataUrlProperty);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("DataURL"), false))
				{
					// TODO...
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("FeatureListURL"), false))
				{
					// TODO...
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("Style"), false))
				{
					WmsStyleProperty styleProperty;
					ParseStyle(curNode, styleProperty);
					for (size_t i = 0; i < layerProperty.styles.size(); i++)
					{
						if (layerProperty.styles[i].nameUtf8 == styleProperty.nameUtf8)
						{
							// 如果当前 Layer 已经存在同名的 Style，那么就用新的 Style 替换旧的 Style
							layerProperty.styles.erase(layerProperty.styles.begin() + i);
							break;
						}
					}
					layerProperty.styles.push_back(styleProperty);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("MinScaleDenominator"), false))
				{
					// TODO...
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("MaxScaleDenominator"), false))
				{
					// TODO...
				}
			}

			if (parentProperty)
			{
				layerParentIdMap[layerProperty.orderId] = parentProperty->orderId;
			}

			if (!layerProperty.nameUtf8.empty())
			{
				queryableLayerIdCache[layerProperty.nameUtf8] = layerProperty.queryable;
				layersSupported.push_back(layerProperty);
				if (layerProperty.subLayers.empty())
				{
					layerProperty.styles.clear();
				}
			}

			if (!layerProperty.subLayers.empty())
			{
				layerParentNamesMap[layerProperty.orderId].clear();
				layerParentNamesMap[layerProperty.orderId].push_back(layerProperty.nameUtf8);
				layerParentNamesMap[layerProperty.orderId].push_back(layerProperty.titleUtf8);
				layerParentNamesMap[layerProperty.orderId].push_back(layerProperty.abstractUtf8);
			}
		}

		void ParseDimension(const CPLXMLNode* rootNode, WmsDimensionProperty& dimensionProperty)
		{
			if (!rootNode)
			{
				return;
			}

			dimensionProperty.nameUtf8 = GetXmlNodeAttribute(rootNode, GB_STR("name"));
			dimensionProperty.unitsUtf8 = GetXmlNodeAttribute(rootNode, GB_STR("units"));
			dimensionProperty.unitSymbolUtf8 = GetXmlNodeAttribute(rootNode, GB_STR("unitSymbol"));
			dimensionProperty.defaultValueUtf8 = GetXmlNodeAttribute(rootNode, GB_STR("default"));

			const std::string multipleValuesAttribute = GetXmlNodeAttribute(rootNode, GB_STR("multipleValues"));
			if (!multipleValuesAttribute.empty())
			{
				dimensionProperty.multipleValues = GB_Utf8Equals(multipleValuesAttribute, GB_STR("1"), false) || GB_Utf8Equals(multipleValuesAttribute, GB_STR("true"), false);
			}

			const std::string nearestValueAttribute = GetXmlNodeAttribute(rootNode, GB_STR("nearestValue"));
			if (!nearestValueAttribute.empty())
			{
				dimensionProperty.nearestValue = GB_Utf8Equals(nearestValueAttribute, GB_STR("1"), false) || GB_Utf8Equals(nearestValueAttribute, GB_STR("true"), false);
			}

			const std::string currentAttribute = GetXmlNodeAttribute(rootNode, GB_STR("current"));
			if (!currentAttribute.empty())
			{
				dimensionProperty.current = GB_Utf8Equals(currentAttribute, GB_STR("1"), false) || GB_Utf8Equals(currentAttribute, GB_STR("true"), false);
			}

			dimensionProperty.extentUtf8 = GB_Utf8Simplified(GetXmlNodeValue(rootNode));
		}

		void ParseExtent(const CPLXMLNode* rootNode, std::vector<WmsDimensionProperty>& dimensionProperties)
		{
			if (!rootNode)
			{
				return;
			}

			const std::string name = GetXmlNodeAttribute(rootNode, GB_STR("name"));
			for (WmsDimensionProperty& dimensionProperty : dimensionProperties)
			{
				if (dimensionProperty.nameUtf8 != name)
				{
					continue;
				}

				dimensionProperty.extentUtf8 = GB_Utf8Simplified(GetXmlNodeValue(rootNode));
				dimensionProperty.defaultValueUtf8 = GetXmlNodeAttribute(rootNode, GB_STR("default"));

				const std::string multipleValuesAttribute = GetXmlNodeAttribute(rootNode, GB_STR("multipleValues"));
				if (!multipleValuesAttribute.empty())
				{
					dimensionProperty.multipleValues = GB_Utf8Equals(multipleValuesAttribute, GB_STR("1"), false) || GB_Utf8Equals(multipleValuesAttribute, GB_STR("true"), false);
				}

				const std::string nearestValueAttribute = GetXmlNodeAttribute(rootNode, GB_STR("nearestValue"));
				if (!nearestValueAttribute.empty())
				{
					dimensionProperty.nearestValue = GB_Utf8Equals(nearestValueAttribute, GB_STR("1"), false) || GB_Utf8Equals(nearestValueAttribute, GB_STR("true"), false);
				}

				const std::string currentAttribute = GetXmlNodeAttribute(rootNode, GB_STR("current"));
				if (!currentAttribute.empty())
				{
					dimensionProperty.current = GB_Utf8Equals(currentAttribute, GB_STR("1"), false) || GB_Utf8Equals(currentAttribute, GB_STR("true"), false);
				}
			}
		}

		void ParseMetadataUrl(const CPLXMLNode* rootNode, WmsMetadataUrlProperty& metadataUrlProperty)
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

				if (GB_Utf8Equals(nodeName, GB_STR("Format"), false))
				{
					metadataUrlProperty.formatUtf8 = GetXmlNodeValue(curNode);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("OnlineResource"), false))
				{
					ParseOnlineResource(curNode, metadataUrlProperty.onlineResource);
				}
			}
		}

		void ParseStyle(const CPLXMLNode* rootNode, WmsStyleProperty& styleProperty)
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

				if (GB_Utf8Equals(nodeName, GB_STR("Name"), false))
				{
					styleProperty.nameUtf8 = GetXmlNodeValue(curNode);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("Title"), false))
				{
					styleProperty.titleUtf8 = GetXmlNodeValue(curNode);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("Abstract"), false))
				{
					styleProperty.abstractUtf8 = GetXmlNodeValue(curNode);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("LegendURL"), false))
				{
					WmsLegendUrlProperty legendUrlProperty;
					ParseLegendUrl(curNode, legendUrlProperty);
					styleProperty.legendUrls.push_back(legendUrlProperty);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("StyleSheetURL"), false))
				{
					// TODO...
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("StyleURL"), false))
				{
					// TODO...
				}
			}
		}

		void ParseLegendUrl(const CPLXMLNode* rootNode, WmsLegendUrlProperty& legendUrlProperty)
		{
			if (!rootNode)
			{
				return;
			}

			legendUrlProperty.width = GB_ToULongLong(GetXmlNodeAttribute(rootNode, GB_STR("width")));
			legendUrlProperty.height = GB_ToULongLong(GetXmlNodeAttribute(rootNode, GB_STR("height")));

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

				if (GB_Utf8Equals(nodeName, GB_STR("Format"), false))
				{
					legendUrlProperty.formatUtf8 = GetXmlNodeValue(curNode);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("OnlineResource"), false))
				{
					ParseOnlineResource(curNode, legendUrlProperty.onlineResource);
				}
			}
		}

		void ParseTileSetProfile(const CPLXMLNode* rootNode)
		{
			std::vector<std::string> resolutions, layers, styles;
			GeoBoundingBox boundingBox;
			WmtsTileMatrixSet matrixSet;
			WmtsTileMatrix tileMatrix;
			std::unordered_set<std::string> uniqueFormats;
			std::unordered_set<std::string> uniqueStyles;

			WmtsTileLayer tileLayer;
			tileLayer.dpi = -1;
			tileLayer.timeFormat = WmtsTileLayer::WmtsTimeFormat::YYYYMMDD;
			tileLayer.tileMode = MapTileMode::WMSC;

			if (rootNode)
			{
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

					if (GB_Utf8Equals(nodeName, GB_STR("Layers"), false))
					{
						layers.push_back(GetXmlNodeValue(curNode));
					}
					else if (GB_Utf8Equals(nodeName, GB_STR("Styles"), false))
					{
						const std::string styleValue = GetXmlNodeValue(curNode);
						if (uniqueStyles.find(styleValue) == uniqueStyles.end())
						{
							styles.push_back(styleValue);
							uniqueStyles.insert(styleValue);
						}
					}
					else if (GB_Utf8Equals(nodeName, GB_STR("Width"), false))
					{
						tileMatrix.tileWidth = static_cast<int>(GB_ToUInt(GetXmlNodeValue(curNode)));
					}
					else if (GB_Utf8Equals(nodeName, GB_STR("Height"), false))
					{
						tileMatrix.tileHeight = static_cast<int>(GB_ToUInt(GetXmlNodeValue(curNode)));
					}
					else if (GB_Utf8Equals(nodeName, GB_STR("SRS"), false))
					{
						matrixSet.crsUtf8 = GetXmlNodeValue(curNode);
					}
					else if (GB_Utf8Equals(nodeName, GB_STR("Format"), false))
					{
						const std::string formatValue = GetXmlNodeValue(curNode);
						if (uniqueFormats.find(formatValue) == uniqueFormats.end())
						{
							tileLayer.formats.push_back(formatValue);
							uniqueFormats.insert(formatValue);
						}
					}
					else if (GB_Utf8Equals(nodeName, GB_STR("BoundingBox"), false))
					{
						GeoBoundingBox boundingBox;
						try
						{
							boundingBox.rect.minX = GB_ToDouble(GB_Utf8Replace(GetXmlNodeAttribute(curNode, GB_STR("minx")), GB_STR(","), GB_STR(".")));
							boundingBox.rect.minY = GB_ToDouble(GB_Utf8Replace(GetXmlNodeAttribute(curNode, GB_STR("miny")), GB_STR(","), GB_STR(".")));
							boundingBox.rect.maxX = GB_ToDouble(GB_Utf8Replace(GetXmlNodeAttribute(curNode, GB_STR("maxx")), GB_STR(","), GB_STR(".")));
							boundingBox.rect.maxY = GB_ToDouble(GB_Utf8Replace(GetXmlNodeAttribute(curNode, GB_STR("maxy")), GB_STR(","), GB_STR(".")));
						}
						catch (const std::exception& e)
						{
							GBLOG_WARNING(GB_Utf8Format("Failed to parse BoundingBox attributes in TileSetProfile. Error message: %s", e.what()));
						}

						std::string srsValue = GetXmlNodeAttribute(curNode, GB_STR("SRS"));
						if (srsValue.empty())
						{
							srsValue = GetXmlNodeAttribute(curNode, GB_STR("srs"));
						}

						std::string crsValue = GetXmlNodeAttribute(curNode, GB_STR("CRS"));
						if (crsValue.empty())
						{
							crsValue = GetXmlNodeAttribute(curNode, GB_STR("crs"));
						}

						if (!srsValue.empty() && GeoCrsManager::IsDefinitionValidCached(srsValue))
						{
							boundingBox.wktUtf8 = GB_ToWkt(srsValue);
						}
						else if (!crsValue.empty() && GeoCrsManager::IsDefinitionValidCached(crsValue))
						{
							boundingBox.wktUtf8 = GB_ToWkt(crsValue);
						}
						else
						{
							GBLOG_WARNING("BoundingBox element in TileSetProfile is missing both CRS and SRS attributes, so the bounding box will be ignored.");
						}

						if (!boundingBox.wktUtf8.empty())
						{
							tileLayer.boundingBoxes.push_back(boundingBox);
						}
					}
					else if (GB_Utf8Equals(nodeName, GB_STR("Resolutions"), false))
					{
						resolutions = GB_Utf8Split(GB_Utf8Trim(GetXmlNodeValue(curNode)), GB_CHAR(' '));
					}
				}
			}

			{
				std::string layersNamesCombined = "";
				for (const std::string& layerName : layers)
				{
					layersNamesCombined += layerName;
					layersNamesCombined += GB_CHAR('_');
				}
				if (!layersNamesCombined.empty())
				{
					layersNamesCombined.pop_back();
				}

				matrixSet.identifierUtf8 = layersNamesCombined + GB_STR("-wmsc-") + std::to_string(tileLayersSupported.size());
			}

			{
				std::string layersNamesCombined = "";
				for (const std::string& layerName : layers)
				{
					layersNamesCombined += layerName;
					layersNamesCombined += GB_CHAR(',');
				}
				if (!layersNamesCombined.empty())
				{
					layersNamesCombined.pop_back();
				}
				tileLayer.identifierUtf8 = layersNamesCombined;
			}

			WmtsStyle style;
			{
				std::string styleCombined = "";
				for (const std::string& styleName : styles)
				{
					styleCombined += styleName;
					styleCombined += GB_CHAR(',');
				}
				if (!styleCombined.empty())
				{
					styleCombined.pop_back();
				}
				style.identifierUtf8 = styleCombined;
			}
			tileLayer.styles[style.identifierUtf8] = style;
			tileLayer.defaultStyleUtf8 = style.identifierUtf8;

			WmtsTileMatrixSetLink setLink;
			setLink.tileMatrixSetIdentifierUtf8 = matrixSet.identifierUtf8;
			tileLayer.setLinks[setLink.tileMatrixSetIdentifierUtf8] = setLink;
			tileLayersSupported.push_back(tileLayer);

			for (size_t i = 0; i < resolutions.size(); i++)
			{
				const std::string& resolution = resolutions[i];
				const double resolutionValue = GB_ToDouble(GB_Utf8Replace(resolution, GB_STR(","), GB_STR(".")));
				if (resolutionValue <= 0)
				{
					GBLOG_WARNING(GB_Utf8Format("Invalid resolution '%s' in TileSetProfile. Resolutions must be positive numbers.", resolution.c_str()));
					continue;
				}

				tileMatrix.identifierUtf8 = std::to_string(resolutionValue);
				if (tileLayer.boundingBoxes.size() != 1)
				{
					GBLOG_WARNING("TileSetProfile contains multiple bounding boxes, which is not supported by the current implementation. Only the first bounding box will be used.");
				}
				if (tileLayer.boundingBoxes.empty() || tileMatrix.tileWidth <= 0 || tileMatrix.tileHeight <= 0)
				{
					continue;
				}
				tileMatrix.matrixWidth = static_cast<int>(std::ceil(tileLayer.boundingBoxes[0].rect.Width() / tileMatrix.tileWidth / resolutionValue));
				tileMatrix.matrixHeight = static_cast<int>(std::ceil(tileLayer.boundingBoxes[0].rect.Height() / tileMatrix.tileHeight / resolutionValue));
				tileMatrix.topLeft.Set(tileLayer.boundingBoxes[0].rect.minX, tileLayer.boundingBoxes[0].rect.minY + tileMatrix.matrixHeight * tileMatrix.tileHeight * resolutionValue);
				tileMatrix.tres = resolutionValue;
				matrixSet.tileMatrices[resolutionValue] = tileMatrix;
			}
			tileMatrixSets[matrixSet.identifierUtf8] = matrixSet;
		}

		bool ShouldInvertAxisOrder(const std::string& wkt) const
		{
			bool shouldInvert = false;
			if ((capabilities.versionUtf8 == GB_STR("1.3.0") || capabilities.versionUtf8 == GB_STR("1.3")) && !parserOptions.ignoreAxisOrientation)
			{
				if (GeoCrsManager::IsDefinitionValidCached(wkt) && GeoCrsManager::IsDefinitionAxisOrderReversedCached(wkt))
				{
					shouldInvert = true;
				}
			}

			if (parserOptions.invertAxisOrientation)
			{
				shouldInvert = !shouldInvert;
				GBLOG_INFO(GB_Utf8Format("Axis orientation for CRS '%s' is being inverted due to parser options. Original axis order will be %s.", wkt.c_str(), shouldInvert ? "inverted" : "normal"));
			}
			return shouldInvert;
		}
	};




}







bool ParseWmsCapabilities(const std::string& capabilitiesXmlUtf8, WmsCapabilitiesProperty& outCapabilities, const WmsParserOptions& options)
{
	outCapabilities = WmsCapabilitiesProperty();

	WmsCapabilitiesParser parser;
	return parser.Parse(capabilitiesXmlUtf8, options);
}
