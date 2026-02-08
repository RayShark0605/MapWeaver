#include "..\MapWeaverCore\include\GeoCrs.h"
#include "../MapWeaverCore\include/GeoBoundingBox.h"


int main(int argc, char* argv[])
{
	const GeoCrs crs = GeoCrs::CreateFromEpsgCode(3857);

	const std::string name = crs.GetNameUtf8();
	const bool isGeographic = crs.IsGeographic();
	const bool isProjected = crs.IsProjected();
	const std::string wktUtf8 = crs.ExportToWktUtf8();
	const int epsgCode = crs.TryGetEpsgCode();
	const std::string epsgString = crs.ToEpsgStringUtf8();
	const GeoCrs::UnitsInfo linearUnits = crs.GetLinearUnits();
	const GeoCrs::UnitsInfo angularUnits = crs.GetAngularUnits();
	const GeoBoundingBox bbox = crs.GetValidArea();
	const GeoBoundingBox bbox4326 = crs.GetValidAreaLonLat();



return 0;
}







