#ifndef MAP_WEAVER_CORE_H
#define MAP_WEAVER_CORE_H

#include "MapWeaverPort.h"
#include "GB_Network.h"
#include "Geometry/GB_Polygon.h"
#include "MapLayer.h"
#include "ArcGISMapServiceInfo.h"
#include <string>

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





};

MAPWEAVERCORE_PORT std::vector<MapRequestItem> BuildVisibleMapRequestItems(const BuildVisibleMapRequestItemsInput& input, bool* success = nullptr);







#endif