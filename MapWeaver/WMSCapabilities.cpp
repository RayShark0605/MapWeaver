#include "WMSCapabilities.h"
#include "Network.h"
#include <unordered_set>
#include <iomanip>
#include <regex>
#include "libxml/parser.h"
using namespace std;

#define GETTEXT(x) x->GetText() ? x->GetText() : ""

namespace internal
{
	bool IsUrlForWMTS(const string& url)
	{
		string lowerUrl = url;
		transform(lowerUrl.begin(), lowerUrl.end(), lowerUrl.begin(), ::tolower);
		return lowerUrl.find("service=wmts") != string::npos || lowerUrl.find("/wmtscapabilities.xml") != string::npos;
	}

	string ToLower(const string& str)
	{
		string result = str;
		transform(result.begin(), result.end(), result.begin(), ::tolower);
		return result;
	}

	bool StartsWith(const string& str, const string& prefix)
	{
		if (str.size() < prefix.size())
		{
			return false;
		}

		return (str.substr(0, prefix.size()) == prefix);
	}
}

bool WMSCapabilitiesWorker::CheckRootTag(TiXmlElement* root)
{
	if (!root)
	{
		return false;
	}
	const string docTag = internal::ToLower(root->ValueStr());
	return internal::ToLower(docTag) == internal::ToLower("WMS_Capabilities") || 
		internal::ToLower(docTag) == internal::ToLower("WMT_MS_Capabilities") ||
		internal::ToLower(docTag) == internal::ToLower("Capabilities");
}

// 属性是否存在
bool WMSCapabilitiesWorker::ExistsAttribute(TiXmlElement* element, const string& attributeName)
{
	if (!element || attributeName.empty())
	{
		return false;
	}

	string stringValue = "";
	return element->QueryStringAttribute(attributeName.c_str(), &stringValue) != TIXML_NO_ATTRIBUTE;
}

// 获取属性。例如从"<Capabilities version="1.0.0" xmlns="http://www.opengis.net/wmts/1.0">"中获取"version"
bool WMSCapabilitiesWorker::GetAttribute(TiXmlElement* element, const string& attributeName, string& value)
{
	if (!element || attributeName.empty())
	{
		return false;
	}
	const string* result = element->Attribute(attributeName);
	if (!result)
	{
		return false;
	}
	value = *result;
	return true;
}


// 获取值。例如从"<ows:Title>Esri_Hydro_Reference_Overlay</ows:Title>"中获取Esri_Hydro_Reference_Overlay
bool WMSCapabilitiesWorker::GetValue(TiXmlElement* element, string& value)
{
	if (!element)
	{
		return false;
	}

	TiXmlNode* firstNode = element->FirstChild();
	if (!firstNode)
	{
		return false;
	}
	value = firstNode->ValueStr();
	return true;
}

// 解析Service或ows:ServiceProvider或ows:ServiceIdentification
void WMSCapabilitiesWorker::ParseService(TiXmlElement* node, WMSCapabilitiesService& service)
{
	if (!node)
	{
		return;
	}
	TiXmlElement* curNode = node->FirstChildElement();
	while (curNode)
	{
		string tagName = curNode->ValueStr();
		// 去掉可能存在的"wms:"和"ows:"前缀
		if (internal::StartsWith(tagName, "wms:") || internal::StartsWith(tagName, "ows:"))
		{
			tagName = tagName.substr(4);
		}

		if (tagName == "Title")
		{
			GetValue(curNode, service.title);
		}
		else if (tagName == "Abstract")
		{
			GetValue(curNode, service.abstract);
		}
		else if (tagName == "KeywordList" || tagName == "Keywords")
		{
			ParseKeywordList(curNode, service.keywordList);
		}
		else if (tagName == "OnlineResource")
		{
			ParseOnlineResource(curNode, service.onlineResourceHref);
		}
		else if (tagName == "ContactInformation" || tagName == "ServiceContact")
		{
			//...
		}
		else if (tagName == "Fees")
		{
			service.fees = GETTEXT(curNode);
		}
		else if (tagName == "AccessConstraints")
		{
			service.accessConstraints = GETTEXT(curNode);
		}
		else if (tagName == "LayerLimit")
		{
			service.layerLimit = stoul(GETTEXT(curNode));
		}
		else if (tagName == "MaxWidth")
		{
			service.maxWidth = stoul(GETTEXT(curNode));
		}
		else if (tagName == "MaxHeight")
		{
			service.maxHeight = stoul(GETTEXT(curNode));
		}

		curNode = curNode->NextSiblingElement();
	}
}

void WMSCapabilitiesWorker::ParseOnlineResource(TiXmlElement* node, string& xref)
{
	if (!node)
	{
		return;
	}

	xref = "";
	GetAttribute(node, "xlink:href", xref);
}

void WMSCapabilitiesWorker::ParseGet(TiXmlElement* node, string& get)
{
	if (!node)
	{
		return;
	}

	get = "";
	TiXmlElement* curNode = node->FirstChildElement();
	while (curNode)
	{
		string tagName = curNode->ValueStr();
		if (internal::StartsWith(tagName, "wms:"))
		{
			tagName = tagName.substr(4);
		}
		if (tagName == "OnlineResource")
		{
			ParseOnlineResource(curNode, get);
			if (!get.empty())
			{
				return;
			}
		}
		curNode = curNode->NextSiblingElement();
	}
}

void WMSCapabilitiesWorker::ParsePost(TiXmlElement* node, string& post)
{
	if (!node)
	{
		return;
	}

	post = "";
	TiXmlElement* curNode = node->FirstChildElement();
	while (curNode)
	{
		string tagName = curNode->ValueStr();
		if (internal::StartsWith(tagName, "wms:"))
		{
			tagName = tagName.substr(4);
		}

		if (tagName == "OnlineResource")
		{
			ParseOnlineResource(curNode, post);
			if (!post.empty())
			{
				return;
			}
		}
		curNode = curNode->NextSiblingElement();
	}
}

void WMSCapabilitiesWorker::ParseHTTP(TiXmlElement* node, WMSCapabilitiesCapability::CapabilityRequest::CapabilityRequestOperation::HTTP& http)
{
	if (!node)
	{
		return;
	}

	TiXmlElement* curNode = node->FirstChildElement();
	while (curNode)
	{
		string tagName = curNode->ValueStr();
		if (internal::StartsWith(tagName, "wms:"))
		{
			tagName = tagName.substr(4);
		}

		if (tagName == "Get")
		{
			ParseGet(curNode, http.get);
		}
		else if (tagName == "Post")
		{
			ParsePost(curNode, http.post);
		}
		curNode = curNode->NextSiblingElement();
	}
}

void WMSCapabilitiesWorker::ParseDCPType(TiXmlElement* node, WMSCapabilitiesCapability::CapabilityRequest::CapabilityRequestOperation::HTTP& http)
{
	if (!node)
	{
		return;
	}

	TiXmlElement* curNode = node->FirstChildElement();
	while (curNode)
	{
		if (curNode->ValueStr() == "HTTP")
		{
			ParseHTTP(curNode, http);
		}
		curNode = curNode->NextSiblingElement();
	}
}

void WMSCapabilitiesWorker::ParseOperation(TiXmlElement* node, WMSCapabilitiesCapability::CapabilityRequest::CapabilityRequestOperation& operation)
{
	if (!node)
	{
		return;
	}

	TiXmlElement* curNode = node->FirstChildElement();
	while (curNode)
	{
		string tagName = curNode->ValueStr();
		if (internal::StartsWith(tagName, "wms:"))
		{
			tagName = tagName.substr(4);
		}

		if (tagName == "Format")
		{
			string format = "";
			if (GetValue(curNode, format) && !format.empty())
			{
				operation.format.push_back(format);
			}
		}
		else if (tagName == "DCPType")
		{
			WMSCapabilitiesCapability::CapabilityRequest::CapabilityRequestOperation::HTTP http;
			ParseDCPType(curNode, http);
			operation.dcpType.push_back(http);
		}
		curNode = curNode->NextSiblingElement();
	}
}

// 解析Request
void WMSCapabilitiesWorker::ParseRequest(TiXmlElement* node, WMSCapabilitiesCapability::CapabilityRequest& request)
{
	TiXmlElement* curNode = node->FirstChildElement();
	while (curNode)
	{
		string operationName = curNode->ValueStr();
		if (operationName == "Operation")
		{
			GetAttribute(curNode, "name", operationName);
		}

		if (operationName == "GetMap")
		{
			ParseOperation(curNode, request.getMap);
		}
		else if (operationName == "GetFeatureInfo")
		{
			ParseOperation(curNode, request.getFeatureInfo);
		}
		else if (operationName == "GetLegendGraphic" || operationName == "sld:GetLegendGraphic")
		{
			ParseOperation(curNode, request.getLegendGraphic);
		}
		curNode = curNode->NextSiblingElement();
	}
}

void WMSCapabilitiesWorker::ParseKeywordList(TiXmlElement* node, vector<string>& keywordList)
{
	if (!node)
	{
		return;
	}

	TiXmlElement* curNode = node->FirstChildElement();
	while (curNode)
	{
		string tagName = curNode->ValueStr();
		if (internal::StartsWith(tagName, "wms:") || internal::StartsWith(tagName, "ows:"))
		{
			tagName = tagName.substr(4);
		}

		if (tagName == "Keyword")
		{
			string keyword = "";
			if (GetValue(curNode, keyword) && !keyword.empty())
			{
				keywordList.push_back(keyword);
			}
		}

		curNode = curNode->NextSiblingElement();
	}
}

void WMSCapabilitiesWorker::ParseKeywords(TiXmlElement* node, vector<string>& keywords)
{
	if (!node)
	{
		return;
	}

	TiXmlElement* keywordsNode = node->FirstChildElement("ows:Keywords");
	if (!keywordsNode)
	{
		return;
	}

	keywords.clear();
	TiXmlElement* curNode = keywordsNode->FirstChildElement("ows:Keyword");
	while (curNode)
	{
		keywords.push_back(GETTEXT(curNode));
		curNode = curNode->NextSiblingElement("ows:Keyword");
	}
}

void WMSCapabilitiesWorker::ParseMetaURL(TiXmlElement* node, WMSLayerMetadataUrl& metaURL)
{
	if (!node)
	{
		return;
	}

	TiXmlElement* curNode = node->FirstChildElement();
	while (curNode)
	{
		string tagName = curNode->ValueStr();
		if (internal::StartsWith(tagName, "wms:"))
		{
			tagName = tagName.substr(4);
		}

		if (internal::ToLower(tagName) == internal::ToLower("Format"))
		{
			string format = "";
			if (GetValue(curNode, format) && !format.empty())
			{
				metaURL.format = format;
			}
		}
		else if (internal::ToLower(tagName) == internal::ToLower("OnlineResource"))
		{
			ParseOnlineResource(curNode, metaURL.xlinkHref);
		}
		curNode = curNode->NextSiblingElement();
	}
}

