#include "../MapWeaverCore/include/MapWeaverCore.h"
#include "../MapWeaverCore/include/GeoCrsManager.h"
#include "GB_Logger.h"
#include "GB_FileSystem.h"
#include "GB_Interval.h"
#include "GB_DateTime.h"
#include "GB_IO.h"
#include <iostream>

int main(int argc, char* argv[])
{
	GB_SetConsoleEncodingToUtf8();
	GB_SetLogToConsole(true);

	const std::string baseUrl = GB_STR("https://demo.cubewerx.com/cubewerx/cubeserv/simple?version=1.3.0&service=WMS&REQUEST=GetCapabilities");

	std::string capabilitiesXmlUtf8 = "";
	if (!DownloadWmsCapabilities(baseUrl, capabilitiesXmlUtf8) || capabilitiesXmlUtf8.empty())
	{
		return 1;
	}

	WmsCapabilitiesProperty capabilities;
	if (!ParseWmsCapabilities(capabilitiesXmlUtf8, baseUrl, capabilities))
	{
		return 1;
	}

	std::vector<WmsTreeNode> outLayerTree;
	bool success = BuildWmsLayerTree(capabilities, outLayerTree);
	return 0;
}







