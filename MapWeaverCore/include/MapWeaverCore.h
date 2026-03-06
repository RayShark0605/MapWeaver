#ifndef MAP_WEAVER_CORE_H
#define MAP_WEAVER_CORE_H

#include "MapWeaverPort.h"
#include "GB_Network.h"
#include "MapLayer.h"
#include <string>

MAPWEAVERCORE_PORT bool IsUrlForWMTS(const std::string& urlUtf8);

MAPWEAVERCORE_PORT bool DownloadWmsCapabilities(const std::string& urlUtf8, std::string& outCapabilitiesXmlUtf8, const GB_NetworkRequestOptions& options = GB_NetworkRequestOptions());

struct WmsParserOptions
{
	bool ignoreAxisOrientation = false;
	bool invertAxisOrientation = false;
};

MAPWEAVERCORE_PORT bool ParseWmsCapabilities(const std::string& capabilitiesXmlUtf8, const std::string& baseUrl, WmsCapabilitiesProperty& outCapabilities, const WmsParserOptions& options = WmsParserOptions());

struct BuildLayerTreeOptions
{
	std::string defaultImageFormat = "image/png";
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


#endif