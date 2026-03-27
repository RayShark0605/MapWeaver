#ifndef MAP_WEAVER_CORE_H
#define MAP_WEAVER_CORE_H

#include "MapWeaverPort.h"
#include "GB_Network.h"
#include "Geometry/GB_Polygon.h"
#include "MapLayer.h"
#include "ArcGISMapServiceInfo.h"
#include <cstddef>
#include <string>
#include <vector>

MAPWEAVERCORE_PORT bool IsUrlForWMTS(const std::string& urlUtf8);

MAPWEAVERCORE_PORT bool DownloadWmsCapabilities(const std::string& urlUtf8, std::string& outCapabilitiesXmlUtf8, const GB_NetworkRequestOptions& options = GB_NetworkRequestOptions());

MAPWEAVERCORE_PORT bool RequestArcGISServerJson(const std::string& urlUtf8, ArcGISMapServiceInfo& mapServiceInfo, const GB_NetworkRequestOptions& options = GB_NetworkRequestOptions());

struct WmsParserOptions
{
	bool ignoreAxisOrientation = false;
	bool invertAxisOrientation = false;
};

MAPWEAVERCORE_PORT bool ParseWmsCapabilities(const std::string& capabilitiesXmlUtf8, const std::string& baseUrl, WmsCapabilitiesProperty& outCapabilities, const ArcGISMapServiceInfo* arcGISMapServiceInfo = nullptr, const WmsParserOptions& options = WmsParserOptions());

struct BuildLayerTreeOptions
{
	bool ignoreUniqueChildNode = true;
};

struct WmsTreeNode
{
	enum class NodeType
	{
		Unknown = 0,
		Root,
		Layer,
		WmtsTileMatrixSet,
		Style,
		Format
	};
	NodeType nodeType = NodeType::Unknown;
	std::string textUtf8 = "";
	std::string nameUtf8 = "";
	std::vector<WmsTreeNode> children;
	std::string uidUtf8 = "";

	MAPWEAVERCORE_PORT std::string ToString(const std::string& indentStringUtf8 = "----") const;
};

MAPWEAVERCORE_PORT bool BuildWmsLayerTree(const WmsCapabilitiesProperty& capabilities, WmsTreeNode& rootNode, const BuildLayerTreeOptions& options = BuildLayerTreeOptions());

struct MapRequestItem
{
	std::string urlUtf8 = "";
	GeoBoundingBox boundingBox;
	int zoomLevel = -1;
	size_t rowIndex = 0, colIndex = 0;
	std::string uidUtf8 = "";
};

struct MapRenderTargetOptions
{
	enum class ResolutionMode
	{
		Auto = 0,              // 自动：优先按输出像素尺寸推导目标分辨率
		ByZoomLevel,           // 直接指定 zoom/lod
		ByResolution,          // 直接指定目标分辨率（目标 CRS 单位 / 像素）
		ByScaleDenominator,    // 指定比例尺分母
		ByOutputImageSize      // 由输出图像尺寸 + BBOX 反推目标分辨率
	};

	enum class LodSelectMode
	{
		Nearest = 0,           // 选最接近的 lod
		NotCoarserThanTarget,  // 不允许比目标更粗，宁可更细一些
		NotFinerThanTarget     // 不允许比目标更细，宁可更粗一些
	};

	enum class WmsImageSizeMode
	{
		ByResolution = 0,      // 先确定目标分辨率，再反算 width/height
		ExactWidthHeight,      // 直接使用 outputImageWidth / outputImageHeight
		FixedLongEdge,         // 固定长边像素，短边按比例算
		FitWithinMaxSize       // 约束在 maxOutputImageWidth / maxOutputImageHeight 内
	};

	ResolutionMode resolutionMode = ResolutionMode::Auto;
	LodSelectMode lodSelectMode = LodSelectMode::Nearest;
	WmsImageSizeMode wmsImageSizeMode = WmsImageSizeMode::ByResolution;

	// ByZoomLevel
	int zoomLevel = -1;

	// ByResolution
	double targetResolution = 0; // 目标 CRS 单位 / 像素

	// ByScaleDenominator
	double scaleDenominator = 0;
	double renderingPixelSizeMeters = 0.00028; // OGC 标准像素尺寸

	// ByOutputImageSize / ExactWidthHeight
	size_t outputImageWidth = 0;
	size_t outputImageHeight = 0;

	// FixedLongEdge
	size_t longEdgePixels = 0;

	// FitWithinMaxSize
	size_t maxOutputImageWidth = 4096;
	size_t maxOutputImageHeight = 4096;

	bool keepAspectRatio = true;
};

struct BuildVisibleMapRequestItemsInput
{
	MapTileMode mapType;
	const WmsCapabilitiesProperty* capabilities = nullptr;
	std::string mapServiceUrlUtf8 = "";
	std::string layerNameUtf8 = "";
	std::string styleUtf8 = "";
	std::string tileMatrixSetUtf8 = "";
	std::string formatUtf8 = "";
	int minZoomLevel = -1;
	int maxZoomLevel = -1;

	std::string requestAreaWkt = "";
	GB_Polygon requestAreaPolygon;

	MapRenderTargetOptions renderTarget;

};

MAPWEAVERCORE_PORT std::vector<MapRequestItem> BuildVisibleMapRequestItems(const BuildVisibleMapRequestItemsInput& input, bool* success = nullptr);

//MAPWEAVERCORE_PORT bool TryExportWkt2WithCustomTransverseMercatorAreaBbox(const std::string& inputWkt, std::string& outputWkt2, bool* areaBboxInjected = nullptr);

//MAPWEAVERCORE_PORT bool GetCartesianExtents(const std::string& wkt, double& minX, double& minY, double& maxX, double& maxY);





#endif