void WMSCapabilitiesWorker::ParseLegendURL(TiXmlElement* node, WMSLayerStyle::WMSLayerStyleLegendUrl& legendURL)
{
	if (!node)
	{
		return;
	}

	node->QueryIntAttribute("width", &legendURL.width);
	node->QueryIntAttribute("width", &legendURL.height);

	TiXmlElement* curNode = node->FirstChildElement();
	while (curNode)
	{
		string tagName = curNode->ValueStr();
		if (internal::StartsWith(tagName, "wms:"))
		{
			tagName = tagName.substr(4);
		}

		if (tagName == "Format")
		{
			legendURL.format = GETTEXT(curNode);
		}
		else if (tagName == "Format")
		{
			ParseOnlineResource(curNode, legendURL.xlinkHref);
		}

		curNode = curNode->NextSiblingElement();
	}
}

void WMSCapabilitiesWorker::ParseStyle(TiXmlElement* node, WMSLayerStyle& style)
{
	if (!node)
	{
		return;
	}

	TiXmlElement* curNode = node->FirstChildElement();
	while (curNode)
	{
		string tagName = curNode->ValueStr();
		if (internal::StartsWith(tagName, "wms:"))
		{
			tagName = tagName.substr(4);
		}

		if (tagName == "Name")
		{
			style.name = GETTEXT(curNode);
			/*string name = "";
			if (GetValue(curNode, name) && !name.empty())
			{
				style.name = name;
			}*/
		}
		else if (tagName == "Title")
		{
			style.title = GETTEXT(curNode);
			/*string title = "";
			if (GetValue(curNode, title) && !title.empty())
			{
				style.title = title;
			}*/
		}
		else if (tagName == "Abstract")
		{
			style.abstract = GETTEXT(curNode);
			/*string abstract = "";
			if (GetValue(curNode, abstract) && !abstract.empty())
			{
				style.abstract = abstract;
			}*/
		}
		else if (tagName == "LegendURL")
		{
			WMSLayerStyle::WMSLayerStyleLegendUrl legendURL;
			ParseLegendURL(curNode, legendURL);
			style.legendUrl.push_back(legendURL);
		}
		else if (tagName == "StyleSheetURL")
		{
			//...
		}
		else if (tagName == "StyleURL")
		{
			//...
		}

		curNode = curNode->NextSiblingElement();
	}
}


void WMSCapabilitiesWorker::ParseLayer(TiXmlElement* node, WMSLayer& layer, WMSLayer* parentLayer)
{
	if (!node)
	{
		return;
	}

	numLayers++;
	layer.orderID = numLayers;

	string queryable = "0";
	GetAttribute(node, "queryable", queryable);
	layer.queryable = (queryable == "1" || internal::ToLower(queryable) == "true");

	string cascaded = "0";
	GetAttribute(node, "cascaded", cascaded);
	layer.cascaded = static_cast<unsigned int>(stoul(cascaded));

	string opaque = "0";
	GetAttribute(node, "opaque", opaque);
	layer.opaque = (opaque == "1" || internal::ToLower(opaque) == "true");

	string noSubsets = "0";
	GetAttribute(node, "noSubsets", noSubsets);
	layer.noSubsets = (noSubsets == "1" || internal::ToLower(noSubsets) == "true");

	string fixedWidth = "0";
	GetAttribute(node, "fixedWidth", fixedWidth);
	layer.fixedWidth = static_cast<unsigned int>(stoul(fixedWidth));

	string fixedHeight = "0";
	GetAttribute(node, "fixedHeight", fixedHeight);
	layer.fixedHeight = static_cast<unsigned int>(stoul(fixedHeight));

	TiXmlElement* curNode = node->FirstChildElement();
	while (curNode)
	{
		string tagName = curNode->ValueStr();
		if (internal::StartsWith(tagName, "wms:") || internal::StartsWith(tagName, "ows:"))
		{
			tagName = tagName.substr(4);
		}

		if (tagName == "Layer")
		{
			WMSLayer subLayer;

			vector<WMSLayerStyle> inheritedStyles = layer.style;
			TiXmlElement* subLayerNameNode = curNode->FirstChildElement("Name");
			string subLayerName = "";
			if (subLayerNameNode && GetValue(subLayerNameNode, subLayerName) && !subLayerName.empty())
			{
				for (WMSLayerStyle& style : inheritedStyles)
				{
					for (WMSLayerStyle::WMSLayerStyleLegendUrl& legendURL : style.legendUrl)
					{
						string oldLayerParam = "";
						if (URLProcessing::HasQueryParam(legendURL.xlinkHref, "layer", oldLayerParam))
						{
							URLProcessing::AddQueryParam(legendURL.xlinkHref, "layer", subLayerName);
						}
					}
				}
			}
			subLayer.style = inheritedStyles;

			subLayer.crs = layer.crs;
			subLayer.boundingBox = layer.boundingBox;
			subLayer.ex_GeographicBoundingBox = layer.ex_GeographicBoundingBox;

			ParseLayer(curNode, subLayer, &layer); // 递归调用
			layer.layer.push_back(subLayer);
		}
		else if (tagName == "Name")
		{
			GetValue(curNode, layer.name);
		}
		else if (tagName == "Title")
		{
			GetValue(curNode, layer.title);
		}
		else if (tagName == "Abstract")
		{
			GetValue(curNode, layer.abstract);
		}
		else if (tagName == "KeywordList")
		{
			ParseKeywordList(curNode, layer.keywordList);
		}
		else if (tagName == "SRS" || tagName == "CRS")
		{
			string crsValue = "";
			if (GetValue(curNode, crsValue) && !crsValue.empty())
			{
				// CRS可能包含多个用空白字符分隔的定义, 尽管这种方式在WMS 1.1.1中已被废弃
				const vector<string> crsList = SplitString(crsValue);
				for (const string& crs : crsList)
				{
					if (find(layer.crs.begin(), layer.crs.end(), crs) == layer.crs.end())
					{
						layer.crs.push_back(crs);
					}
				}
			}
		}
		else if (tagName == "LatLonBoundingBox") // WMS早期版本
		{
			// boundingBox可能会使用逗号作为小数分隔符, 并且图层范围根本没有被计算. 通过将逗号替换为点号来修复该问题
			string minX = "", minY = "", maxX = "", maxY = "";
			if (GetAttribute(curNode, "minx", minX) && GetAttribute(curNode, "miny", minY) &&
				GetAttribute(curNode, "maxx", maxX) && GetAttribute(curNode, "maxy", maxY) &&
				!minX.empty() && !minY.empty() && !maxX.empty() && !maxY.empty())
			{
				replace(minX.begin(), minX.end(), ',', '.');
				replace(minY.begin(), minY.end(), ',', '.');
				replace(maxX.begin(), maxX.end(), ',', '.');
				replace(maxY.begin(), maxY.end(), ',', '.');

				layer.ex_GeographicBoundingBox = Rectangle(stod(minX), stod(minY), stod(maxX), stod(maxY));
			}

			string srsString = "";
			if (GetAttribute(curNode, "SRS", srsString) && !srsString.empty() && srsString != "CRS:84")
			{
				const BoundingBox srcBoundingBox(srsString, layer.ex_GeographicBoundingBox);
				BoundingBox destBoundingBox("CRS:84", Rectangle());
				if (CSConverter::TransformBoundingBox(srcBoundingBox, destBoundingBox))
				{
					layer.ex_GeographicBoundingBox = destBoundingBox.bbox;
				}
			}
		}
		else if (tagName == "EX_GeographicBoundingBox") // WMS 1.3
		{
			TiXmlElement* westBoundLongitudeNode = nullptr;
			TiXmlElement* eastBoundLongitudeNode = nullptr;
			TiXmlElement* southBoundLatitudeNode = nullptr;
			TiXmlElement* northBoundLatitudeNode = nullptr;

			if (curNode->ValueStr() == "wms:EX_GeographicBoundingBox")
			{
				westBoundLongitudeNode = curNode->FirstChildElement("wms:westBoundLongitude");
				eastBoundLongitudeNode = curNode->FirstChildElement("wms:eastBoundLongitude");
				southBoundLatitudeNode = curNode->FirstChildElement("wms:southBoundLatitude");
				northBoundLatitudeNode = curNode->FirstChildElement("wms:northBoundLatitude");
			}
			else
			{
				westBoundLongitudeNode = curNode->FirstChildElement("westBoundLongitude");
				eastBoundLongitudeNode = curNode->FirstChildElement("eastBoundLongitude");
				southBoundLatitudeNode = curNode->FirstChildElement("southBoundLatitude");
				northBoundLatitudeNode = curNode->FirstChildElement("northBoundLatitude");
			}

			if (westBoundLongitudeNode && eastBoundLongitudeNode && southBoundLatitudeNode && northBoundLatitudeNode)
			{
				string westBoundLongitudeString = "", eastBoundLongitudeString = "", southBoundLatitudeString = "", northBoundLatitudeString = "";
				if (GetValue(westBoundLongitudeNode, westBoundLongitudeString) && GetValue(eastBoundLongitudeNode, eastBoundLongitudeString) &&
					GetValue(southBoundLatitudeNode, southBoundLatitudeString) && GetValue(northBoundLatitudeNode, northBoundLatitudeString) &&
					!westBoundLongitudeString.empty() && !eastBoundLongitudeString.empty() && !southBoundLatitudeString.empty() && !northBoundLatitudeString.empty())
				{
					replace(westBoundLongitudeString.begin(), westBoundLongitudeString.end(), ',', '.');
					replace(eastBoundLongitudeString.begin(), eastBoundLongitudeString.end(), ',', '.');
					replace(southBoundLatitudeString.begin(), southBoundLatitudeString.end(), ',', '.');
					replace(northBoundLatitudeString.begin(), northBoundLatitudeString.end(), ',', '.');

					const double minX = stod(westBoundLongitudeString);
					const double minY = stod(southBoundLatitudeString);
					const double maxX = stod(eastBoundLongitudeString);
					const double maxY = stod(northBoundLatitudeString);

					layer.ex_GeographicBoundingBox = Rectangle(minX, minY, maxX, maxY);
				}
			}
		}
		else if (tagName == "BoundingBox")
		{
			BoundingBox boundingBox;

			string minXString = "", minYString = "", maxXString = "", maxYString = "";
			if (GetAttribute(curNode, "minx", minXString) && GetAttribute(curNode, "miny", minYString) &&
				GetAttribute(curNode, "maxx", maxXString) && GetAttribute(curNode, "maxy", maxYString) &&
				!minXString.empty() && !minYString.empty() && !maxXString.empty() && !maxYString.empty())
			{
				replace(minXString.begin(), minXString.end(), ',', '.');
				replace(minYString.begin(), minYString.end(), ',', '.');
				replace(maxXString.begin(), maxXString.end(), ',', '.');
				replace(maxYString.begin(), maxYString.end(), ',', '.');

				const double minX = stod(minXString);
				const double minY = stod(minYString);
				const double maxX = stod(maxXString);
				const double maxY = stod(maxYString);
				boundingBox.bbox = Rectangle(minX, minY, maxX, maxY);
			}

			if (ExistsAttribute(curNode, "CRS") || ExistsAttribute(curNode, "SRS"))
			{
				string crs = "", srs = "";
				if (ExistsAttribute(curNode, "CRS") && GetAttribute(curNode, "CRS", crs) && !crs.empty())
				{
					boundingBox.crs = crs;
				}
				else if (ExistsAttribute(curNode, "SRS") && GetAttribute(curNode, "SRS", srs) && !srs.empty())
				{
					boundingBox.crs = srs;
				}

				if ((capabilities.version == "1.3.0" || capabilities.version == "1.3") && CSConverter::ShouldInvertAxisOrientation(boundingBox.crs))
				{
					const Rectangle invBoundingBox(boundingBox.bbox.GetMinPoint().y, boundingBox.bbox.GetMinPoint().x, boundingBox.bbox.GetMaxPoint().y, boundingBox.bbox.GetMaxPoint().x);
					boundingBox.bbox = invBoundingBox;
				}

				// 每个CRS只能对应一个boundingbox
				bool existSameCRS = false;
				for (BoundingBox& bbox : layer.boundingBox)
				{
					if (bbox.crs == boundingBox.crs) // 如果已经存在相同CRS的boundingbox, 则用新值替换旧值
					{
						bbox = boundingBox;
						existSameCRS = true;
					}
				}
				if (!existSameCRS) // 如果没有相同CRS的boundingbox, 则添加新的boundingbox
				{
					layer.boundingBox.push_back(boundingBox);
				}
			}
		}
		else if (tagName == "Dimension")
		{
			//...
		}
		else if (tagName == "Extent")
		{
			//...
		}
		else if (tagName == "Attribution")
		{
			//...
		}
		else if (tagName == "AuthorityURL")
		{
			//...
		}
		else if (tagName == "Identifier")
		{
			//...
		}
		else if (tagName == "MetadataURL")
		{
			WMSLayerMetadataUrl metaURL;
			ParseMetaURL(curNode, metaURL);
			layer.metadataUrl.push_back(metaURL);
		}
		else if (tagName == "DataURL")
		{
			//...
		}
		else if (tagName == "FeatureListURL")
		{
			//...
		}
		else if (tagName == "Style")
		{
			WMSLayerStyle style;
			ParseStyle(curNode, style);

			for (size_t i = 0; i < layer.style.size(); i++)
			{
				if (layer.style[i].name == style.name)
				{
					// 覆盖继承自父图层的样式. 根据WMS规范, 这种情况不应该发生, 但Mapserver可能会出现这种问题
					layer.style.erase(layer.style.begin() + i);
					break;
				}
			}
			layer.style.push_back(style);
		}
		else if (tagName == "MinScaleDenominator")
		{
			//...
		}
		else if (tagName == "MaxScaleDenominator")
		{
			//...
		}

		curNode = curNode->NextSiblingElement();
	}

	if (parentLayer)
	{
		layerParents[layer.orderID] = parentLayer->orderID;
	}

	if (!layer.name.empty())
	{
		layerQueryable[layer.name] = layer.queryable;
		layers.push_back(layer);
		
		// 如果多个 <Layer> 元素没有父图层, 则需要清空样式列表
		if (layer.layer.empty())
		{
			layer.style.clear();
		}
	}

	if (!layer.layer.empty())
	{
		layerParentNames[layer.orderID] = { layer.name, layer.title, layer.abstract };
	}
}

