#include "MapWeaverCore.h"
#include "GB_Utf8String.h"
#include "GB_Network.h"
#include "GB_Logger.h"
#include "GeoCrsManager.h"
#include "GeoCrsTransform.h"

#include "cpl_minixml.h"
#include "cpl_error.h"
#include "cpl_string.h"

#include "GB_Crypto.h"

constexpr static double tiandituRenderingPixelSize = 0.0254 / 96;	// 天地图：0.0254 米（1 英寸）除以 96 像素（常见屏幕分辨率）
constexpr static double standardRenderingPixelSize = 0.00028;		// 标准：0.28 毫米（0.00028 米）

enum class RasterIdentifyFormat
{
	Unknown = 0,
	Value = 1,
	Text = 1 << 1,
	Html = 1 << 2,
	Feature = 1 << 3
};

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

static inline std::string ConvertRawBytesToUtf8(const std::string& rawBytes)
{
	try
	{
		const int64_t startPos = GB_Utf8Find(rawBytes, GB_STR("encoding"), false);
		if (startPos < 0)
		{
			GBLOG_WARNING("Failed to find encoding declaration in XML. Defaulting to UTF-8.");
			return rawBytes;
		}

		const int64_t firstPos = GB_Utf8Find(rawBytes, GB_STR("\""), false, startPos + 8);
		if (firstPos < 0 || firstPos - startPos > 20)
		{
			GBLOG_WARNING("Failed to find opening quote for encoding declaration in XML. Defaulting to UTF-8.");
			return rawBytes;
		}

		const int64_t secondPos = GB_Utf8Find(rawBytes, GB_STR("\""), false, firstPos + 1);
		if (secondPos < 0 || secondPos - firstPos > 30)
		{
			GBLOG_WARNING("Failed to find closing quote for encoding declaration in XML. Defaulting to UTF-8.");
			return rawBytes;
		}

		const std::string encoding = GB_Utf8Substr(rawBytes, firstPos + 1, secondPos - firstPos - 1);
		if (GB_Utf8Equals(encoding, GB_STR("utf-8"), false) || GB_Utf8Equals(encoding, GB_STR("utf8"), false))
		{
			return rawBytes;
		}
		return GB_BytesToUtf8(rawBytes, encoding);
	}
	catch (const std::exception& ex)
	{
		GBLOG_WARNING(GB_Utf8Format("Exception while converting raw bytes to UTF-8: %s. Defaulting to UTF-8.", ex.what()));
		return rawBytes;
	}
}


