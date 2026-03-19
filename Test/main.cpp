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

	const std::string baseUrl = GB_STR("https://ovc.catastro.meh.es/Cartografia/WMS/ServidorWMS.aspx?SERVICE=WMS&REQUEST=GETCAPABILITIES");

	std::string capabilitiesXmlUtf8 = "";
	if (!DownloadWmsCapabilities(baseUrl, capabilitiesXmlUtf8) || capabilitiesXmlUtf8.empty())
	{
		return 1;
	}

	ArcGISMapServiceInfo mapServiceInfo;
	const bool success = RequestArcGISServerJson(baseUrl, mapServiceInfo);
	const ArcGISMapServiceInfo* mapServiceInfoPtr = (success ? &mapServiceInfo : nullptr);

	WmsCapabilitiesProperty capabilities;
	if (!ParseWmsCapabilities(capabilitiesXmlUtf8, baseUrl, capabilities, mapServiceInfoPtr))
	{
		return 1;
	}

	WmsTreeNode rootNode;
	if (!BuildWmsLayerTree(capabilities, rootNode))
	{
		return 1;
	}

	const std::string wmsTreeString = rootNode.ToString();
	std::cout << wmsTreeString << std::endl;
	//GB_WriteUtf8ToFile("CapabilitiesLayerTree.txt", wmsTreeString);
	return 0;
}