// 解析Capability或ows:OperationsMetadata
void WMSCapabilitiesWorker::ParseCapability(TiXmlElement* node, WMSCapabilitiesCapability& capability)
{
	if (!node)
	{
		return;
	}

	TiXmlElement* curNode = node->FirstChildElement();
	while (curNode)
	{
		string tagName = curNode->ValueStr();
		if (internal::StartsWith(tagName, "wms:")) // 去掉可能存在的"wms:"前缀
		{
			tagName = tagName.substr(4);
		}

		if (tagName == "Request")
		{
			ParseRequest(curNode, capability.request);
		}
		else if (tagName == "Layer")
		{
			WMSLayer layer;
			ParseLayer(curNode, layer);
			capability.layers.push_back(layer);
		}
		else if (tagName == "VendorSpecificCapabilities")
		{
			//...
		}
		else if (tagName == "ows:Operation")
		{
			string operationName = "";
			if (!GetAttribute(curNode, "name", operationName) || operationName.empty())
			{
				curNode = curNode->NextSiblingElement();
				continue;
			}

			TiXmlElement* dcpNode = curNode->FirstChildElement("ows:DCP");
			if (!dcpNode)
			{
				curNode = curNode->NextSiblingElement();
				continue;
			}

			TiXmlElement* httpNode = dcpNode->FirstChildElement("ows:HTTP");
			if (!httpNode)
			{
				curNode = curNode->NextSiblingElement();
				continue;
			}

			TiXmlElement* getNode = httpNode->FirstChildElement("ows:Get");
			if (!getNode)
			{
				curNode = curNode->NextSiblingElement();
				continue;
			}

			WMSCapabilitiesCapability::CapabilityRequest::CapabilityRequestOperation* requestOperation = nullptr;
			WMSCapabilitiesCapability::CapabilityRequest::CapabilityRequestOperation::HTTP http;
			GetAttribute(getNode, "xlink:href", http.get);
			if (!http.get.empty())
			{
				if (operationName == "GetTile")
				{
					requestOperation = &capability.request.getTile;
				}
				else if (operationName == "GetFeatureInfo")
				{
					requestOperation = &capability.request.getFeatureInfo;
				}
				else if (operationName == "GetLegendGraphic" || operationName == "sld:GetLegendGraphic")
				{
					requestOperation = &capability.request.getLegendGraphic;
				}
			}

			if (requestOperation)
			{
				requestOperation->dcpType.push_back(http);
				requestOperation->allowedEncodings.clear();

				TiXmlElement* constraintNode = getNode->FirstChildElement("ows:Constraint");
				if (constraintNode)
				{
					TiXmlElement* allowedValuesNode = constraintNode->FirstChildElement("ows:AllowedValues");
					if (allowedValuesNode)
					{
						TiXmlElement* valueNode = allowedValuesNode->FirstChildElement("ows:Value");
						while (valueNode)
						{
							string value = "";
							if (GetValue(valueNode, value) && !value.empty())
							{
								// 将KVP转换为大写
								if (internal::ToLower(value) == "kvp")
								{
									value = "KVP";
								}
								requestOperation->allowedEncodings.push_back(value);
							}

							valueNode = valueNode->NextSiblingElement("ows:Value");
						}
					}
				}
			}
		}

		curNode = curNode->NextSiblingElement();
	}

	// 有些TileSet图层可能没有设置title或abstract, 这些缺失的属性可能在普通图层的列表中存在, 因此尝试补充这些信息
	if (!tileLayers.empty())
	{
		unordered_map<string, string> titles, abstracts;
		for (const WMSLayer& layer : layers)
		{
			if (layer.name.empty())
			{
				continue;
			}

			if (!layer.title.empty())
			{
				titles[layer.name, layer.title];
			}

			if (!layer.abstract.empty())
			{
				abstracts[layer.name, layer.abstract];
			}
		}

		for (WMTSTileLayer& tileLayer : tileLayers)
		{
			if (tileLayer.title.empty() && titles.find(tileLayer.identifier) != titles.end())
			{
				tileLayer.title = titles[tileLayer.identifier];
			}

			if (tileLayer.abstract.empty() && abstracts.find(tileLayer.identifier) != abstracts.end())
			{
				tileLayer.abstract = abstracts[tileLayer.identifier];
			}
		}
	}
}