bool DownloadWmsCapabilities(const std::string& rawUrlUtf8, std::string& outCapabilitiesXmlUtf8, const GB_NetworkRequestOptions& options)
{
	std::string urlUtf8 = rawUrlUtf8;
	if (!IsUrlForWMTS(urlUtf8))
	{
		urlUtf8 = GB_UrlOperator::SetUrlQueryValue(urlUtf8, GB_STR("SERVICE"), GB_STR("WMS"));
		urlUtf8 = GB_UrlOperator::SetUrlQueryValue(urlUtf8, GB_STR("REQUEST"), GB_STR("GetCapabilities"));
	}

	GB_NetworkResponse response;
	for (int i = 0; i < 2; i++)
	{
		response = GB_RequestUrlData(urlUtf8, options);
		if (response.ok)
		{
			const std::string rawString = response.body;
			outCapabilitiesXmlUtf8 = ConvertRawBytesToUtf8(rawString);
			return true;
		}
	}
	GBLOG_WARNING(GB_Utf8Format("Failed to download WMS capabilities from URL: '%s'. HTTP status code: %ld. Error message: %s", urlUtf8.c_str(), response.httpStatusCode, response.errorMessageUtf8.c_str()));

	response = GB_RequestUrlData(rawUrlUtf8, options);
	if (response.ok)
	{
		const std::string rawString = response.body;
		outCapabilitiesXmlUtf8 = ConvertRawBytesToUtf8(rawString);
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

		bool Parse(const std::string& capabilitiesXmlUtf8, const WmsParserOptions& options, const std::string& baseUrl)
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
				GBLOG_WARNING("Failed to parse capabilities XML.");
				return false;
			}

			for (const std::string& formatValue : capabilities.capability.request.getFeatureInfo.formatsUtf8)
			{
				RasterIdentifyFormat format = RasterIdentifyFormat::Unknown;
				if (GB_Utf8Equals(formatValue, GB_STR("MIME"), false) || GB_Utf8Equals(formatValue, GB_STR("text/plain"), false))
				{
					format = RasterIdentifyFormat::Text;
				}
				else if (GB_Utf8Equals(formatValue, GB_STR("text/html"), false))
				{
					format = RasterIdentifyFormat::Html;
				}
				else if (GB_Utf8StartsWith(formatValue, GB_STR("GML."), false) || GB_Utf8Equals(formatValue, GB_STR("application/vnd.ogc.gml"), false) || GB_Utf8Equals(formatValue, GB_STR("application/json"), false) ||
					GB_Utf8Equals(formatValue, GB_STR("application/geojson"), false) || GB_Utf8Equals(formatValue, GB_STR("application/geo+json"), false) || GB_Utf8Find(formatValue, GB_STR("gml"), false) > 0 ||
					(GB_Utf8Equals(formatValue, GB_STR("text/xml"), false) && GB_Utf8Find(baseUrl, GB_STR("MapServer"), false) < 0))
				{
					format = RasterIdentifyFormat::Feature;
				}
				identifyFormats[format] = formatValue;
			}
			return true;
		}

		WmsCapabilitiesProperty GetCapabilities() const
		{
			return capabilities;
		}

		std::vector<WmsLayerProperty> GetWmsLayers() const
		{
			return layersSupported;
		}

		std::vector<std::string> GetSupportedImageEncodings() const
		{
			return capabilities.capability.request.getMap.formatsUtf8;
		}

		void GetLayerParents(std::unordered_map<int, int>& parents) const
		{
			parents = layerParentIdMap;
		}

		std::vector<WmtsTileLayer> GetTileLayers() const
		{
			return tileLayersSupported;
		}

		std::unordered_map<std::string, WmtsTileMatrixSet> GetTileMatrixSets() const
		{
			return tileMatrixSets;
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
		std::vector<WmtsTheme> tileThemes;
		std::unordered_map<std::string, WmtsTileMatrixSet> tileMatrixSets;
		std::string firstTileMatrixSetId = "";
		WmsCapabilitiesProperty capabilities;
		std::unordered_map<RasterIdentifyFormat, std::string> identifyFormats;

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
				else if (GB_Utf8Equals(nodeName, GB_STR("Contents"), false))
				{
					ParseWMTSContents(curNode);
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
					const std::string operationName = GetXmlNodeAttribute(curNode, GB_STR("name"));
					CPLXMLNode* dcpNode = FindChildElement(curNode, GB_STR("ows:DCP"));
					if (dcpNode)
					{
						CPLXMLNode* httpNode = FindChildElement(dcpNode, GB_STR("ows:HTTP"));
						if (httpNode)
						{
							CPLXMLNode* getNode = FindChildElement(httpNode, GB_STR("ows:Get"));
							if (getNode)
							{
								const std::string href = GetXmlNodeAttribute(getNode, GB_STR("xlink:href"));
								WmsDcpTypeProperty dcp;
								dcp.http.get.onlineResource.xlinkHrefUtf8 = href;

								WmsOperationType* operationType = nullptr;
								if (!href.empty())
								{
									if (GB_Utf8Equals(operationName, GB_STR("GetTile"), false))
									{
										operationType = &capabilityProperty.request.getTile;
									}
									else if (GB_Utf8Equals(operationName, GB_STR("GetFeatureInfo"), false))
									{
										operationType = &capabilityProperty.request.getFeatureInfo;
									}
									else if (GB_Utf8Equals(operationName, GB_STR("GetLegendGraphic"), false) || GB_Utf8Equals(operationName, GB_STR("sld:GetLegendGraphic"), false))
									{
										operationType = &capabilityProperty.request.getLegendGraphic;
									}
								}

								if (operationType)
								{
									operationType->dcpTypes.push_back(dcp);
									operationType->allowedEncodingsUtf8.clear();

									CPLXMLNode* constraintsNode = FindChildElement(getNode, GB_STR("ows:Constraint"));
									if (constraintsNode)
									{
										CPLXMLNode* allowedValuesNode = FindChildElement(constraintsNode, GB_STR("ows:AllowedValues"));
										if (allowedValuesNode)
										{
											for (CPLXMLNode* valueNode = allowedValuesNode->psChild; valueNode != nullptr; valueNode = valueNode->psNext)
											{
												if (valueNode->eType != CXT_Element)
												{
													continue;
												}

												const std::string valueNodeName = GetXmlNodeTagName(valueNode);
												if (GB_Utf8Equals(valueNodeName, GB_STR("ows:Value"), false))
												{
													const std::string encodingValue = GetXmlNodeValue(valueNode);
													if (!encodingValue.empty())
													{
														operationType->allowedEncodingsUtf8.push_back(encodingValue);
													}
												}
											}
										}
									}
								}
							}
						}
					}
				}
			}

			if (!tileLayersSupported.empty())
			{
				std::unordered_map<std::string, std::string> titles;
				std::unordered_map<std::string, std::string> abstracts;
				for (const WmsLayerProperty& layer : layersSupported)
				{
					if (layer.nameUtf8.empty())
					{
						continue;
					}

					if (!layer.titleUtf8.empty())
					{
						titles[layer.nameUtf8] = layer.titleUtf8;
					}

					if (!layer.abstractUtf8.empty())
					{
						abstracts[layer.nameUtf8] = layer.abstractUtf8;
					}
				}

				for (WmtsTileLayer& tileLayer : tileLayersSupported)
				{
					if (tileLayer.titleUtf8.empty() && titles.find(tileLayer.identifierUtf8) != titles.end())
					{
						tileLayer.titleUtf8 = titles[tileLayer.identifierUtf8];
					}

					if (tileLayer.abstractUtf8.empty() && abstracts.find(tileLayer.identifierUtf8) != abstracts.end())
					{
						tileLayer.abstractUtf8 = abstracts[tileLayer.identifierUtf8];
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

		void ParseKeywords(const CPLXMLNode* rootNode, std::vector<std::string>& keywords)
		{
			if (!rootNode)
			{
				return;
			}

			keywords.clear();

			const CPLXMLNode* keywordsNode = FindChildElement(rootNode, GB_STR("ows:Keywords"));
			if (!keywordsNode)
			{
				return;
			}

			for (CPLXMLNode* keywordNode = keywordsNode->psChild; keywordNode != nullptr; keywordNode = keywordNode->psNext)
			{
				if (keywordNode->eType != CXT_Element)
				{
					continue;
				}

				const std::string keywordNodeName = GetXmlNodeTagName(keywordNode);
				if (!GB_Utf8Equals(keywordNodeName, GB_STR("ows:Keyword"), false))
				{
					continue;
				}

				keywords.push_back(GetXmlNodeValue(keywordNode));
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

		void ParseWMTSContents(const CPLXMLNode* rootNode)
		{
			if (!rootNode)
			{
				return;
			}

			tileMatrixSets.clear();

			for (CPLXMLNode* tileMatrixSetNode = rootNode->psChild; tileMatrixSetNode != nullptr; tileMatrixSetNode = tileMatrixSetNode->psNext)
			{
				if (tileMatrixSetNode->eType != CXT_Element)
				{
					continue;
				}

				const std::string nodeName = GetXmlNodeTagName(tileMatrixSetNode);
				if (!GB_Utf8Equals(nodeName, GB_STR("TileMatrixSet"), false))
				{
					continue;
				}

				WmtsTileMatrixSet set;
				set.identifierUtf8 = GetXmlNodeValue(FindChildElement(tileMatrixSetNode, GB_STR("ows:Identifier")));
				set.titleUtf8 = GetXmlNodeValue(FindChildElement(tileMatrixSetNode, GB_STR("ows:Title")));
				set.abstractUtf8 = GetXmlNodeValue(FindChildElement(tileMatrixSetNode, GB_STR("ows:Abstract")));
				ParseKeywords(tileMatrixSetNode, set.keywordsUtf8);
				set.wkScaleSetUtf8 = GetXmlNodeValue(FindChildElement(tileMatrixSetNode, GB_STR("WellKnownScaleSet")));

				const std::string supportedCrs = GetXmlNodeValue(FindChildElement(tileMatrixSetNode, GB_STR("ows:SupportedCRS")));
				if (supportedCrs.empty())
				{
					GBLOG_WARNING(GB_Utf8Format("No SupportedCRS found in TileMatrixSet '%s'. This TileMatrixSet will be ignored.", set.identifierUtf8.c_str()));
					continue;
				}

				const std::shared_ptr<const GeoCrs> crs = GeoCrsManager::GetFromDefinitionCached(supportedCrs);
				if (!crs)
				{
					GBLOG_WARNING(GB_Utf8Format("Unsupported CRS '%s' found in TileMatrixSet '%s'. This TileMatrixSet will be ignored.", supportedCrs.c_str(), set.identifierUtf8.c_str()));
					continue;
				}

				const double metersPerUnit = crs->GetMetersPerUnit();
				if (metersPerUnit > 0)
				{
					set.crsUtf8 = crs->ToEpsgStringUtf8();
				}
				else
				{
					GBLOG_WARNING(GB_Utf8Format("CRS '%s' in TileMatrixSet '%s' has non-positive meters per unit, which is not supported. This TileMatrixSet will be ignored.", supportedCrs.c_str(), set.identifierUtf8.c_str()));
				}

				if (set.crsUtf8.empty())
				{
					set.crsUtf8 = GB_ToWkt(supportedCrs);
				}

				bool invert = !parserOptions.ignoreAxisOrientation && GeoCrsManager::IsDefinitionAxisOrderReversedCached(set.crsUtf8);
				if (parserOptions.invertAxisOrientation)
				{
					invert = !invert;
				}

				for (CPLXMLNode* tileMatrixNode = tileMatrixSetNode->psChild; tileMatrixNode != nullptr; tileMatrixNode = tileMatrixNode->psNext)
				{
					if (tileMatrixNode->eType != CXT_Element)
					{
						continue;
					}

					const std::string nodeName = GetXmlNodeTagName(tileMatrixNode);
					if (!GB_Utf8Equals(nodeName, GB_STR("TileMatrix"), false))
					{
						continue;
					}

					WmtsTileMatrix tileMatrix;
					tileMatrix.identifierUtf8 = GetXmlNodeValue(FindChildElement(tileMatrixNode, GB_STR("ows:Identifier")));
					tileMatrix.titleUtf8 = GetXmlNodeValue(FindChildElement(tileMatrixNode, GB_STR("ows:Title")));
					tileMatrix.abstractUtf8 = GetXmlNodeValue(FindChildElement(tileMatrixNode, GB_STR("ows:Abstract")));
					ParseKeywords(tileMatrixNode, tileMatrix.keywordsUtf8);
					tileMatrix.scaleDenominator = GB_ToDouble(GetXmlNodeValue(FindChildElement(tileMatrixNode, GB_STR("ScaleDenominator"))));

					const std::string topLeftCornerValue = GetXmlNodeValue(FindChildElement(tileMatrixNode, GB_STR("TopLeftCorner")));
					const std::vector<std::string> topLeftCornerParts = GB_Utf8Split(GB_Utf8Trim(topLeftCornerValue), GB_CHAR(' '));
					if (topLeftCornerParts.size() != 2)
					{
						GBLOG_WARNING(GB_Utf8Format("Invalid TopLeftCorner value '%s' in TileMatrix '%s' of TileMatrixSet '%s'. Expected format is 'x y'.", topLeftCornerValue.c_str(), tileMatrix.identifierUtf8.c_str(), set.identifierUtf8.c_str()));
						continue;
					}
					if (invert)
					{
						tileMatrix.topLeft.Set(GB_ToDouble(GB_Utf8Replace(topLeftCornerParts[1], GB_STR(","), GB_STR("."))), GB_ToDouble(GB_Utf8Replace(topLeftCornerParts[0], GB_STR(","), GB_STR("."))));
					}
					else
					{
						tileMatrix.topLeft.Set(GB_ToDouble(GB_Utf8Replace(topLeftCornerParts[0], GB_STR(","), GB_STR("."))), GB_ToDouble(GB_Utf8Replace(topLeftCornerParts[1], GB_STR(","), GB_STR("."))));
					}

					tileMatrix.tileWidth = static_cast<int>(GB_ToUInt(GetXmlNodeValue(FindChildElement(tileMatrixNode, GB_STR("TileWidth")))));
					tileMatrix.tileHeight = static_cast<int>(GB_ToUInt(GetXmlNodeValue(FindChildElement(tileMatrixNode, GB_STR("TileHeight")))));
					tileMatrix.matrixWidth = static_cast<int>(GB_ToUInt(GetXmlNodeValue(FindChildElement(tileMatrixNode, GB_STR("MatrixWidth")))));
					tileMatrix.matrixHeight = static_cast<int>(GB_ToUInt(GetXmlNodeValue(FindChildElement(tileMatrixNode, GB_STR("MatrixHeight")))));

					const double pixelSize = (IsTianditu() ? tiandituRenderingPixelSize : standardRenderingPixelSize);
					tileMatrix.tres = tileMatrix.scaleDenominator * pixelSize / metersPerUnit;
					set.tileMatrices[tileMatrix.tres] = tileMatrix;
				}

				tileMatrixSets[set.identifierUtf8] = set;
				if (firstTileMatrixSetId.empty())
				{
					firstTileMatrixSetId = set.identifierUtf8;
				}
			}

			tileLayersSupported.clear();
			for (CPLXMLNode* layerNode = rootNode->psChild; layerNode != nullptr; layerNode = layerNode->psNext)
			{
				if (layerNode->eType != CXT_Element)
				{
					continue;
				}

				const std::string nodeName = GetXmlNodeTagName(layerNode);
				if (!GB_Utf8Equals(nodeName, GB_STR("Layer"), false))
				{
					continue;
				}

				WmtsTileLayer tileLayer;
				tileLayer.tileMode = MapTileMode::WMTS;
				tileLayer.identifierUtf8 = GetXmlNodeValue(FindChildElement(layerNode, GB_STR("ows:Identifier")));
				tileLayer.titleUtf8 = GetXmlNodeValue(FindChildElement(layerNode, GB_STR("ows:Title")));
				tileLayer.abstractUtf8 = GetXmlNodeValue(FindChildElement(layerNode, GB_STR("ows:Abstract")));
				ParseKeywords(layerNode, tileLayer.keywordsUtf8);

				GeoBoundingBox boundingBox;
				const CPLXMLNode* wgs84BoundingBoxNode = FindChildElement(layerNode, GB_STR("ows:WGS84BoundingBox"));
				if (wgs84BoundingBoxNode)
				{
					const std::vector<std::string> lowerCornerValue = GB_Utf8Split(GetXmlNodeValue(FindChildElement(wgs84BoundingBoxNode, GB_STR("ows:LowerCorner"))), GB_CHAR(' '));
					const std::vector<std::string> upperCornerValue = GB_Utf8Split(GetXmlNodeValue(FindChildElement(wgs84BoundingBoxNode, GB_STR("ows:UpperCorner"))), GB_CHAR(' '));
					if (lowerCornerValue.size() == 2 && upperCornerValue.size() == 2)
					{
						boundingBox.wktUtf8 = GB_ToWkt("CRS:84");
						boundingBox.rect.Set(GB_ToDouble(GB_Utf8Replace(lowerCornerValue[0], GB_STR(","), GB_STR("."))),
							GB_ToDouble(GB_Utf8Replace(lowerCornerValue[1], GB_STR(","), GB_STR("."))),
							GB_ToDouble(GB_Utf8Replace(upperCornerValue[0], GB_STR(","), GB_STR("."))),
							GB_ToDouble(GB_Utf8Replace(upperCornerValue[1], GB_STR(","), GB_STR("."))));
						tileLayer.boundingBoxes.push_back(boundingBox);
					}
				}

				for (CPLXMLNode* boundingBoxNode = layerNode->psChild; boundingBoxNode != nullptr; boundingBoxNode = boundingBoxNode->psNext)
				{
					if (boundingBoxNode->eType != CXT_Element)
					{
						continue;
					}

					const std::string nodeName = GetXmlNodeTagName(boundingBoxNode);
					if (!GB_Utf8Equals(nodeName, GB_STR("ows:BoundingBox"), false))
					{
						continue;
					}

					boundingBox.Reset();
					const std::vector<std::string> lowerCornerValue = GB_Utf8Split(GetXmlNodeValue(FindChildElement(boundingBoxNode, GB_STR("ows:LowerCorner"))), GB_CHAR(' '));
					const std::vector<std::string> upperCornerValue = GB_Utf8Split(GetXmlNodeValue(FindChildElement(boundingBoxNode, GB_STR("ows:UpperCorner"))), GB_CHAR(' '));
					if (lowerCornerValue.size() != 2 || upperCornerValue.size() != 2)
					{
						GBLOG_WARNING(GB_Utf8Format("Invalid LowerCorner or UpperCorner value in BoundingBox of Layer '%s'. Expected format is 'x y'. This bounding box will be ignored.", tileLayer.identifierUtf8.c_str()));
						continue;
					}

					boundingBox.rect.Set(GB_ToDouble(GB_Utf8Replace(lowerCornerValue[0], GB_STR(","), GB_STR("."))),
						GB_ToDouble(GB_Utf8Replace(lowerCornerValue[1], GB_STR(","), GB_STR("."))),
						GB_ToDouble(GB_Utf8Replace(upperCornerValue[0], GB_STR(","), GB_STR("."))),
						GB_ToDouble(GB_Utf8Replace(upperCornerValue[1], GB_STR(","), GB_STR("."))));

					std::string srsValue = GetXmlNodeAttribute(boundingBoxNode, GB_STR("SRS"));
					if (srsValue.empty())
					{
						srsValue = GetXmlNodeAttribute(boundingBoxNode, GB_STR("srs"));
					}

					std::string crsValue = GetXmlNodeAttribute(boundingBoxNode, GB_STR("CRS"));
					if (crsValue.empty())
					{
						crsValue = GetXmlNodeAttribute(boundingBoxNode, GB_STR("crs"));
					}

					if (!srsValue.empty() && GeoCrsManager::IsDefinitionValidCached(srsValue))
					{
						boundingBox.wktUtf8 = GB_ToWkt(srsValue);
					}
					else if (!crsValue.empty() && GeoCrsManager::IsDefinitionValidCached(crsValue))
					{
						boundingBox.wktUtf8 = GB_ToWkt(crsValue);
					}
					if (boundingBox.wktUtf8.empty())
					{
						GBLOG_WARNING(GB_Utf8Format("BoundingBox element in Layer '%s' is missing both CRS and SRS attributes, so the bounding box will be ignored.", tileLayer.identifierUtf8.c_str()));
						continue;
					}

					bool invert = !parserOptions.ignoreAxisOrientation && GeoCrsManager::IsDefinitionAxisOrderReversedCached(boundingBox.wktUtf8);
					if (parserOptions.invertAxisOrientation)
					{
						invert = !invert;
					}
					if (invert)
					{
						boundingBox.rect.Set(boundingBox.rect.minY, boundingBox.rect.minX, boundingBox.rect.maxY, boundingBox.rect.maxX);
					}
					tileLayer.boundingBoxes.push_back(boundingBox);
				}

				for (CPLXMLNode* styleNode = layerNode->psChild; styleNode != nullptr; styleNode = styleNode->psNext)
				{
					if (styleNode->eType != CXT_Element)
					{
						continue;
					}

					const std::string nodeName = GetXmlNodeTagName(styleNode);
					if (!GB_Utf8Equals(nodeName, GB_STR("Style"), false))
					{
						continue;
					}

					WmtsStyle style;
					style.identifierUtf8 = GetXmlNodeValue(FindChildElement(styleNode, GB_STR("ows:Identifier")));
					style.titleUtf8 = GetXmlNodeValue(FindChildElement(styleNode, GB_STR("ows:Title")));
					style.abstractUtf8 = GetXmlNodeValue(FindChildElement(styleNode, GB_STR("ows:Abstract")));
					ParseKeywords(styleNode, style.keywordsUtf8);

					for (CPLXMLNode* legendUrlNode = styleNode->psChild; legendUrlNode != nullptr; legendUrlNode = legendUrlNode->psNext)
					{
						if (legendUrlNode->eType != CXT_Element)
						{
							continue;
						}
						const std::string nodeName = GetXmlNodeTagName(legendUrlNode);
						if (!GB_Utf8Equals(nodeName, GB_STR("ows:legendURL"), false))
						{
							continue;
						}

						WmtsLegendUrl legendUrl;
						legendUrl.formatUtf8 = GetXmlNodeValue(FindChildElement(legendUrlNode, GB_STR("format")));
						legendUrl.minScale = GB_ToDouble(GetXmlNodeValue(FindChildElement(legendUrlNode, GB_STR("minScale"))));
						legendUrl.maxScale = GB_ToDouble(GetXmlNodeValue(FindChildElement(legendUrlNode, GB_STR("maxScale"))));
						legendUrl.hrefUtf8 = GetXmlNodeValue(FindChildElement(legendUrlNode, GB_STR("href")));
						legendUrl.width = static_cast<int>(GB_ToUInt(GetXmlNodeValue(FindChildElement(legendUrlNode, GB_STR("width")))));
						legendUrl.height = static_cast<int>(GB_ToUInt(GetXmlNodeValue(FindChildElement(legendUrlNode, GB_STR("height")))));
						style.legendUrls.push_back(legendUrl);
					}

					CPLXMLNode* legendUrlNode = FindChildElement(styleNode, GB_STR("LegendURL"));
					if (legendUrlNode)
					{
						WmtsLegendUrl legendUrl;
						legendUrl.formatUtf8 = GetXmlNodeAttribute(legendUrlNode, GB_STR("format"));
						legendUrl.minScale = GB_ToDouble(GetXmlNodeAttribute(legendUrlNode, GB_STR("minScaleDenominator")));
						legendUrl.maxScale = GB_ToDouble(GetXmlNodeAttribute(legendUrlNode, GB_STR("maxScaleDenominator")));
						legendUrl.hrefUtf8 = GetXmlNodeAttribute(legendUrlNode, GB_STR("xlink:href"));
						legendUrl.width = static_cast<int>(GB_ToUInt(GetXmlNodeAttribute(legendUrlNode, GB_STR("width"))));
						legendUrl.height = static_cast<int>(GB_ToUInt(GetXmlNodeAttribute(legendUrlNode, GB_STR("height"))));
						style.legendUrls.push_back(legendUrl);
					}

					style.isDefault = GB_Utf8Equals(GetXmlNodeAttribute(styleNode, GB_STR("isDefault")), GB_STR("true"), false);

					tileLayer.styles[style.identifierUtf8] = style;
					if (style.isDefault)
					{
						tileLayer.defaultStyleUtf8 = style.identifierUtf8;
					}
				}

				if (tileLayer.styles.empty())
				{
					GBLOG_WARNING(GB_Utf8Format("No styles found for Layer '%s' in WMTS capabilities. A default style has been generated, but the layer may not render correctly.", tileLayer.identifierUtf8.c_str()));

					WmtsStyle defaultStyle;
					defaultStyle.identifierUtf8 = GB_STR("default");
					defaultStyle.titleUtf8 = GB_STR("Generated default style");
					defaultStyle.abstractUtf8 = GB_STR("This style was automatically generated because no styles were defined for this layer in the WMTS capabilities.");
					tileLayer.styles[defaultStyle.identifierUtf8] = defaultStyle;
				}

				{
					std::unordered_set<std::string> uniqueFormats;
					for (CPLXMLNode* formatNode = layerNode->psChild; formatNode != nullptr; formatNode = formatNode->psNext)
					{
						if (formatNode->eType != CXT_Element)
						{
							continue;
						}

						const std::string nodeName = GetXmlNodeTagName(formatNode);
						if (!GB_Utf8Equals(nodeName, GB_STR("Format"), false))
						{
							continue;
						}

						const std::string formatValue = GetXmlNodeValue(formatNode);
						if (!formatValue.empty() && uniqueFormats.find(formatValue) == uniqueFormats.end())
						{
							tileLayer.formats.push_back(formatValue);
							uniqueFormats.insert(formatValue);
						}
					}
				}

				for (CPLXMLNode* infoFormatNode = layerNode->psChild; infoFormatNode != nullptr; infoFormatNode = infoFormatNode->psNext)
				{
					if (infoFormatNode->eType != CXT_Element)
					{
						continue;
					}

					const std::string nodeName = GetXmlNodeTagName(infoFormatNode);
					if (!GB_Utf8Equals(nodeName, GB_STR("InfoFormat"), false))
					{
						continue;
					}

					const std::string infoFormatValue = GetXmlNodeValue(infoFormatNode);
					if (!infoFormatValue.empty())
					{
						tileLayer.infoFormats.push_back(infoFormatValue);
					}

					RasterIdentifyFormat identifyFormat = RasterIdentifyFormat::Unknown;
					if (GB_Utf8Equals(infoFormatValue, GB_STR("MIME"), false) || GB_Utf8Equals(infoFormatValue, GB_STR("text/plain"), false))
					{
						identifyFormat = RasterIdentifyFormat::Text;
					}
					else if (GB_Utf8Equals(infoFormatValue, GB_STR("text/html"), false))
					{
						identifyFormat = RasterIdentifyFormat::Html;
					}
					else if (GB_Utf8StartsWith(infoFormatValue, GB_STR("GML."), false) || GB_Utf8Equals(infoFormatValue, GB_STR("application/vnd.ogc.gml"), false) ||
						GB_Utf8Find(infoFormatValue, GB_STR("gml"), false) >= 0 || GB_Utf8Equals(infoFormatValue, GB_STR("application/json"), false) ||
						GB_Utf8Equals(infoFormatValue, GB_STR("application/geojson"), false) || GB_Utf8Equals(infoFormatValue, GB_STR("application/geo+json"), false))
					{
						identifyFormat = RasterIdentifyFormat::Feature;
					}
					else
					{
						continue;
					}
					identifyFormats[identifyFormat] = infoFormatValue;
				}

				for (CPLXMLNode* dimensionNode = layerNode->psChild; dimensionNode != nullptr; dimensionNode = dimensionNode->psNext)
				{
					if (dimensionNode->eType != CXT_Element)
					{
						continue;
					}

					const std::string nodeName = GetXmlNodeTagName(dimensionNode);
					if (!GB_Utf8Equals(nodeName, GB_STR("Dimension"), false))
					{
						continue;
					}

					WmtsDimension dimension;
					dimension.identifierUtf8 = GetXmlNodeValue(FindChildElement(dimensionNode, GB_STR("ows:Identifier")));
					if (dimension.identifierUtf8.empty())
					{
						continue;
					}

					dimension.titleUtf8 = GetXmlNodeValue(FindChildElement(dimensionNode, GB_STR("ows:Title")));
					dimension.abstractUtf8 = GetXmlNodeValue(FindChildElement(dimensionNode, GB_STR("ows:Abstract")));
					ParseKeywords(dimensionNode, dimension.keywordsUtf8);
					dimension.unitOfMeasureUtf8 = GetXmlNodeValue(FindChildElement(dimensionNode, GB_STR("UOM")));
					dimension.unitSymbolUtf8 = GetXmlNodeValue(FindChildElement(dimensionNode, GB_STR("unitSymbol")));
					dimension.defaultValueUtf8 = GetXmlNodeValue(FindChildElement(dimensionNode, GB_STR("Default")));
					dimension.current = GB_Utf8Equals(GetXmlNodeValue(FindChildElement(dimensionNode, GB_STR("current"))), GB_STR("true"), false);
					for (CPLXMLNode* dimensionValueNode = dimensionNode->psChild; dimensionValueNode != nullptr; dimensionValueNode = dimensionValueNode->psNext)
					{
						if (dimensionValueNode->eType != CXT_Element)
						{
							continue;
						}

						const std::string nodeName = GetXmlNodeTagName(dimensionValueNode);
						if (!GB_Utf8Equals(nodeName, GB_STR("Value"), false))
						{
							continue;
						}
						dimension.values.push_back(GetXmlNodeValue(dimensionValueNode));
					}

					tileLayer.dimensions[dimension.identifierUtf8] = dimension;

					// TODO...
				}

				for (CPLXMLNode* setLinkNode = layerNode->psChild; setLinkNode != nullptr; setLinkNode = setLinkNode->psNext)
				{
					if (setLinkNode->eType != CXT_Element)
					{
						continue;
					}

					const std::string nodeName = GetXmlNodeTagName(setLinkNode);
					if (!GB_Utf8Equals(nodeName, GB_STR("TileMatrixSetLink"), false))
					{
						continue;
					}

					WmtsTileMatrixSetLink setLink;
					setLink.tileMatrixSetIdentifierUtf8 = GetXmlNodeValue(FindChildElement(setLinkNode, GB_STR("TileMatrixSet")));
					if (tileMatrixSets.find(setLink.tileMatrixSetIdentifierUtf8) == tileMatrixSets.end())
					{
						GBLOG_WARNING(GB_Utf8Format("TileMatrixSet '%s' referenced in TileMatrixSetLink of Layer '%s' not found. This set link will be ignored.", setLink.tileMatrixSetIdentifierUtf8.c_str(), tileLayer.identifierUtf8.c_str()));
						continue;
					}

					const WmtsTileMatrixSet& tileMatrixSet = tileMatrixSets[setLink.tileMatrixSetIdentifierUtf8];
					for (CPLXMLNode* setLimitsNode = setLinkNode->psChild; setLimitsNode != nullptr; setLimitsNode = setLimitsNode->psNext)
					{
						if (setLimitsNode->eType != CXT_Element)
						{
							continue;
						}

						const std::string nodeName = GetXmlNodeTagName(setLimitsNode);
						if (!GB_Utf8Equals(nodeName, GB_STR("TileMatrixSetLimits"), false))
						{
							continue;
						}

						for (CPLXMLNode* matrixLimitsNode = setLimitsNode->psChild; matrixLimitsNode != nullptr; matrixLimitsNode = matrixLimitsNode->psNext)
						{
							if (matrixLimitsNode->eType != CXT_Element)
							{
								continue;
							}

							const std::string nodeName = GetXmlNodeTagName(matrixLimitsNode);
							if (!GB_Utf8Equals(nodeName, GB_STR("TileMatrixLimits"), false))
							{
								continue;
							}

							const std::string id = GetXmlNodeValue(FindChildElement(matrixLimitsNode, GB_STR("TileMatrix")));
							bool isValid = false;
							int matrixWidth = -1, matrixHeight = -1;
							for (const auto& kvp : tileMatrixSet.tileMatrices)
							{
								if (kvp.second.identifierUtf8 == id)
								{
									isValid = true;
									matrixWidth = kvp.second.matrixWidth;
									matrixHeight = kvp.second.matrixHeight;
									break;
								}
							}
							if (!isValid)
							{
								GBLOG_WARNING(GB_Utf8Format("TileMatrix '%s' referenced in TileMatrixLimits of Layer '%s' and TileMatrixSet '%s' not found. This TileMatrixLimits will be ignored.", id.c_str(), tileLayer.identifierUtf8.c_str(), tileMatrixSet.identifierUtf8.c_str()));
								continue;
							}

							WmtsTileMatrixLimits matrixLimits;
							matrixLimits.rowIndexInterval.lower = static_cast<int>(GB_ToUInt(GetXmlNodeValue(FindChildElement(matrixLimitsNode, GB_STR("MinTileRow")))));
							matrixLimits.rowIndexInterval.upper = static_cast<int>(GB_ToUInt(GetXmlNodeValue(FindChildElement(matrixLimitsNode, GB_STR("MaxTileRow")))));
							matrixLimits.colIndexInterval.lower = static_cast<int>(GB_ToUInt(GetXmlNodeValue(FindChildElement(matrixLimitsNode, GB_STR("MinTileCol")))));
							matrixLimits.colIndexInterval.upper = static_cast<int>(GB_ToUInt(GetXmlNodeValue(FindChildElement(matrixLimitsNode, GB_STR("MaxTileCol")))));
							isValid = (matrixLimits.colIndexInterval.lower >= 0 && matrixLimits.colIndexInterval.lower < matrixWidth && matrixLimits.colIndexInterval.upper >= 0 && matrixLimits.colIndexInterval.upper < matrixWidth &&
								matrixLimits.rowIndexInterval.lower >= 0 && matrixLimits.rowIndexInterval.lower < matrixHeight && matrixLimits.rowIndexInterval.upper >= 0 && matrixLimits.rowIndexInterval.upper < matrixHeight &&
								matrixLimits.colIndexInterval.lower <= matrixLimits.colIndexInterval.upper && matrixLimits.rowIndexInterval.lower <= matrixLimits.rowIndexInterval.upper);
							if (!isValid)
							{
								GBLOG_WARNING(GB_Utf8Format("Invalid TileMatrixLimits for TileMatrix '%s' in Layer '%s' and TileMatrixSet '%s'. Col index intervals must be within [0, %d] and row index intervals must be within [0, %d], and lower bounds must be less than or equal to upper bounds. This TileMatrixLimits will be ignored.", id.c_str(), tileLayer.identifierUtf8.c_str(), tileMatrixSet.identifierUtf8.c_str(), matrixWidth - 1, matrixHeight - 1));
								continue;
							}
							setLink.limits[id] = matrixLimits;
						}
					}
					tileLayer.setLinks[setLink.tileMatrixSetIdentifierUtf8] = setLink;
				}

				for (CPLXMLNode* resourceUrlNode = layerNode->psChild; resourceUrlNode != nullptr; resourceUrlNode = resourceUrlNode->psNext)
				{
					if (resourceUrlNode->eType != CXT_Element)
					{
						continue;
					}

					const std::string nodeName = GetXmlNodeTagName(resourceUrlNode);
					if (!GB_Utf8Equals(nodeName, GB_STR("ResourceURL"), false))
					{
						continue;
					}

					const std::string format = GetXmlNodeAttribute(resourceUrlNode, GB_STR("format"));
					const std::string resourceType = GetXmlNodeAttribute(resourceUrlNode, GB_STR("resourceType"));
					const std::string tmpl = GetXmlNodeAttribute(resourceUrlNode, GB_STR("template"));
					if (format.empty() || resourceType.empty() || tmpl.empty())
					{
						GBLOG_WARNING(GB_Utf8Format("ResourceURL element in Layer '%s' is missing required attributes. All of 'format', 'resourceType', and 'template' attributes are required. This ResourceURL will be ignored.", tileLayer.identifierUtf8.c_str()));
						continue;
					}

					if (GB_Utf8Equals(resourceType, GB_STR("tile"), false))
					{
						tileLayer.getTileUrls[format] = tmpl;
					}
					else if (GB_Utf8Equals(resourceType, GB_STR("FeatureInfo"), false))
					{
						tileLayer.getFeatureInfoUrls[format] = tmpl;

						RasterIdentifyFormat identifyFormat = RasterIdentifyFormat::Unknown;
						if (GB_Utf8Equals(format, GB_STR("MIME"), false) || GB_Utf8Equals(format, GB_STR("text/plain"), false))
						{
							identifyFormat = RasterIdentifyFormat::Text;
						}
						else if (GB_Utf8Equals(format, GB_STR("text/html"), false))
						{
							identifyFormat = RasterIdentifyFormat::Html;
						}
						else if (GB_Utf8StartsWith(format, GB_STR("GML."), false) || GB_Utf8Equals(format, GB_STR("application/vnd.ogc.gml"), false) ||
							GB_Utf8Find(format, GB_STR("gml"), false) >= 0 || GB_Utf8Equals(format, GB_STR("application/json"), false) ||
							GB_Utf8Equals(format, GB_STR("application/geojson"), false) || GB_Utf8Equals(format, GB_STR("application/geo+json"), false))
						{
							identifyFormat = RasterIdentifyFormat::Feature;
						}
						else
						{
							continue;
						}
						identifyFormats[identifyFormat] = format;
					}
					else
					{
						continue;
					}
				}

				tileLayersSupported.push_back(tileLayer);
			}

			tileThemes.clear();
			for (CPLXMLNode* themeNode = rootNode->psChild; themeNode != nullptr; themeNode = themeNode->psNext)
			{
				if (themeNode->eType != CXT_Element)
				{
					continue;
				}

				const std::string nodeName = GetXmlNodeTagName(themeNode);
				if (!GB_Utf8Equals(nodeName, GB_STR("Theme"), false))
				{
					continue;
				}

				WmtsTheme theme;
				ParseTheme(themeNode, theme);
				tileThemes.push_back(theme);
			}

			for (WmtsTileLayer& tileLayer : tileLayersSupported)
			{
				if (tileLayer.boundingBoxes.empty())
				{
					if (!DetectTileLayerBoundingBox(tileLayer))
					{
						GBLOG_WARNING(GB_Utf8Format("No bounding box found for Layer '%s'. This layer may not render correctly.", tileLayer.identifierUtf8.c_str()));
						const static GeoBoundingBox defaultBoundingBox(GB_ToWkt("CRS:84"), GB_Rectangle(-180, -90, 180, 90));
						tileLayer.boundingBoxes.push_back(defaultBoundingBox);
					}
				}
			}
		}

		void ParseTheme(const CPLXMLNode* rootNode, WmtsTheme& theme)
		{
			theme.identifierUtf8 = GetXmlNodeValue(FindChildElement(rootNode, GB_STR("ows:Identifier")));
			theme.titleUtf8 = GetXmlNodeValue(FindChildElement(rootNode, GB_STR("ows:Title")));
			theme.abstractUtf8 = GetXmlNodeValue(FindChildElement(rootNode, GB_STR("ows:Abstract")));
			ParseKeywords(rootNode, theme.keywordsUtf8);

			const CPLXMLNode* themeNode = FindChildElement(rootNode, GB_STR("ows:Theme"));
			if (themeNode)
			{
				theme.subTheme = new WmtsTheme();
				ParseTheme(themeNode, *theme.subTheme);
			}
			else
			{
				theme.subTheme = nullptr;
			}

			theme.layerRefs.clear();
			for (CPLXMLNode* layerRefNode = rootNode->psChild; layerRefNode != nullptr; layerRefNode = layerRefNode->psNext)
			{
				if (layerRefNode->eType != CXT_Element)
				{
					continue;
				}

				const std::string nodeName = GetXmlNodeTagName(layerRefNode);
				if (!GB_Utf8Equals(nodeName, GB_STR("ows:LayerRef"), false))
				{
					continue;
				}

				theme.layerRefs.push_back(GetXmlNodeValue(layerRefNode));
			}
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

		bool IsTianditu() const
		{
			if (capabilities.capability.request.getTile.dcpTypes.empty())
			{
				return false;
			}

			const std::string& dcpType = capabilities.capability.request.getTile.dcpTypes[0].http.get.onlineResource.xlinkHrefUtf8;
			return GB_Utf8Find(dcpType, GB_STR("tianditu"), false) > 0;
		}

		bool DetectTileLayerBoundingBox(WmtsTileLayer& tileLayer)
		{
			if (tileLayer.setLinks.empty())
			{
				return false;
			}

			for (const auto& kvp : tileLayer.setLinks)
			{
				const WmtsTileMatrixSetLink& setLink = kvp.second;
				const auto it = tileMatrixSets.find(setLink.tileMatrixSetIdentifierUtf8);
				if (it == tileMatrixSets.end())
				{
					continue;
				}

				const std::shared_ptr<const GeoCrs> crs = GeoCrsManager::GetFromDefinitionCached(it->second.crsUtf8);
				if (!GeoCrsManager::IsDefinitionValidCached(it->second.crsUtf8) || !crs)
				{
					continue;
				}

				auto lastIt = it->second.tileMatrices.end();
				lastIt--;
				if (lastIt == it->second.tileMatrices.end())
				{
					continue;
				}

				const WmtsTileMatrix& tileMatrix = lastIt->second;
				const double metersPerUnit = crs->GetMetersPerUnit();
				const double pixelSize = (IsTianditu() ? tiandituRenderingPixelSize : standardRenderingPixelSize);
				const double resolution = tileMatrix.scaleDenominator * pixelSize / metersPerUnit;
				const GB_Point2d bottomRight(tileMatrix.topLeft.x + resolution * tileMatrix.tileWidth * tileMatrix.matrixWidth, tileMatrix.topLeft.y - resolution * tileMatrix.tileHeight * tileMatrix.matrixHeight);
				GB_Rectangle extent(tileMatrix.topLeft, bottomRight);
				extent.Normalize();

				const GeoBoundingBox boundingBoxProperty(crs->ExportToWktUtf8(), extent);
				tileLayer.boundingBoxes.push_back(boundingBoxProperty);
			}

			return !tileLayer.boundingBoxes.empty();
		}
	};
}

bool ParseWmsCapabilities(const std::string& capabilitiesXmlUtf8, const std::string& baseUrl, WmsCapabilitiesProperty& outCapabilities, const WmsParserOptions& options)
{
	outCapabilities = WmsCapabilitiesProperty();

	WmsCapabilitiesParser parser;
	if (!parser.Parse(capabilitiesXmlUtf8, options, baseUrl))
	{
		return false;
	}

	outCapabilities = parser.GetCapabilities();
	outCapabilities.capability.layers = outCapabilities.capability.layers;
	outCapabilities.capability.tileLayers = parser.GetTileLayers();
	outCapabilities.capability.tileMatrixSets = parser.GetTileMatrixSets();
	parser.GetLayerParents(outCapabilities.capability.layerParents);
	return true;
}

namespace
{
	struct SerializeFrame
	{
		const WmsTreeNode* node = nullptr;
		size_t depth = 0;
	};

	static size_t CalculateSerializedLength(const WmsTreeNode& rootNode, size_t indentUnitLength)
	{
		size_t totalLength = 0;
		bool isFirstLine = true;

		std::vector<SerializeFrame> stack;
		stack.reserve(64);
		stack.push_back({ &rootNode, 0 });

		while (!stack.empty())
		{
			const SerializeFrame frame = stack.back();
			stack.pop_back();

			if (!isFirstLine)
			{
				totalLength += 1; // '\n'
			}
			isFirstLine = false;

			totalLength += frame.depth * indentUnitLength;
			totalLength += frame.node->textUtf8.size();

			const size_t numChildren = frame.node->children.size();
			for (size_t i = numChildren; i > 0; i--)
			{
				stack.push_back({ &frame.node->children[i - 1], frame.depth + 1 });
			}
		}

		return totalLength;
	}

	static void AppendIndent(std::string& output, const std::string& indentString, size_t depth)
	{
		if (indentString.empty() || depth == 0)
		{
			return;
		}

		for (size_t i = 0; i < depth; i++)
		{
			output.append(indentString);
		}
	}
}

std::string WmsTreeNode::ToString(const std::string& indentStringUtf8) const
{
	const size_t totalLength = CalculateSerializedLength(*this, indentStringUtf8.size());

	std::string output;
	output.reserve(totalLength);

	bool isFirstLine = true;

	std::vector<SerializeFrame> stack;
	stack.reserve(64);
	stack.push_back({ this, 0 });

	while (!stack.empty())
	{
		const SerializeFrame frame = stack.back();
		stack.pop_back();

		if (!isFirstLine)
		{
			output.push_back('\n');
		}
		isFirstLine = false;

		AppendIndent(output, indentStringUtf8, frame.depth);
		output.append(frame.node->textUtf8);

		const size_t numChildren = frame.node->children.size();
		for (size_t i = numChildren; i > 0; i--)
		{
			stack.push_back({ &frame.node->children[i - 1], frame.depth + 1 });
		}
	}
	return output;
}

namespace
{
	static std::vector<WmsTreeNode> CreateTileLayerFormatNodes(const WmtsTileLayer& tileLayer, const std::string& path)
	{
		std::vector<WmsTreeNode> formatNodes;
		formatNodes.reserve(tileLayer.formats.size());
		for (const std::string& format : tileLayer.formats)
		{
			WmsTreeNode formatNode;
			formatNode.textUtf8 = format;
			formatNode.nodeType = WmsTreeNode::NodeType::Format;
			formatNode.uidUtf8 = GB_Md5Hash(path + "|" + format);
			formatNodes.push_back(std::move(formatNode));
		}
		std::sort(formatNodes.begin(), formatNodes.end(), [](const WmsTreeNode& a, const WmsTreeNode& b) {
			return GB_Utf8CompareLogical(a.textUtf8, b.textUtf8) < 0;
		});
		return formatNodes;
	}

	static std::vector<WmsTreeNode> CreateTileLayerTileMatrixSetNodes(const WmtsTileLayer& tileLayer, const BuildLayerTreeOptions& options, const std::string& path)
	{
		std::vector<WmsTreeNode> tileMatrixSetNodes;
		tileMatrixSetNodes.reserve(tileLayer.setLinks.size());
		for (const auto& kvp : tileLayer.setLinks)
		{
			WmsTreeNode tileMatrixSetNode;
			tileMatrixSetNode.textUtf8 = kvp.first;
			tileMatrixSetNode.nodeType = WmsTreeNode::NodeType::WmtsTileMatrixSet;

			const std::string currentPath = path + "|" + kvp.first;
			tileMatrixSetNode.uidUtf8 = GB_Md5Hash(currentPath);
			if (!options.ignoreUniqueChildNode || tileLayer.formats.size() >= 2)
			{
				tileMatrixSetNode.children = CreateTileLayerFormatNodes(tileLayer, currentPath);
			}
			tileMatrixSetNodes.push_back(std::move(tileMatrixSetNode));
		}
		std::sort(tileMatrixSetNodes.begin(), tileMatrixSetNodes.end(), [](const WmsTreeNode& a, const WmsTreeNode& b) {
			return GB_Utf8CompareLogical(a.textUtf8, b.textUtf8) < 0;
		});
		return tileMatrixSetNodes;
	}

	static std::vector<WmsTreeNode> CreateTileLayerStyleNodes(const WmtsTileLayer& tileLayer, const BuildLayerTreeOptions& options, const std::string& path)
	{
		std::vector<WmsTreeNode> styleNodes;
		styleNodes.reserve(tileLayer.styles.size());
		for (const auto& kvp : tileLayer.styles)
		{
			const WmtsStyle& style = kvp.second;
			WmsTreeNode styleNode;
			styleNode.textUtf8 = style.titleUtf8.empty() ? style.identifierUtf8 : style.titleUtf8;
			styleNode.nodeType = WmsTreeNode::NodeType::Style;

			const std::string currentPath = path + "|" + kvp.first;
			styleNode.uidUtf8 = GB_Md5Hash(currentPath);
			if (options.ignoreUniqueChildNode)
			{
				if (tileLayer.setLinks.size() >= 2)
				{
					styleNode.children = CreateTileLayerTileMatrixSetNodes(tileLayer, options, currentPath);
				}
				else if (tileLayer.formats.size() >= 2)
				{
					styleNode.children = CreateTileLayerFormatNodes(tileLayer, currentPath);
				}
			}
			else
			{
				styleNode.children = CreateTileLayerTileMatrixSetNodes(tileLayer, options, currentPath);
			}
			styleNodes.push_back(std::move(styleNode));
		}
		std::sort(styleNodes.begin(), styleNodes.end(), [](const WmsTreeNode& a, const WmsTreeNode& b) {
			return GB_Utf8CompareLogical(a.textUtf8, b.textUtf8) < 0;
		});
		return styleNodes;
	}

	static WmsTreeNode CreateTileLayerNode(const WmtsTileLayer& tileLayer, const BuildLayerTreeOptions& options, const std::string& path)
	{
		WmsTreeNode layerNode;
		layerNode.textUtf8 = tileLayer.titleUtf8.empty() ? tileLayer.identifierUtf8 : tileLayer.titleUtf8;
		layerNode.nodeType = WmsTreeNode::NodeType::Layer;

		const std::string currentPath = path + "|" + tileLayer.identifierUtf8;
		layerNode.uidUtf8 = GB_Md5Hash(currentPath);

		if (options.ignoreUniqueChildNode)
		{
			if (tileLayer.styles.size() >= 2)
			{
				layerNode.children = CreateTileLayerStyleNodes(tileLayer, options, currentPath);
			}
			else if (tileLayer.setLinks.size() >= 2)
			{
				layerNode.children = CreateTileLayerTileMatrixSetNodes(tileLayer, options, currentPath);
			}
			else if (tileLayer.formats.size() >= 2)
			{
				layerNode.children = CreateTileLayerFormatNodes(tileLayer, currentPath);
			}
		}
		else
		{
			layerNode.children = CreateTileLayerStyleNodes(tileLayer, options, currentPath);
		}
		std::sort(layerNode.children.begin(), layerNode.children.end(), [](const WmsTreeNode& a, const WmsTreeNode& b) {
			return GB_Utf8CompareLogical(a.textUtf8, b.textUtf8) < 0;
		});
		return layerNode;
	}

	static WmsTreeNode CreateWmsLayerNode(const WmsLayerProperty& layer, const std::string& path)
	{
		WmsTreeNode layerNode;
		layerNode.textUtf8 = layer.titleUtf8.empty() ? (layer.nameUtf8.empty() ? std::to_string(layer.orderId) : layer.nameUtf8) : layer.titleUtf8;
		layerNode.nodeType = WmsTreeNode::NodeType::Layer;
		const std::string currentPath = path + "|" + (layer.nameUtf8.empty() ? std::to_string(layer.orderId) : layer.nameUtf8);
		layerNode.uidUtf8 = GB_Md5Hash(currentPath);

		layerNode.children.reserve(layer.subLayers.size());
		for (const WmsLayerProperty& subLayer : layer.subLayers)
		{
			WmsTreeNode subLayerNode = CreateWmsLayerNode(subLayer, currentPath);
			layerNode.children.push_back(std::move(subLayerNode));
		}
		std::sort(layerNode.children.begin(), layerNode.children.end(), [](const WmsTreeNode& a, const WmsTreeNode& b) {
			return GB_Utf8CompareLogical(a.textUtf8, b.textUtf8) < 0;
		});
		return layerNode;
	}
}

bool BuildWmsLayerTree(const WmsCapabilitiesProperty& capabilities, WmsTreeNode& rootNode, const BuildLayerTreeOptions& options)
{
	rootNode = WmsTreeNode();
	rootNode.textUtf8 = capabilities.service.titleUtf8;
	rootNode.nodeType = WmsTreeNode::NodeType::Root;

	const std::string currentPath = (rootNode.textUtf8.empty() ? GB_STR("root") : rootNode.textUtf8);
	rootNode.uidUtf8 = GB_Md5Hash(currentPath);
	rootNode.children.reserve(capabilities.capability.tileLayers.size() + capabilities.capability.layers.size());

	const std::vector<WmtsTileLayer>& tileLayers = capabilities.capability.tileLayers;
	for (const WmtsTileLayer& tileLayer : tileLayers)
	{
		WmsTreeNode layerNode = CreateTileLayerNode(tileLayer, options, currentPath);
		rootNode.children.push_back(std::move(layerNode));
	}

	const std::vector<WmsLayerProperty>& layers = capabilities.capability.layers;
	for (const WmsLayerProperty& layer : layers)
	{
		WmsTreeNode layerNode = CreateWmsLayerNode(layer, currentPath);
		rootNode.children.push_back(std::move(layerNode));
	}

	std::sort(rootNode.children.begin(), rootNode.children.end(), [](const WmsTreeNode& a, const WmsTreeNode& b) {
		return GB_Utf8CompareLogical(a.textUtf8, b.textUtf8) < 0;
	});
	return true;
}







