#pragma once
#include "Common.h"
#include "WMSLayer.h"
#include "tinyxml.h"

class WMSCapabilitiesDownloader
{
public:
	static bool DownloadCapabilitiesXML(const std::string& url, std::string& content, std::string& receiveInfo, const std::string& proxyUrl = "", const std::string& proxyUserName = "", const std::string& proxyPassword = "");
};

struct WMSCapabilitiesCapability
{
	struct CapabilityRequest
	{
		struct CapabilityRequestOperation
		{
			struct HTTP
			{
				std::string get = "";
				std::string post = "";
			};
			std::vector<std::string> format;
			std::vector<HTTP> dcpType;
			std::vector<std::string> allowedEncodings;
		};

		CapabilityRequestOperation getMap;
		CapabilityRequestOperation getFeatureInfo;
		CapabilityRequestOperation getTile;
		CapabilityRequestOperation getLegendGraphic;
	};

	CapabilityRequest request;
	std::vector<std::string> exceptionFormat;
	std::vector<WMSLayer> layers;
	std::vector<WMTSTileLayer> tileLayers;
	std::unordered_map<std::string, WMTSTileMatrixSet> tileMatrixSets;
};
struct WMSCapabilitiesService
{
	std::string title = "", abstract = "", fees = "", accessConstraints = "", onlineResourceHref = "";
	std::vector<std::string> keywordList;
	unsigned int layerLimit = 0, maxWidth = 0, maxHeight = 0;
};
struct WMSCapabilities
{
	std::string version = "";
	WMSCapabilitiesCapability capability;
	WMSCapabilitiesService service;
};

struct TileInfo
{
	int level = -1;									// 该瓦片所在的层级。WMS默认为0
	int row = -1, col = -1;							// 该瓦片在对应层级的矩阵中的行列号。WMS默认都为0
	int numWidthPixels = 0, numHeightPixels = 0;	// 该瓦片的宽度高度像素数
	double leftTopPtX = 0, leftTopPtY = 0;			// 该瓦片左上角点在瓦片CRS下的坐标
	std::string layerTitle = "", layerName = "";	// 该瓦片所属图层标题和图层名
	std::string tileMatrixSet = "";					// 该瓦片所属的矩阵集名
	std::string url = "";							// 该瓦片的下载链接
	std::string filePath = "";						// 该瓦片的文件路径
	BoundingBox bbox;								// 该瓦片的boundingBox
	bool isDownloaded = false;						// 该瓦片是否已下载
	std::string version = "";						// 版本号，例如"1.0.0"
	std::string style = "";							// 图层渲染样式，例如"default"
	std::string format = "";						// 格式，例如"image/png"

	bool IsValid() const;
};

using TileMatrixLimits = WMTSTileLayer::TileMatrixSetLink::TileMatrixLimits;
class WMSCapabilitiesWorker
{
public:
	WMSCapabilities capabilities;
	std::vector<WMSLayer> layers; // WMS图层
	std::vector<WMTSTileLayer> tileLayers; // WMTS图层
	std::unordered_map<std::string, WMTSTileMatrixSet> tileMatrixSets; // WMTS图层的TileMatrixSet

	// 解析content XML
	bool ParseCapabilities(const std::string& content, std::string& errorInfo); 

	// 获取WMS图层和WMTS图层的所有图层标题
	std::vector<std::string> GetRootLayerTitles() const;

	// 根据图层名（title）获取该图层的所有TileMatrixSets，WMTS图层才有
	std::vector<std::string> GetLayerAllTileMatrixSets(const std::string& layerTitle) const;

	// 获取地图在EPSG:4326下的boundingBox
	BoundingBox GetLayerBoundingBox4326(const std::string& layerTitle, const std::string& tileMatrixSetName) const;

	// 根据图层名（title）和TileMatrixSet名获取它所在的CRS名
	std::string GetLayerCRS(const std::string& layerTitle, const std::string& tileMatrixSetName) const;

	// 是否是WMTS图层
	bool IsWMTSLayer(const std::string& layerTitle) const;

	// 根据图层名（title）、TileMatrixSet名和level等级获取该瓦片矩阵的行列号限制
	TileMatrixLimits GetTileMatrixLimits(const std::string& layerTitle, const std::string& tileMatrixSetName, int level) const;

	// 提取token
	std::string ExtractToken(const std::string& url) const;

	// 根据指定图层名（title）和包络盒计算所需的瓦片信息
	std::vector<TileInfo> CalculateTilesInfo(const std::string& layerTitle, const std::string& tileMatrixSetName, const std::string& format, const std::string& style, const BoundingBox& viewExtent, const std::string& url, bool useXlinkHref = false) const;