void WMSCapabilitiesWorker::ParseContents(TiXmlElement* node)
{
	if (!node)
	{
		return;
	}

	tileMatrixSets.clear();

	TiXmlElement* curNode = node->FirstChildElement("TileMatrixSet");
	while (curNode)
	{
		WMTSTileMatrixSet matrixSet;

		TiXmlElement* identifierNode = curNode->FirstChildElement("ows:Identifier");
		if (identifierNode)
		{
			matrixSet.identifier = GETTEXT(identifierNode);
		}

		TiXmlElement* titleNode = curNode->FirstChildElement("ows:Title");
		if (titleNode)
		{
			matrixSet.title = GETTEXT(titleNode);
		}

		TiXmlElement* abstractNode = curNode->FirstChildElement("ows:Abstract");
		if (abstractNode)
		{
			matrixSet.abstract = GETTEXT(abstractNode);
		}

		ParseKeywords(curNode, matrixSet.keywordList);

		TiXmlElement* wkScaleSetNode = curNode->FirstChildElement("WellKnownScaleSet");
		if (wkScaleSetNode)
		{
			matrixSet.wkScaleSet = GETTEXT(wkScaleSetNode);
		}

		TiXmlElement* supportedCRSNode = curNode->FirstChildElement("ows:SupportedCRS");
		if (supportedCRSNode)
		{
			const string supportedCRSString = GETTEXT(supportedCRSNode);
			OGRSpatialReference crs;
			if (SetCRS(supportedCRSString, crs))
			{
				const char* authName = crs.GetAuthorityName(nullptr);
				const char* authCode = crs.GetAuthorityCode(nullptr);
				if (authName && authCode)
				{
					matrixSet.crs = string(authName) + ":" + string(authCode);
				}

				const double metersPerUnit = (crs.IsGeographic() ? 111319.49079327358 : crs.GetLinearUnits());
				const bool isAxisInverted = crs.EPSGTreatsAsLatLong() || crs.EPSGTreatsAsNorthingEasting();

				for (TiXmlElement* tileMatrixNode = curNode->FirstChildElement("TileMatrix");
					tileMatrixNode;
					tileMatrixNode = tileMatrixNode->NextSiblingElement("TileMatrix"))
				{
					WMTSTileMatrix tileMatrix;

					TiXmlElement* identifierNode = tileMatrixNode->FirstChildElement("ows:Identifier");
					if (identifierNode)
					{
						tileMatrix.identifier = GETTEXT(identifierNode);
					}

					TiXmlElement* titleNode = tileMatrixNode->FirstChildElement("ows:Title");
					if (titleNode)
					{
						tileMatrix.title = GETTEXT(titleNode);
					}

					TiXmlElement* abstractNode = tileMatrixNode->FirstChildElement("ows:Abstract");
					if (abstractNode)
					{
						tileMatrix.abstract = GETTEXT(abstractNode);
					}

					ParseKeywords(tileMatrixNode, tileMatrix.keywordList);

					TiXmlElement* scaleDenominatorNode = tileMatrixNode->FirstChildElement("ScaleDenominator");
					if (scaleDenominatorNode)
					{
						tileMatrix.scaleDenominator = stod(GETTEXT(scaleDenominatorNode));
					}

					TiXmlElement* topLeftNode = tileMatrixNode->FirstChildElement("TopLeftCorner");
					if (topLeftNode)
					{
						const vector<string> topLeftString = SplitString(GETTEXT(topLeftNode));
						if (topLeftString.size() == 2)
						{
							tileMatrix.topLeft.x = (isAxisInverted ? stod(topLeftString[1]) : stod(topLeftString[0]));
							tileMatrix.topLeft.y = (isAxisInverted ? stod(topLeftString[0]) : stod(topLeftString[1]));
							if (IsTianDiTu())
							{
								swap(tileMatrix.topLeft.x, tileMatrix.topLeft.y);
							}
						}
					}

					TiXmlElement* tileWidthNode = tileMatrixNode->FirstChildElement("TileWidth");
					if (tileWidthNode)
					{
						tileMatrix.tileWidth = stoi(GETTEXT(tileWidthNode));
					}

					TiXmlElement* tileHeightNode = tileMatrixNode->FirstChildElement("TileHeight");
					if (tileHeightNode)
					{
						tileMatrix.tileHeight = stoi(GETTEXT(tileHeightNode));
					}

					TiXmlElement* matrixWidthNode = tileMatrixNode->FirstChildElement("MatrixWidth");
					if (matrixWidthNode)
					{
						tileMatrix.matrixWidth = stoi(GETTEXT(matrixWidthNode));
					}

					TiXmlElement* matrixHeightNode = tileMatrixNode->FirstChildElement("MatrixHeight");
					if (matrixHeightNode)
					{
						tileMatrix.matrixHeight = stoi(GETTEXT(matrixHeightNode));
					}

					if (IsTianDiTu())
					{
						tileMatrix.pixelSize = tileMatrix.scaleDenominator * 0.0254 / 96 / metersPerUnit;
					}
					else
					{
						tileMatrix.pixelSize = tileMatrix.scaleDenominator * 0.00028 / metersPerUnit;
					}

					matrixSet.tileMatrices[tileMatrix.pixelSize] = tileMatrix;
				}

				tileMatrixSets[matrixSet.identifier] = matrixSet;
			}
		}
		curNode = curNode->NextSiblingElement("TileMatrixSet");
	}

	tileLayers.clear();
	for (TiXmlElement* layerNode = node->FirstChildElement("Layer"); layerNode; layerNode = layerNode->NextSiblingElement("Layer"))
	{
		WMTSTileLayer tileLayer;
		tileLayer.tileMode = WMTSTileLayer::TileMode::WMTS;

		TiXmlElement* identifierNode = layerNode->FirstChildElement("ows:Identifier");
		if (identifierNode)
		{
			tileLayer.identifier = GETTEXT(identifierNode);
		}

		TiXmlElement* titleNode = layerNode->FirstChildElement("ows:Title");
		if (titleNode)
		{
			tileLayer.title = GETTEXT(titleNode);
		}

		TiXmlElement* abstractNode = layerNode->FirstChildElement("ows:Abstract");
		if (abstractNode)
		{
			tileLayer.abstract = GETTEXT(abstractNode);
		}

		ParseKeywords(layerNode, tileLayer.keywordList);

		BoundingBox boundingBox;
		TiXmlElement* boundingBoxNode = layerNode->FirstChildElement("ows:WGS84BoundingBox");
		if (boundingBoxNode)
		{
			TiXmlElement* lowerCornerNode = boundingBoxNode->FirstChildElement("ows:LowerCorner");
			TiXmlElement* upperCornerNode = boundingBoxNode->FirstChildElement("ows:UpperCorner");
			if (lowerCornerNode && upperCornerNode)
			{
				const vector<string> lowerCornerString = SplitString(GETTEXT(lowerCornerNode));
				const vector<string> upperCornerString = SplitString(GETTEXT(upperCornerNode));
				if (lowerCornerString.size() == 2 && upperCornerString.size() == 2)
				{
					boundingBox.crs = "CRS:84";
					boundingBox.bbox = Rectangle(stod(lowerCornerString[0]), stod(lowerCornerString[1]), 
						stod(upperCornerString[0]), stod(upperCornerString[1]));
					tileLayer.boundingBox.push_back(boundingBox);
				}
			}
		}

		for (boundingBoxNode = layerNode->FirstChildElement("ows:BoundingBox"); 
			boundingBoxNode; 
			boundingBoxNode = boundingBoxNode->NextSiblingElement("ows:BoundingBox"))
		{
			TiXmlElement* lowerCornerNode = boundingBoxNode->FirstChildElement("ows:LowerCorner");
			TiXmlElement* upperCornerNode = boundingBoxNode->FirstChildElement("ows:UpperCorner");
			if (lowerCornerNode && upperCornerNode)
			{
				const vector<string> lowerCornerString = SplitString(GETTEXT(lowerCornerNode));
				const vector<string> upperCornerString = SplitString(GETTEXT(upperCornerNode));
				if (lowerCornerString.size() == 2 && upperCornerString.size() == 2)
				{
					boundingBox.bbox = Rectangle(stod(lowerCornerString[0]), stod(lowerCornerString[1]),
						stod(upperCornerString[0]), stod(upperCornerString[1]));

					if (ExistsAttribute(boundingBoxNode, "SRS"))
					{
						boundingBoxNode->QueryStringAttribute("SRS", &boundingBox.crs);
					}
					else if (ExistsAttribute(boundingBoxNode, "srs"))
					{
						boundingBoxNode->QueryStringAttribute("srs", &boundingBox.crs);
					}
					else if (ExistsAttribute(boundingBoxNode, "CRS"))
					{
						boundingBoxNode->QueryStringAttribute("CRS", &boundingBox.crs);
					}
					else if (ExistsAttribute(boundingBoxNode, "crs"))
					{
						boundingBoxNode->QueryStringAttribute("crs", &boundingBox.crs);
					}

					if (!boundingBox.crs.empty())
					{
						OGRSpatialReference crs;
						if (crs.SetFromUserInput(boundingBox.crs.c_str()) == OGRERR_NONE)
						{
							const char* authName = crs.GetAuthorityName(nullptr);
							const char* authCode = crs.GetAuthorityCode(nullptr);
							if (authName && authCode)
							{
								boundingBox.crs = string(authName) + ":" + string(authCode);
							}

							const bool isAxisInverted = crs.EPSGTreatsAsLatLong() || crs.EPSGTreatsAsNorthingEasting();
							if (isAxisInverted)
							{
								boundingBox.Invert();
							}

							tileLayer.boundingBox.push_back(boundingBox);
						}
					}
				}
			}
		}

		for (TiXmlElement* styleNode = layerNode->FirstChildElement("Style");
			styleNode; styleNode = styleNode->NextSiblingElement("Style"))
		{
			WMTSTileLayer::WMTSStyle style;

			TiXmlElement* identifierNode = styleNode->FirstChildElement("ows:Identifier");
			if (identifierNode)
			{
				style.identifier = GETTEXT(identifierNode);
			}

			TiXmlElement* titleNode = styleNode->FirstChildElement("ows:Title");
			if (titleNode)
			{
				style.title = GETTEXT(titleNode);
			}

			TiXmlElement* abstractNode = styleNode->FirstChildElement("ows:Abstract");
			if (abstractNode)
			{
				style.abstract = GETTEXT(abstractNode);
			}

			ParseKeywords(styleNode, style.keywords);

			for (TiXmlElement* legendURLNode = styleNode->FirstChildElement("ows:legendURL");
				legendURLNode; legendURLNode = legendURLNode->NextSiblingElement("ows:legendURL"))
			{
				WMTSTileLayer::WMTSStyle::WMTSLegendURL legendURL;

				TiXmlElement* formatNode = legendURLNode->FirstChildElement("format");
				if (formatNode)
				{
					legendURL.format = GETTEXT(formatNode);
				}

				TiXmlElement* minScaleNode = legendURLNode->FirstChildElement("minScale");
				if (minScaleNode)
				{
					legendURL.minScale = stod(GETTEXT(minScaleNode));
				}

				TiXmlElement* maxScaleNode = legendURLNode->FirstChildElement("maxScale");
				if (maxScaleNode)
				{
					legendURL.maxScale = stod(GETTEXT(maxScaleNode));
				}

				TiXmlElement* hrefNode = legendURLNode->FirstChildElement("href");
				if (hrefNode)
				{
					legendURL.href = GETTEXT(hrefNode);
				}

				TiXmlElement* widthNode = legendURLNode->FirstChildElement("width");
				if (widthNode)
				{
					legendURL.width = stoi(GETTEXT(widthNode));
				}

				TiXmlElement* heightNode = legendURLNode->FirstChildElement("height");
				if (heightNode)
				{
					legendURL.height = stoi(GETTEXT(heightNode));
				}

				style.legendURLs.push_back(legendURL);
			}

			TiXmlElement* legendURLNode = styleNode->FirstChildElement("LegendURL");
			if (legendURLNode)
			{
				WMTSTileLayer::WMTSStyle::WMTSLegendURL legendURL;

				legendURLNode->QueryStringAttribute("format", &legendURL.format);
				legendURLNode->QueryDoubleAttribute("minScaleDenominator", &legendURL.minScale);
				legendURLNode->QueryDoubleAttribute("maxScaleDenominator", &legendURL.maxScale);
				legendURLNode->QueryStringAttribute("xlink:href", &legendURL.href);
				legendURLNode->QueryIntAttribute("width", &legendURL.width);
				legendURLNode->QueryIntAttribute("height", &legendURL.height);

				style.legendURLs.push_back(legendURL);
			}

			string isDefault = "";
			styleNode->QueryStringAttribute("isDefault", &isDefault);
			style.isDefault = (isDefault == "true");

			tileLayer.styles[style.identifier] = style;
			if (style.isDefault)
			{
				tileLayer.defaultStyle = style.identifier;
			}
		}

		if (tileLayer.styles.empty())
		{
			WMTSTileLayer::WMTSStyle style;
			style.identifier = "default";
			style.title = "Generated default style";
			style.abstract = "Style was missing in capabilities";
			tileLayer.styles[style.identifier] = style;
		}

		{
			unordered_set<string> uniqueFormats;
			for (TiXmlElement* formatNode = layerNode->FirstChildElement("Format");
				formatNode; formatNode = formatNode->NextSiblingElement("Format"))
			{
				const string format = GETTEXT(formatNode);
				if (uniqueFormats.find(format) != uniqueFormats.end())
				{
					continue;
				}
				tileLayer.format.push_back(format);
				uniqueFormats.insert(format);
			}
		}

		for (TiXmlElement* infoFormatNode = layerNode->FirstChildElement("InfoFormat");
			infoFormatNode; infoFormatNode = infoFormatNode->NextSiblingElement("InfoFormat"))
		{
			//...
		}

		for (TiXmlElement* dimensionNode = layerNode->FirstChildElement("Dimension");
			dimensionNode; dimensionNode = dimensionNode->NextSiblingElement("Dimension"))
		{
			//...
		}

		for (TiXmlElement* tileMatrixSetLinkNode = layerNode->FirstChildElement("TileMatrixSetLink");
			tileMatrixSetLinkNode; tileMatrixSetLinkNode = tileMatrixSetLinkNode->NextSiblingElement("TileMatrixSetLink"))
		{
			WMTSTileLayer::TileMatrixSetLink tileMatrixSetLink;

			TiXmlElement* tileMatrixSetNode = tileMatrixSetLinkNode->FirstChildElement("TileMatrixSet");
			if (tileMatrixSetNode)
			{
				tileMatrixSetLink.tileMatrixSet = GETTEXT(tileMatrixSetNode);
			}

			if (tileMatrixSets.find(tileMatrixSetLink.tileMatrixSet) == tileMatrixSets.end())
			{
				continue;
			}

			const WMTSTileMatrixSet& tileMatrixSet = tileMatrixSets[tileMatrixSetLink.tileMatrixSet];

			for (TiXmlElement* tileMatrixSetLimitsNode = tileMatrixSetLinkNode->FirstChildElement("TileMatrixSetLimits");
				tileMatrixSetLimitsNode; tileMatrixSetLimitsNode = tileMatrixSetLimitsNode->NextSiblingElement("TileMatrixSetLimits"))
			{
				for (TiXmlElement* tileMatrixLimitsNode = tileMatrixSetLimitsNode->FirstChildElement("TileMatrixLimits");
					tileMatrixLimitsNode; tileMatrixLimitsNode = tileMatrixLimitsNode->NextSiblingElement("TileMatrixLimits"))
				{
					WMTSTileLayer::TileMatrixSetLink::TileMatrixLimits limit;

					TiXmlElement* tileMatrixNode = tileMatrixLimitsNode->FirstChildElement("TileMatrix");
					if (tileMatrixNode)
					{
						const string id = GETTEXT(tileMatrixNode);

						bool isValid = false;
						int matrixWidth = -1, matrixHeight = -1;
						for (auto it = tileMatrixSet.tileMatrices.begin(); it != tileMatrixSet.tileMatrices.end(); it++)
						{
							const WMTSTileMatrix& cur = it->second;
							isValid = (cur.identifier == id);
							if (isValid)
							{
								matrixWidth = cur.matrixWidth;
								matrixHeight = cur.matrixHeight;
								break;
							}
						}

						if (isValid)
						{
							limit.minTileRow = limit.maxTileRow = limit.minTileCol = limit.maxTileCol = -1;

							TiXmlElement* minTileRowNode = tileMatrixLimitsNode->FirstChildElement("MinTileRow");
							TiXmlElement* maxTileRowNode = tileMatrixLimitsNode->FirstChildElement("MaxTileRow");
							TiXmlElement* minTileColNode = tileMatrixLimitsNode->FirstChildElement("MinTileCol");
							TiXmlElement* maxTileColNode = tileMatrixLimitsNode->FirstChildElement("MaxTileCol");
							if (minTileRowNode && maxTileRowNode && minTileColNode && maxTileColNode)
							{
								limit.minTileRow = stoi(GETTEXT(minTileRowNode));
								limit.maxTileRow = stoi(GETTEXT(maxTileRowNode));
								limit.minTileCol = stoi(GETTEXT(minTileColNode));
								limit.maxTileCol = stoi(GETTEXT(maxTileColNode));
							}

							isValid = (limit.minTileCol >= 0 && limit.minTileCol < matrixWidth &&
								limit.maxTileCol >= 0 && limit.maxTileCol < matrixWidth &&
								limit.minTileCol <= limit.maxTileCol &&
								limit.minTileRow >= 0 && limit.minTileRow < matrixHeight &&
								limit.maxTileRow >= 0 && limit.maxTileRow < matrixHeight &&
								limit.minTileRow <= limit.maxTileRow);
						}

						if (isValid)
						{
							tileMatrixSetLink.limits[id] = limit;
						}
					}
				}
			}
			
			tileLayer.matrixSetLinks[tileMatrixSetLink.tileMatrixSet] = tileMatrixSetLink;
		}

		for (TiXmlElement* resourceURLNode = layerNode->FirstChildElement("ResourceURL");
			resourceURLNode; resourceURLNode = resourceURLNode->NextSiblingElement("ResourceURL"))
		{
			string format = "", resourceType = "", templ = "";
			resourceURLNode->QueryStringAttribute("format", &format);
			resourceURLNode->QueryStringAttribute("resourceType", &resourceType);
			resourceURLNode->QueryStringAttribute("template", &templ);
			if (format.empty() || resourceType.empty() || templ.empty())
			{
				continue;
			}

			if (internal::ToLower(resourceType) == "tile")
			{
				tileLayer.getTileURLs[format] = templ; // REST
			}
			else if (internal::ToLower(resourceType) == internal::ToLower("FeatureInfo"))
			{
				tileLayer.getFeatureInfoURLs[format] = templ;

				// TODO...
			}
		}

		tileLayers.push_back(tileLayer);
	}

	// 确保所有图层都有一个boundingbox
	for (WMTSTileLayer& tileLayer : tileLayers)
	{
		if (!tileLayer.boundingBox.empty() || DetectTileLayerBoundingBox(tileLayer))
		{
			continue;
		}

		BoundingBox boundingBox;
		boundingBox.crs = "CRS:84";
		boundingBox.bbox = Rectangle(-180, -90, 180, 90);
		tileLayer.boundingBox.push_back(boundingBox);
	}
}

