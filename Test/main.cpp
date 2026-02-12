#include "../MapWeaverCore/include/GeoCrsManager.h"
#include "../MapWeaverCore/include/GeoBoundingBox.h"
#include "../MapWeaverCore/include/GeoCrsTransform.h"
#include "../GlobalBase/GB_Logger.h"
#include <iostream>

int main(int argc, char* argv[])
{
	GB_SetConsoleEncodingToUtf8();

	std::shared_ptr<const GeoCrs> crs4326 = GeoCrsManager::GetFromEpsgCached(4326);
	if (!crs4326)
	{
		GBLOG_ERROR(GB_STR("无法获取到EPSG:4326坐标系。"));
		return 1;
	}

	const GeoBoundingBox bbox4326 = crs4326->GetValidArea();
	if (!bbox4326.IsValid())
	{
		GBLOG_ERROR(GB_STR("包络框无效。"));
		return 1;
	}

	GeoBoundingBox bbox3857;
	if (!GeoCrsTransform::TransformBoundingBox(bbox4326, GeoCrsManager::EpsgCodeToWktUtf8("EPSG:3857"), bbox3857) ||
		!bbox3857.IsValid())
	{
		GBLOG_ERROR(GB_STR("坐标系转换失败。"));
		return 1;
	}

	std::cout << bbox3857.SerializeToString() << std::endl;
	return 0;
}







