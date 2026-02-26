#include "../MapWeaverCore/include/MapWeaverCore.h"
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

	std::string capabilitiesXmlUtf8 = "";
	if (!DownloadWmsCapabilities(GB_STR("https://data.geopf.fr/wms-r/wms?SERVICE=WMS&VERSION=1.3.0&REQUEST=GetCapabilities"), capabilitiesXmlUtf8)
		|| capabilitiesXmlUtf8.empty())
	{
		return 1;
	}

	WmsCapabilitiesProperty capabilities;
	ParseWmsCapabilities(capabilitiesXmlUtf8, capabilities);



	return 0;
}