bool WMSCapabilitiesWorker::DetectTileLayerBoundingBox(WMTSTileLayer& tileLayer)
{
	if (tileLayer.matrixSetLinks.empty())
	{
		return false;
	}

	for (auto it = tileLayer.matrixSetLinks.begin(); it != tileLayer.matrixSetLinks.end(); it++)
	{
		const WMTSTileLayer::TileMatrixSetLink& setLink = it->second;

		const auto find = tileMatrixSets.find(setLink.tileMatrixSet);
		if (find == tileMatrixSets.end())
		{
			continue;
		}

		OGRSpatialReference crs;
		if (crs.SetFromUserInput(find->first.c_str()) != OGRERR_NONE)
		{
			continue;
		}

		const char* authName = crs.GetAuthorityName(nullptr);
		const char* authCode = crs.GetAuthorityCode(nullptr);
		if (!authName || !authCode)
		{
			continue;
		}

		// 从瓦片矩阵集中选择最粗糙的瓦片矩阵(分辨率最低的矩阵)
		const auto tileMatrixIt = find->second.tileMatrices.cbegin();
		if (tileMatrixIt == find->second.tileMatrices.end())
		{
			continue;
		}

		const WMTSTileMatrix& tileMatrix = tileMatrixIt->second;
		const double metersPerUnit = (crs.IsGeographic() ? 111319.49079327358 : crs.GetLinearUnits());

		const double pixelSize = (IsTianDiTu() ? tileMatrix.scaleDenominator * 0.0254 / 96 / metersPerUnit : tileMatrix.scaleDenominator * 0.00028 / metersPerUnit);

		const Point2d bottomRight(tileMatrix.topLeft.x + pixelSize * tileMatrix.tileWidth * tileMatrix.matrixWidth,
			tileMatrix.topLeft.y - pixelSize * tileMatrix.tileHeight * tileMatrix.matrixHeight);
		const Rectangle extent(tileMatrix.topLeft, bottomRight);
		
		const BoundingBox boundingBox(string(authName) + ":" + string(authCode), extent);
		tileLayer.boundingBox.push_back(boundingBox);
	}

	return !tileLayer.boundingBox.empty();
}


bool WMSCapabilitiesDownloader::DownloadCapabilitiesXML(const string& originUrl, string& content, string& receiveInfo, const string& proxyUrl, const string& proxyUserName, const string& proxyPassword)
{
	content = receiveInfo = "";
	string url = originUrl;

	if (!internal::IsUrlForWMTS(url))
	{
		URLProcessing::AddQueryParam(url, "Service", "WMS");
		URLProcessing::AddQueryParam(url, "Request", "GetCapabilities");
	}

	if (!GetCapabilities(url, content, receiveInfo, "", proxyUrl, proxyUserName, proxyPassword))
	{
		return false;
	}

	if (internal::StartsWith(content, "<html>") || internal::StartsWith(content, "<HTML>"))
	{
		const size_t startPos = content.find("<Capabilities");
		if (startPos != string::npos)
		{
			content = content.substr(startPos);
		}
	}
	return true;
}

static string RemoveDTD(const string& content)
{
	// 使用libXML读字符串
	const xmlDocPtr doc = xmlReadMemory(content.c_str(), content.size(), nullptr, nullptr, 0);
	if (!doc)
	{
		return content;
	}

	// 删除DTD头
	xmlDtdPtr dtdPtr = doc->intSubset;
	if (dtdPtr)
	{
		xmlUnlinkNode((xmlNodePtr)dtdPtr);
		xmlFree(dtdPtr);
		dtdPtr = nullptr;
	}

	// 写入字符串
	xmlChar* xmlBuffer = nullptr;
	int bufferSize = 0;
	xmlDocDumpFormatMemory(doc, &xmlBuffer, &bufferSize, 1);
	xmlFreeDoc(doc);

	const string strWithoutDTD((const char*)xmlBuffer);
	xmlFree(xmlBuffer);

	return strWithoutDTD;
}