	// 根据WMS图层标题获取WMS图层名
	std::string GetWMSLayerName(const std::string& layerTitle) const;

	// 根据WMTS图层标题获取WMTS图层名
	std::string GetWMTSLayerName(const std::string& layerTitle) const;

	// 根据图层名（title）获取它包含的所有格式
	std::vector<std::string> GetLayerFormats(const std::string& layerTitle) const;

	// 根据图层名（title）获取它包含的所有风格
	std::vector<std::string> GetLayerStyles(const std::string& layerTitle) const;

	// 是否是天地图
	bool IsTianDiTu() const;

	// 根据图层ID获取图层标题。仅对WMS图层
	bool GetLayerTitleByID(int layerID, std::string& layerTitle) const;

	// 根据图层标题获取图层ID。仅对WMS图层
	bool GetLayerIDByTitle(const std::string& layerTitle, int& layerID) const;

	// 获取子图层标题。仅对WMS图层
	std::vector<std::string> GetChildrenLayerTitles(const std::string& layerTitle) const;

private:
	int numLayers = -1;
	std::unordered_map<int, int> layerParents; // 每个layer的父layer ID
	std::unordered_map<int, std::vector<std::string>> layerParentNames;
	std::vector<LayerTree> layerTrees;

	std::unordered_map<std::string, bool> layerQueryable;
	
	std::unordered_map<std::string, bool> CRSInvertAxis;

	bool CheckRootTag(TiXmlElement* root);
	bool ExistsAttribute(TiXmlElement* element, const std::string& attributeName);
	bool GetAttribute(TiXmlElement* element, const std::string& attributeName, std::string& value);
	bool GetValue(TiXmlElement* element, std::string& value);
	void ParseService(TiXmlElement* node, WMSCapabilitiesService& service);
	void ParseOnlineResource(TiXmlElement* node, std::string& xref);
	void ParseGet(TiXmlElement* node, std::string& get);
	void ParsePost(TiXmlElement* node, std::string& post);
	void ParseHTTP(TiXmlElement* node, WMSCapabilitiesCapability::CapabilityRequest::CapabilityRequestOperation::HTTP& http);
	void ParseDCPType(TiXmlElement* node, WMSCapabilitiesCapability::CapabilityRequest::CapabilityRequestOperation::HTTP& http);
	void ParseOperation(TiXmlElement* node, WMSCapabilitiesCapability::CapabilityRequest::CapabilityRequestOperation& operation);
	void ParseRequest(TiXmlElement* node, WMSCapabilitiesCapability::CapabilityRequest& request);
	void ParseKeywordList(TiXmlElement* node, std::vector<std::string>& keywordList);
	void ParseKeywords(TiXmlElement* node, std::vector<std::string>& keywords);
	void ParseMetaURL(TiXmlElement* node, WMSLayerMetadataUrl& metaURL);
	void ParseLegendURL(TiXmlElement* node, WMSLayerStyle::WMSLayerStyleLegendUrl& legendURL);
	void ParseStyle(TiXmlElement* node, WMSLayerStyle& style);
	void ParseLayer(TiXmlElement* node, WMSLayer& layer, WMSLayer* parentLayer = nullptr);
	void ParseCapability(TiXmlElement* node, WMSCapabilitiesCapability& capability);
	void ParseContents(TiXmlElement* node);
	bool DetectTileLayerBoundingBox(WMTSTileLayer& tileLayer);

	// 根据图层名（title）和指定区域，计算合适的图层级别
	int CalculateLevel(const std::string& layerTitle, const std::string& tileMatrixSetName, const Rectangle& viewExtentCRS) const;

	// 得到WMTS瓦片的下载链接
	std::string CreateWMTSGetTileUrl(const std::string& url, const TileInfo& tileInfo, bool useXlinkHref) const;

	// 得到WMS的下载链接
	std::string CreateWMSGetTileUrl(const std::string& url, const TileInfo& tileInfo, bool useXlinkHref) const;

	// 是否是KVP请求
	bool IsKVP() const;

	// 得到WMTS瓦片的下载保存路径
	std::string CreateWMTSFilePath(const TileInfo& tileInfo) const;

	// 得到WMS的下载保存路径
	std::string CreateWMSFilePath(const TileInfo& tileInfo) const;

	// 获取原瓦片矩阵名
	std::string GetTileMatrixName(const std::string& layerTitle, const std::string& tileMatrixSetName, int level) const;

	bool SetCRS(const std::string& crsString, OGRSpatialReference& crs) const;
};