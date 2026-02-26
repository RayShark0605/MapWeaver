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

MAPWEAVERCORE_PORT bool ParseWmsCapabilities(const std::string& capabilitiesXmlUtf8, WmsCapabilitiesProperty& outCapabilities, const WmsParserOptions& options = WmsParserOptions());













#endif