bool WMSCapabilitiesWorker::ParseCapabilities(const string& content, string& errorInfo)
{
	errorInfo = "";
	if (content.empty())
	{
		errorInfo = "Empty capabilities document";
		return false;
	}

	// 检测是否为HTML文件
	if (internal::StartsWith(content, "<html>") || internal::StartsWith(content, "<HTML>"))
	{
		errorInfo = "Starts with <html>";
		return false;
	}

	// TinyXML解析
	TiXmlDocument doc;
	doc.Parse(content.c_str());
	if (doc.Error())
	{
		errorInfo = doc.ErrorDesc();
		return false;
	}

	// 获取根元素
	TiXmlElement* root = doc.RootElement();
	if (!root)
	{
		// 如果没有根元素，可能是因为XML文件头中有DTD声明
		const string strWithoutDTD = RemoveDTD(content);
		doc.Parse(strWithoutDTD.c_str());
		if (doc.Error())
		{
			errorInfo = doc.ErrorDesc();
			return false;
		}
		root = doc.RootElement();
		if (!root)
		{
			errorInfo = "Can not get root element";
			return false;
		}
	}

	// 检查XML文件头
	if (!CheckRootTag(root))
	{
		errorInfo = "Root tag error";
		return false;
	}

	capabilities = WMSCapabilities();

	// 提取"version"
	GetAttribute(root, "version", capabilities.version);

	// 遍历XML
	TiXmlElement* curNode = root->FirstChildElement();
	while (curNode)
	{
		const string tagName = curNode->ValueStr();
		if (tagName == "Service" || tagName == "ows:ServiceProvider" || tagName == "ows:ServiceIdentification")
		{
			ParseService(curNode, capabilities.service);
		}
		else if (tagName == "Capability" || tagName == "ows:OperationsMetadata")
		{
			ParseCapability(curNode, capabilities.capability);
		}
		else if (tagName == "Contents")
		{
			ParseContents(curNode);
		}

		curNode = curNode->NextSiblingElement();
	}

	// 对于WMS服务，把在layers中没有的，但是在capability.layers中的图层，加到layers中
	for (const WMSLayer& layer : capabilities.capability.layers)
	{
		const auto find = (find_if(layers.begin(), layers.end(), [](const WMSLayer& layer) {
				return layer.orderID == 0;
			}) != layers.end());
		if (!find)
		{
			layers.push_back(layer);
		}
	}
	sort(layers.begin(), layers.end(), [](const WMSLayer& layer1, const WMSLayer& layer2) {
			return layer1.orderID < layer2.orderID;
		});

	// 构建layer树
	if (!layerParents.empty())
	{
		layerTrees = LayerTree::GenerateLayerTree(layerParents);
	}

	return true;
}

vector<string> WMSCapabilitiesWorker::GetRootLayerTitles() const
{
	vector<string> result;

	for (const LayerTree& root : layerTrees)
	{
		const int rootID = root.rootOrderID;
		string rootTitle = "";
		if (GetLayerTitleByID(rootID, rootTitle))
		{
			result.push_back(rootTitle);
		}
	}

	for (const WMTSTileLayer& layer : tileLayers)
	{
		result.push_back(layer.title);
	}

	sort(result.begin(), result.end());
	return result;
}

vector<string> WMSCapabilitiesWorker::GetLayerAllTileMatrixSets(const string& layerTitle) const
{
	vector<string> result;
	for (const WMTSTileLayer& layer : tileLayers)
	{
		if (layer.title != layerTitle)
		{
			continue;
		}

		for (const auto& matrixSetLink : layer.matrixSetLinks)
		{
			result.push_back(matrixSetLink.first);
		}
	}
	return result;
}

BoundingBox WMSCapabilitiesWorker::GetLayerBoundingBox4326(const string& layerTitle, const string& tileMatrixSetName) const
{
	BoundingBox result;

	// WMS
	for (const WMSLayer& layer : layers)
	{
		if (layer.title != layerTitle)
		{
			continue;
		}

		const Rectangle exBoundingBox = layer.ex_GeographicBoundingBox;
		const Point2d exBoundingBoxMinPt = exBoundingBox.GetMinPoint();
		const Point2d exBoundingBoxMaxPt = exBoundingBox.GetMaxPoint();
		if (exBoundingBox.IsValid() && exBoundingBoxMinPt.x < exBoundingBoxMaxPt.x &&
			exBoundingBoxMinPt.y < exBoundingBoxMaxPt.y &&
			exBoundingBoxMinPt.x >= -180 && exBoundingBoxMinPt.x <= 180 && exBoundingBoxMinPt.y >= -90 && exBoundingBoxMinPt.y <= 90 &&
			exBoundingBoxMaxPt.x >= -180 && exBoundingBoxMaxPt.x <= 180 && exBoundingBoxMaxPt.y >= -90 && exBoundingBoxMaxPt.y <= 90)
		{
			result.crs = "EPSG:4326";
			result.bbox = exBoundingBox;
			return result;
		}
	}

	// WMTS
	if (tileMatrixSets.find(tileMatrixSetName) == tileMatrixSets.end())
	{
		return result;
	}
	const string tileMatrixCRSName = tileMatrixSets.at(tileMatrixSetName).crs;

	for (const WMTSTileLayer& layer : tileLayers)
	{
		if (layer.title != layerTitle && !layer.boundingBox.empty())
		{
			continue;
		}

		for (const BoundingBox& layerBBox : layer.boundingBox)
		{
			if (!layerBBox.IsValid())
			{
				continue;
			}

			BoundingBox bbox4326("EPSG:4326", Rectangle());
			if (!CSConverter::TransformBoundingBox(layerBBox, bbox4326) || !bbox4326.IsValid())
			{
				continue;
			}

			result.crs = "EPSG:4326";
			result.bbox = bbox4326.bbox;
			return result;
		}
	}

	return result;
}

string WMSCapabilitiesWorker::GetLayerCRS(const string& layerTitle, const string& tileMatrixSetName) const
{
	for (const WMSLayer& layer : layers)
	{
		if (layer.title != layerTitle)
		{
			continue;
		}

		// 如果没有设置CRS，默认使用EPSG:4326
		if (layer.crs.empty())
		{
			return "EPSG:4326";
		}

		// 如果设置了唯一CRS，则返回该唯一CRS
		if (layer.crs.size() == 1)
		{
			return layer.crs[0];
		}

		// 优先查找EPSG:4326，次优先查找EPSG:3857，最后查找第一个有效的坐标系
		if (find(layer.crs.begin(), layer.crs.end(), "EPSG:4326") != layer.crs.end())
		{
			return "EPSG:4326";
		}
		if (find(layer.crs.begin(), layer.crs.end(), "CRS:84") != layer.crs.end() || 
			find(layer.crs.begin(), layer.crs.end(), "EPSG:3857") != layer.crs.end())
		{
			return "EPSG:3857";
		}
		for (const string& crsString : layer.crs)
		{
			OGRSpatialReference crs;
			if (crs.SetFromUserInput(crsString.c_str()) != OGRERR_NONE)
			{
				continue;
			}

			const char* authName = crs.GetAuthorityName(nullptr);
			const char* authCode = crs.GetAuthorityCode(nullptr);
			if (!authName || !authCode)
			{
				continue;
			}

			return string(authName) + ":" + string(authCode);
		}

		return "EPSG:4326";
	}

	if (tileMatrixSets.find(tileMatrixSetName) == tileMatrixSets.end())
	{
		return "";
	}
	return tileMatrixSets.at(tileMatrixSetName).crs;
}

bool WMSCapabilitiesWorker::IsWMTSLayer(const string& layerTitle) const
{
	for (const WMTSTileLayer& layer : tileLayers)
	{
		if (layer.title == layerTitle)
		{
			return true;
		}
	}
	return false;
}

TileMatrixLimits WMSCapabilitiesWorker::GetTileMatrixLimits(const string& layerTitle, const string& tileMatrixSetName, int level) const
{
	TileMatrixLimits result;
	for (const WMTSTileLayer& layer : tileLayers)
	{
		if (layer.title != layerTitle)
		{
			continue;
		}
		const auto find = layer.matrixSetLinks.find(tileMatrixSetName);
		if (find == layer.matrixSetLinks.end())
		{
			break;
		}
		const WMTSTileLayer::TileMatrixSetLink& setLink = find->second;
		const auto limitsFind = setLink.limits.find(to_string(level));
		if (limitsFind == setLink.limits.end())
		{
			break;
		}
		result = limitsFind->second;
		break;
	}
	return result;
}

string WMSCapabilitiesWorker::ExtractToken(const string& url) const
{
	string result = "";
	if (URLProcessing::HasQueryParam(url, "token", result))
	{
		return result;
	}
	if (URLProcessing::HasQueryParam(url, "tk", result))
	{
		return result;
	}
	return "";
}

