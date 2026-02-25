#include "../MapWeaverCore/include/GeoCrsManager.h"
#include "../MapWeaverCore/include/GeoBoundingBox.h"
#include "../MapWeaverCore/include/GeoCrsTransform.h"
#include "../MapWeaverCore/include/MapWeaverCore.h"
#include "GB_Logger.h"
#include "GB_SmbAccessor.h"
#include "GB_Interval.h"
#include "GB_DateTime.h"
#include <iostream>

int main(int argc, char* argv[])
{
	GB_SetConsoleEncodingToUtf8();

	std::string capabilitiesXmlUtf8 = "";
	bool ok = DownloadWmsCapabilities(GB_STR("https://gsi-cyberjapan.github.io/experimental_wmts/gsitiles_wmts_light.xml"), capabilitiesXmlUtf8);


	return 0;
}







