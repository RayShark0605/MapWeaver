#pragma once
#include "Common.h"

struct WMSLayerAttribution
{
	std::string title = "";
	std::string xlinkHref = "";

	WMSLayerAttribution() = default;
	WMSLayerAttribution(const std::string& title, const std::string& xlinkHref);

	bool IsValid() const;
};

struct WMSLayerAuthorityUrl
{
	std::string name = "";
	std::string xlinkHref = "";

	WMSLayerAuthorityUrl() = default;
	WMSLayerAuthorityUrl(const std::string& name, const std::string& xlinkHref);

	bool IsValid() const;
};

struct WMSLayerMetadataUrl
{
	std::string format = "";
	std::string type = "";
	std::string xlinkHref = "";

	WMSLayerMetadataUrl() = default;
	WMSLayerMetadataUrl(const std::string& format, const std::string& type, const std::string& xlinkHref);

	bool IsValid() const;
};

struct WMSLayerFeatureListUrl
{
	std::string format = "";
	std::string xlinkHref = "";

	WMSLayerFeatureListUrl() = default;
	WMSLayerFeatureListUrl(const std::string& format, const std::string& xlinkHref);

	bool IsValid() const;
};

struct WMSLayerStyle
{
	std::string name = "";
	std::string title = "";
	std::string abstract = "";

	struct WMSLayerStyleLegendUrl
	{
		std::string format = "";
		std::string xlinkHref = "";
		int width = -1;
		int height = -1;
	};
	std::vector<WMSLayerStyleLegendUrl> legendUrl;

	typedef WMSLayerFeatureListUrl WMSLayerStyleStyleSheetUrl;
	WMSLayerStyleStyleSheetUrl styleSheetUrl;

	typedef WMSLayerFeatureListUrl WMSLayerStyleStyleUrl;
	WMSLayerStyleStyleUrl styleUrl;

	WMSLayerStyle() = default;
	WMSLayerStyle(const std::string& name, const std::string& title = "", const std::string& abstract= "");

	bool IsValid() const;
};

class WMSLayer
{
public:
	int orderID = -1;

	std::string name = ""; // 可能不存在
	std::string title = "";
	std::string abstract = "";
	double minScaleDenominator = 0;
	double maxScaleDenominator = 0;

	std::vector<std::string> keywordList;
	std::vector<std::string> crs;
	std::vector<std::string> identifierAuthority;

	Rectangle ex_GeographicBoundingBox; // 经纬度bounding box
	std::vector<BoundingBox> boundingBox;
	WMSLayerAttribution attribution;
	std::vector<WMSLayerAuthorityUrl> authorityUrl;
	std::vector<WMSLayerMetadataUrl> metadataUrl;
	std::vector<WMSLayerFeatureListUrl> featureListUrl;
	std::vector<WMSLayerStyle> style;
	std::vector<WMSLayer> layer; // 嵌套图层
	
	// WMS图层属性
	bool queryable = false; // 是否支持GetFeatureInfo操作
	unsigned int cascaded = 0; // 图层被级联地图服务器重新传输的次数
	bool opaque = false; // 地图数据是否全部或大部分不透明
	bool noSubsets = false; // WMS是否只能绘制整个地图边界框
	unsigned int fixedWidth = 0; // 若非0, 则表示地图有固定宽度, WMS不能更改
	unsigned int fixedHeight = 0; // 若非0, 则表示地图有固定宽度, WMS不能更改

	WMSLayer() = default;
	WMSLayer(int orderID, const std::string& name, const std::string& title, const std::string& abstract = "");

	// 在当前Layer和递归地查找所有子Layer中，查找orderID为layerID的图层，返回layerTitle
	bool GetLayerTitleByID(int layerID, std::string& layerTitle) const;

	std::vector<const WMSLayer*> GetAllLayers() const;

	bool IsValid() const;
};

struct WMTSTileLayer
{
	enum TileMode { WMTS, WMSC, XYZ };
	struct WMTSStyle
	{
		struct WMTSLegendURL
		{
			std::string format = "", href = "";
			double minScale = 0, maxScale = 0;
			int width = 0, height = 0;
		};

		std::string identifier = "", title = "", abstract = "";
		std::vector<std::string> keywords;
		bool isDefault = false;
		std::vector<WMTSLegendURL> legendURLs;
	};
	struct TileMatrixSetLink
	{
		struct TileMatrixLimits
		{
			std::string tileMatrix = "";
			int minTileRow = -1, maxTileRow = -1, minTileCol = -1, maxTileCol = -1;

			bool IsValid() const;
			bool IsValid(int level) const;
		};

		std::string tileMatrixSet = "";
		std::unordered_map<std::string, TileMatrixLimits> limits;
	};

	TileMode tileMode;
	std::string identifier = "", title = "", abstract = "", defaultStyle = "";
	std::vector<std::string> keywordList, format, infoFormats;
	std::vector<BoundingBox> boundingBox;
	int dpi = -1; // -1表示位置DPI
	std::unordered_map<std::string, WMTSStyle> styles;
	std::unordered_map<std::string, TileMatrixSetLink> matrixSetLinks;
	std::unordered_map<std::string, std::string> getTileURLs, getFeatureInfoURLs;
};

struct WMTSTileMatrix
{
	std::string identifier = "", title = "", abstract = "";
	std::vector<std::string> keywordList;
	double scaleDenominator = 0, pixelSize = 0;
	Point2d topLeft;
	int tileWidth = 0, tileHeight = 0, matrixWidth = 0, matrixHeight = 0;
};

struct WMTSTileMatrixSet
{
	std::string identifier = "", title = "", abstract = "", crs = "", wkScaleSet = "";
	std::vector<std::string> keywordList;
	std::map<double, WMTSTileMatrix> tileMatrices;

	// 根据标识符（一般是level）获取对应的WMTSTileMatrix
	const WMTSTileMatrix* GetTileMatrix(const std::string& identifier) const;
};

struct LayerTree
{
	int rootOrderID = -1;
	std::vector<LayerTree> subLayers;

	LayerTree();
	LayerTree(int rootOrderID);

	// layerParents表示每个layer的父layer ID
	static std::vector<LayerTree> GenerateLayerTree(const std::unordered_map<int, int>& layerParents);

	void SortRecursive();
};