vector<TileInfo> WMSCapabilitiesWorker::CalculateTilesInfo(const string& layerTitle, const string& tileMatrixSetName, const string& format, const string& style, const BoundingBox& viewExtent, const string& url, bool useXlinkHref) const
{
	const string tileCRS = GetLayerCRS(layerTitle, tileMatrixSetName);
	if (tileCRS.empty())
	{
		return {};
	}

	// 把传入的viewExtent转换到瓦片的CRS
	BoundingBox viewExtentCRS = viewExtent;
	if (viewExtentCRS.crs != tileCRS)
	{
		viewExtentCRS.crs = tileCRS;
		if (!CSConverter::TransformBoundingBox(viewExtent, viewExtentCRS))
		{
			return {};
		}
	}

	// 计算合适的level层级
	int level = CalculateLevel(layerTitle, tileMatrixSetName, viewExtentCRS.bbox);
	if (level < 0 || level >= 25)
	{
		return {};
	}

	if (IsWMTSLayer(layerTitle))
	{
		level = (level < 2 ? 2 : level);

		// 获取到对应level的TileMatrix
		if (tileMatrixSets.find(tileMatrixSetName) == tileMatrixSets.end())
		{
			return {};
		}

		const WMTSTileMatrixSet& tileMatrixSet = tileMatrixSets.at(tileMatrixSetName);
		const string tileMatrixSetName = tileMatrixSet.identifier;
		const WMTSTileMatrix* tileMatrix = tileMatrixSet.GetTileMatrix(to_string(level));
		if (!tileMatrix)
		{
			return {};
		}

		const double tileWidthLength = tileMatrix->tileWidth * tileMatrix->pixelSize;
		const double tileHeightLength = tileMatrix->tileHeight * tileMatrix->pixelSize;

		int startX = max(static_cast<int>((viewExtent.bbox.GetMinPoint().x - tileMatrix->topLeft.x) / tileWidthLength), 0);
		int endX = static_cast<int>((viewExtent.bbox.GetMaxPoint().x - tileMatrix->topLeft.x) / tileWidthLength);
		int startY = max(static_cast<int>((tileMatrix->topLeft.y - viewExtent.bbox.GetMaxPoint().y) / tileHeightLength), 0);
		int endY = static_cast<int>((tileMatrix->topLeft.y - viewExtent.bbox.GetMinPoint().y) / tileHeightLength);
		endX = min(endX, startX + tileMatrix->matrixWidth - 1);
		endY = min(endY, startY + tileMatrix->matrixHeight - 1);

		// 用limit进一步限制
		const TileMatrixLimits limits = GetTileMatrixLimits(layerTitle, tileMatrixSetName, level);
		if (limits.IsValid(level))
		{
			startX = max(startX, limits.minTileCol);
			endX = min(endX, limits.maxTileCol);
			startY = max(startY, limits.minTileRow);
			endY = min(endY, limits.maxTileRow);
		}
		if (startX < 0 || endX < 0 || startY < 0 || endY < 0 || startX > endX || startY > endY)
		{
			return {};
		}

		TileInfo tileInfo;
		tileInfo.level = level;
		tileInfo.numWidthPixels = tileMatrix->tileWidth;
		tileInfo.numHeightPixels = tileMatrix->tileHeight;
		tileInfo.layerName = GetWMTSLayerName(layerTitle);
		tileInfo.layerTitle = layerTitle;
		tileInfo.tileMatrixSet = tileMatrixSetName;
		tileInfo.format = format;
		tileInfo.style = style;

		vector<TileInfo> tiles;
		for (int tileRowIndex = startY; tileRowIndex <= endY; tileRowIndex++)
		{
			for (int tileColIndex = startX; tileColIndex <= endX; tileColIndex++)
			{
				tileInfo.row = tileRowIndex;
				tileInfo.col = tileColIndex;
				tileInfo.leftTopPtX = tileMatrix->topLeft.x + tileColIndex * tileWidthLength;
				tileInfo.leftTopPtY = tileMatrix->topLeft.y - tileRowIndex * tileHeightLength;
				tileInfo.bbox = BoundingBox(tileMatrixSet.crs, Rectangle(tileInfo.leftTopPtX, tileInfo.leftTopPtY,
					tileInfo.leftTopPtX + tileWidthLength, tileInfo.leftTopPtY - tileHeightLength));
				tileInfo.filePath = CreateWMTSFilePath(tileInfo);
				tileInfo.url = CreateWMTSGetTileUrl(url, tileInfo, useXlinkHref);
				tiles.push_back(tileInfo);
			}
		}
		return tiles;
	}

	// WMS
	constexpr static int pixelWidth = 1600;							// 图片的宽度像素数
	constexpr static int pixelHeight = pixelWidth * 1080 / 1920;	// 图片的高度像素数

	TileInfo tileInfo;
	tileInfo.level = 0;
	tileInfo.row = tileInfo.col = 0;
	tileInfo.format = format;
	tileInfo.style = style;
	tileInfo.layerName = GetWMSLayerName(layerTitle);
	tileInfo.layerTitle = layerTitle;
	tileInfo.leftTopPtX = viewExtentCRS.bbox.GetMinPoint().x;
	tileInfo.leftTopPtY = viewExtentCRS.bbox.GetMaxPoint().y;
	tileInfo.bbox = viewExtentCRS;
	tileInfo.numWidthPixels = pixelWidth;
	tileInfo.numHeightPixels = pixelHeight;
	tileInfo.filePath = CreateWMSFilePath(tileInfo);
	tileInfo.url = CreateWMSGetTileUrl(url, tileInfo, useXlinkHref);
	return { tileInfo };
}

string WMSCapabilitiesWorker::GetWMSLayerName(const string& layerTitle) const
{
	for (const WMSLayer& layer : layers)
	{
		if (layer.title == layerTitle)
		{
			return layer.name;
		}
	}
	return "";
}

string WMSCapabilitiesWorker::GetWMTSLayerName(const string& layerTitle) const
{
	for (const WMTSTileLayer& layer : tileLayers)
	{
		if (layer.title == layerTitle)
		{
			return layer.identifier;
		}
	}
	return "";
}

vector<string> WMSCapabilitiesWorker::GetLayerFormats(const string& layerTitle) const
{
	for (const WMSLayer& layer : layers)
	{
		if (layer.title != layerTitle)
		{
			continue;
		}

		vector<string> result;
		for (const WMSLayerStyle& style : layer.style)
		{
			for (const WMSLayerStyle::WMSLayerStyleLegendUrl& legendUrl : style.legendUrl)
			{
				result.push_back(legendUrl.format);
			}
		}

		// 在layer中没找到format，则在getMap中继续找
		if (result.empty())
		{
			result = capabilities.capability.request.getMap.format;
		}
		return result;
	}

	for (const WMTSTileLayer& layer : tileLayers)
	{
		if (layer.title != layerTitle)
		{
			continue;
		}
		return layer.format;
	}

	return {};
}

vector<string> WMSCapabilitiesWorker::GetLayerStyles(const string& layerTitle) const
{
	for (const WMSLayer& layer : layers)
	{
		if (layer.title != layerTitle)
		{
			continue;
		}

		vector<string> result;
		for (const WMSLayerStyle& style : layer.style)
		{
			result.push_back(style.name);
		}
		return result;
	}

	for (const WMTSTileLayer& layer : tileLayers)
	{
		if (layer.title != layerTitle)
		{
			continue;
		}

		vector<string> result;
		for (const auto& pair : layer.styles)
		{
			result.push_back(pair.first);
		}

		return result;
	}

	return {};
}

bool WMSCapabilitiesWorker::IsTianDiTu() const
{
	if (capabilities.capability.request.getTile.dcpType.size() == 1)
	{
		const string url = capabilities.capability.request.getTile.dcpType[0].get;
		if (internal::ToLower(url).find("tianditu") != string::npos)
		{
			return true;
		}
		return false;
	}
	return false;
}

bool WMSCapabilitiesWorker::GetLayerTitleByID(int layerID, string& layerTitle) const
{
	for (const WMSLayer& layer : layers)
	{
		if (layer.orderID == layerID)
		{
			layerTitle = layer.title;
			return true;
		}
	}

	return false;
}

bool WMSCapabilitiesWorker::GetLayerIDByTitle(const string& layerTitle, int& layerID) const
{
	for (const WMSLayer& layer : layers)
	{
		if (layer.title == layerTitle)
		{
			layerID = layer.orderID;
			return true;
		}
	}
	return false;
}

vector<string> WMSCapabilitiesWorker::GetChildrenLayerTitles(const string& layerTitle) const
{
	for (const WMSLayer& layer : layers)
	{
		if (layer.title != layerTitle)
		{
			continue;
		}

		vector<string> childrenLayerTitles;
		for (const WMSLayer& subLayer : layer.layer)
		{
			childrenLayerTitles.push_back(subLayer.title);
		}

		sort(childrenLayerTitles.begin(), childrenLayerTitles.end());
		return childrenLayerTitles;
	}

	return {};
}

int WMSCapabilitiesWorker::CalculateLevel(const string& layerTitle, const string& tileMatrixSetName, const Rectangle& viewExtentCRS) const
{
	int result = -1;
	if (!viewExtentCRS.IsValid())
	{
		return result;
	}

	const double lengthX = viewExtentCRS.GetMaxPoint().x - viewExtentCRS.GetMinPoint().x;
	const double lengthY = viewExtentCRS.GetMaxPoint().y - viewExtentCRS.GetMinPoint().y;
	const double viewExtentHeight = (lengthX > lengthY ? lengthY : lengthX);
	const double viewExtentWidth = (lengthX > lengthY ? lengthX : lengthY);
	constexpr static int maxNumTileRowsInScreen = 2; // 屏幕中高度方向最多瓦片数
	constexpr static int maxNumTileColsInScreen = 8; // 屏幕中宽度方向最多瓦片数

	for (const WMSLayer& layer : layers)
	{
		if (layer.title != layerTitle)
		{
			continue;
		}

		return 0; // WMS图层默认返回0
	}

	for (const WMTSTileLayer& layer : tileLayers)
	{
		if (layer.title != layerTitle)
		{
			continue;
		}

		if (layer.matrixSetLinks.find(tileMatrixSetName) == layer.matrixSetLinks.end() || tileMatrixSets.find(tileMatrixSetName) == tileMatrixSets.end())
		{
			continue;
		}
		const WMTSTileMatrixSet& tileMatrixSet = tileMatrixSets.at(tileMatrixSetName);

		// 根据pixelSize从大往小遍历
		for (auto it = tileMatrixSet.tileMatrices.rbegin(); it != tileMatrixSet.tileMatrices.rend(); it++)
		{
			const double pixelSize = it->first;
			const int tileWidthNumPixels = it->second.tileWidth;
			const int tileHeightNumPixels = it->second.tileHeight;
			if (viewExtentHeight > pixelSize * tileHeightNumPixels * maxNumTileRowsInScreen ||
				viewExtentWidth > pixelSize * tileWidthNumPixels * maxNumTileColsInScreen)
			{
				string levelIDString = it->second.identifier;
				if (levelIDString.empty())
				{
					continue;
				}

				const size_t lastColonPos = levelIDString.find_last_of(':');
				if (lastColonPos != string::npos && lastColonPos < levelIDString.size() - 1) // 例如"EPSG:4326:1", 得到"1"
				{
					levelIDString = levelIDString.substr(lastColonPos + 1);
				}

				try
				{
					result = stoi(levelIDString);
				}
				catch (...)
				{
					result = -1;
				}

				if (result < 0 || result > 25) // level范围[0, 25]
				{
					result = -1;
					continue;
				}
				return result;
			}
		}
	}
	return result;
}

