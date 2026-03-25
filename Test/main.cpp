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

	GB_NetworkRequestOptions networkOptions;
	networkOptions.verifyTlsHost = false;
	networkOptions.verifyTlsPeer = false;

	const std::string url = "http://localhost:8080/geoserver/gwc/service/wmts?service=WMTS&version=1.1.1&request=GetCapabilities";
	
	std::string capabilitiesXmlUtf8 = "";
	const bool downloadXmlSuccess = DownloadWmsCapabilities(url, capabilitiesXmlUtf8, networkOptions);
	if (!downloadXmlSuccess)
	{
		std::cerr << "Failed to download capabilities XML." << std::endl;
		return -1;
	}
	
	WmsCapabilitiesProperty capabilities;
	const bool parseSuccess = ParseWmsCapabilities(capabilitiesXmlUtf8, url, capabilities);
	if (!parseSuccess)
	{
		std::cerr << "Failed to parse capabilities XML." << std::endl;
		return -1;
	}
	
	WmsTreeNode layerTree;
	const bool buildTreeSuccess = BuildWmsLayerTree(capabilities, layerTree);
	if (!buildTreeSuccess)
	{
		std::cerr << "Failed to build layer tree." << std::endl;
		return -1;
	}
	
	BuildVisibleMapRequestItemsInput input;
	input.mapType = MapTileMode::WMTS;
	input.capabilities = &capabilities;
	input.mapServiceUrlUtf8 = url;
	input.layerNameUtf8 = "ne:world";
	input.tileMatrixSetUtf8 = "EPSG:4326";
	input.formatUtf8 = "image/png";
	input.requestAreaWkt = GB_ToWkt("EPSG:4326");
	input.requestAreaPolygon.SetVertices({ GB_Point2d(228.9925, 153.8763), GB_Point2d(-108.0474, 25.1635), GB_Point2d(131.0075, -153.8763) });
	//input.requestAreaPolygon.SetVertices({ GB_Point2d(-180, 90), GB_Point2d(-180, -90), GB_Point2d(180, -90), GB_Point2d(180, 90) });
	input.renderTarget.zoomLevel = 2;
	const std::vector<MapRequestItem> items = BuildVisibleMapRequestItems(input);



	//const std::string url = "https://192.168.60.148:6443/arcgis/rest/services/nb2000_2/MapServer/WMTS/1.0.0/WMTSCapabilities.xml";
	//ArcGISMapServiceInfo arcGISMapServiceInfo;
	//RequestArcGISServerJson(url, arcGISMapServiceInfo, networkOptions);
	//const std::string originWkt = arcGISMapServiceInfo.m_spatialReference.m_wkt;
	//
	////std::string wktWithArea = "";
	////TryExportWkt2WithCustomTransverseMercatorAreaBbox(originWkt, wktWithArea);
	//
	//double minX = 0, minY = 0, maxX = 0, maxY = 0;
	//GetCartesianExtents(originWkt, minX, minY, maxX, maxY);

	return 0;
}







