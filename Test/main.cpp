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
	if (!DownloadWmsCapabilities(GB_STR("https://gibs.earthdata.nasa.gov/wmts/epsg3857/best/1.0.0/WMTSCapabilities.xml"), capabilitiesXmlUtf8)
		|| capabilitiesXmlUtf8.empty())
	{
		return 1;
	}

	if (!GB_WriteUtf8ToFile(GB_GetExeDirectory() + GB_STR("capabilities.xml"), capabilitiesXmlUtf8, false))
	{
		return 1;
	}
	return 0;
}