string WMSCapabilitiesWorker::CreateWMTSGetTileUrl(const string& url, const TileInfo& tileInfo, bool useXlinkHref) const
{
	if (IsKVP())
	{
		string requestUrl = "";
		if (useXlinkHref && !capabilities.capability.request.getTile.dcpType.empty())
		{
			requestUrl = capabilities.capability.request.getTile.dcpType[0].get;
		}
		else
		{
			requestUrl = URLProcessing::GetRequestBaseUrl(url);
		}
		URLProcessing::AddQueryParam(requestUrl, "SERVICE", "WMTS");
		URLProcessing::AddQueryParam(requestUrl, "REQUEST", "GetTile");
		URLProcessing::AddQueryParam(requestUrl, "VERSION", capabilities.version);
		URLProcessing::AddQueryParam(requestUrl, "LAYER", EscapeString(tileInfo.layerName));
		if (!tileInfo.style.empty())
		{
			URLProcessing::AddQueryParam(requestUrl, "STYLE", EscapeString(tileInfo.style));
		}
		URLProcessing::AddQueryParam(requestUrl, "FORMAT", EscapeString(tileInfo.format));
		URLProcessing::AddQueryParam(requestUrl, "TILEMATRIXSET", EscapeString(tileInfo.tileMatrixSet));
		
		const string tileMatrixName = GetTileMatrixName(tileInfo.layerTitle, tileInfo.tileMatrixSet, tileInfo.level);
		URLProcessing::AddQueryParam(requestUrl, "TILEMATRIX", EscapeString(tileMatrixName.c_str()));
		URLProcessing::AddQueryParam(requestUrl, "TILEROW", to_string(tileInfo.row));
		URLProcessing::AddQueryParam(requestUrl, "TILECOL", to_string(tileInfo.col));

		const string token = ExtractToken(url);
		if (!token.empty())
		{
			URLProcessing::AddQueryParam(requestUrl, "tk", token);
		}
		return requestUrl;
	}

	// REST
	for (const WMTSTileLayer& layer : tileLayers)
	{
		if (layer.title != tileInfo.layerTitle)
		{
			continue;
		}

		if (layer.getTileURLs.find(tileInfo.format) == layer.getTileURLs.end())
		{
			continue;
		}

		string requestUrl = layer.getTileURLs.at(tileInfo.format);
		URLProcessing::ReplaceQueryParam(requestUrl, "{layer}", EscapeString(tileInfo.layerTitle));
		URLProcessing::ReplaceQueryParam(requestUrl, "{style}", EscapeString(tileInfo.style));
		URLProcessing::ReplaceQueryParam(requestUrl, "{tilematrixset}", EscapeString(tileInfo.tileMatrixSet));
		URLProcessing::ReplaceQueryParam(requestUrl, "{tilematrix}", to_string(tileInfo.level));
		URLProcessing::ReplaceQueryParam(requestUrl, "{tilerow}", to_string(tileInfo.row));
		URLProcessing::ReplaceQueryParam(requestUrl, "{tilecol}", to_string(tileInfo.col));

		return requestUrl;
	}
	
	return "";
}

string WMSCapabilitiesWorker::CreateWMSGetTileUrl(const string& url, const TileInfo& tileInfo, bool useXlinkHref) const
{
	constexpr static int dpi = 96;
	if (tileInfo.bbox.crs.empty())
	{
		return "";
	}

	OGRSpatialReference crs;
	if (crs.SetFromUserInput(tileInfo.bbox.crs.c_str()) != OGRERR_NONE)
	{
		return "";
	}

	string requestUrl = "";
	if (useXlinkHref && !capabilities.capability.request.getMap.dcpType.empty())
	{
		requestUrl = capabilities.capability.request.getMap.dcpType[0].get;
	}
	else
	{
		requestUrl = URLProcessing::GetRequestBaseUrl(url);
	}
	URLProcessing::AddQueryParam(requestUrl, "SERVICE", "WMS");
	URLProcessing::AddQueryParam(requestUrl, "VERSION", capabilities.version);
	URLProcessing::AddQueryParam(requestUrl, "REQUEST", "GetMap");

	const bool isAxisInverted = crs.EPSGTreatsAsLatLong() || crs.EPSGTreatsAsNorthingEasting();
	const Rectangle bbox = (isAxisInverted ? tileInfo.bbox.bbox.Invert() : tileInfo.bbox.bbox);
	URLProcessing::AddQueryParam(requestUrl, "BBOX", bbox.ToString());

	const string crsKey = ((capabilities.version == "1.3.0" || capabilities.version == "1.3") ? "CRS" : "SRS");
	URLProcessing::AddQueryParam(requestUrl, crsKey, tileInfo.bbox.crs);

	URLProcessing::AddQueryParam(requestUrl, "WIDTH", to_string(tileInfo.numWidthPixels));
	URLProcessing::AddQueryParam(requestUrl, "HEIGHT", to_string(tileInfo.numHeightPixels));
	URLProcessing::AddQueryParam(requestUrl, "LAYERS", EscapeString(tileInfo.layerName));
	if (!tileInfo.style.empty())
	{
		URLProcessing::AddQueryParam(requestUrl, "STYLES", EscapeString(tileInfo.style));
	}
	URLProcessing::AddQueryParam(requestUrl, "FORMAT", EscapeString(tileInfo.format));

	URLProcessing::AddQueryParam(requestUrl, "DPI", to_string(dpi));
	URLProcessing::AddQueryParam(requestUrl, "MAP_RESOLUTION", to_string(dpi));
	URLProcessing::AddQueryParam(requestUrl, "FORMAT_OPTIONS", "dpi:" + to_string(dpi));

	if (tileInfo.style == "image/x-jpegorpng" || 
		(internal::ToLower(tileInfo.style).find("jpeg") == string::npos && internal::ToLower(tileInfo.style).find("jpg") == string::npos))
	{
		URLProcessing::AddQueryParam(requestUrl, "TRANSPARENT", "TRUE");
	}
	return requestUrl;
}

bool WMSCapabilitiesWorker::IsKVP() const
{
	// 如果“GetTile的dctType为空”或者“GetTile的allowedEncodings非空但又不存在"KVP"”，则不是KVP
	const WMSCapabilitiesCapability::CapabilityRequest::CapabilityRequestOperation& getTileOperation = capabilities.capability.request.getTile;
	const vector<string>& allowedEncodings = getTileOperation.allowedEncodings;
	const bool isAllowEncodingEmpty = allowedEncodings.empty();
	const bool isExistKVP = (find(allowedEncodings.begin(), allowedEncodings.end(), "KVP") != allowedEncodings.end());
	if (getTileOperation.dcpType.empty() || (!isAllowEncodingEmpty && !isExistKVP))
	{
		return false;
	}
	return true;
}

string WMSCapabilitiesWorker::CreateWMTSFilePath(const TileInfo& tileInfo) const
{
	const string saveDirPath = GetTempDirPath() + "/";
	
	string fileName = GetStringMD5(tileInfo.layerTitle + "_" + tileInfo.tileMatrixSet) + "_" + to_string(tileInfo.level) + "_" + to_string(tileInfo.row) + "_" + to_string(tileInfo.col);
	if (tileInfo.format.find("webp") != string::npos)
	{
		fileName += ".webp";
	}
	else if (tileInfo.format.find("jpg") != string::npos || tileInfo.format.find("jpeg") != string::npos)
	{
		fileName += ".jpg";
	}
	else if (tileInfo.format.find("tif") != string::npos || tileInfo.format.find("tiff") != string::npos)
	{
		fileName += ".tif";
	}
	else
	{
		fileName += ".png";
	}

	return saveDirPath + fileName;
}

string WMSCapabilitiesWorker::CreateWMSFilePath(const TileInfo& tileInfo) const
{
	const string saveDirPath = GetTempDirPath() + "/";

	string fileName = GetStringMD5(tileInfo.layerTitle + "_" + tileInfo.layerName) + "_" + tileInfo.bbox.bbox.ToString();
	if (tileInfo.format.find("webp") != string::npos)
	{
		fileName += ".webp";
	}
	else if (tileInfo.format.find("jpg") != string::npos || tileInfo.format.find("jpeg") != string::npos)
	{
		fileName += ".jpg";
	}
	else if (tileInfo.format.find("tif") != string::npos || tileInfo.format.find("tiff") != string::npos)
	{
		fileName += ".tif";
	}
	else
	{
		fileName += ".png";
	}

	return saveDirPath + fileName;
}

string WMSCapabilitiesWorker::GetTileMatrixName(const string& layerTitle, const string& tileMatrixSetName, int level) const
{
	for (const auto& pair1 : tileMatrixSets)
	{
		if (pair1.first != tileMatrixSetName)
		{
			continue;
		}

		for (const auto& pair2 : pair1.second.tileMatrices)
		{
			string tileMatrixName = pair2.second.identifier;
			if (tileMatrixName.empty())
			{
				continue;
			}

			if (tileMatrixName == to_string(level))
			{
				return tileMatrixName;
			}

			const size_t lastColonPos = tileMatrixName.find_last_of(':');
			if (lastColonPos != string::npos && lastColonPos < tileMatrixName.size() - 1) // 例如"EPSG:4326:1", 得到"1"
			{
				tileMatrixName = tileMatrixName.substr(lastColonPos + 1);
			}
			if (tileMatrixName == to_string(level) || tileMatrixName == "0" + to_string(level))
			{
				return pair2.second.identifier;
			}
		}
	}
	return "";
}

bool WMSCapabilitiesWorker::SetCRS(const std::string& crsString, OGRSpatialReference& crs) const
{
	if (crs.SetFromUserInput(crsString.c_str()) == OGRERR_NONE)
	{
		const char* authName = crs.GetAuthorityName(nullptr);
		const char* authCode = crs.GetAuthorityCode(nullptr);
		if (authName && authCode)
		{
			return true;
		}
	}

	// 对于类似"urn:ogc:def:crs:EPSG:6.18:3:3857"的输入字符串
	if (crsString._Starts_with("urn:ogc:def") && internal::ToLower(crsString).find("epsg") != string::npos)
	{
		const vector<string> parts = SplitString(crsString, ':');
		const regex pattern("^[1-9]\\d*$");
		if (!parts.empty() && regex_match(parts.back(), pattern))
		{
			const string epsgCode = "EPSG:" + parts.back();
			if (crs.SetFromUserInput(epsgCode.c_str()) == OGRERR_NONE)
			{
				const char* authName = crs.GetAuthorityName(nullptr);
				const char* authCode = crs.GetAuthorityCode(nullptr);
				if (authName && authCode)
				{
					return true;
				}
			}
		}
	}

	return false;
}

bool TileInfo::IsValid() const
{
	return (level >= 0 && level <= 25 && row >= 0 && col >= 0 && !layerName.empty() && bbox.IsValid());
}
