#include "../MapWeaverCore/include/GeoCrsManager.h"
#include "../MapWeaverCore/include/GeoBoundingBox.h"
#include "../MapWeaverCore/include/GeoCrsTransform.h"
#include "../GlobalBase/GB_Logger.h"
#include "../GlobalBase/GB_SmbAccessor.h"
#include "../GlobalBase/GB_Interval.h"
#include "../GlobalBase/GB_DateTime.h"
#include <iostream>

int main(int argc, char* argv[])
{
	GB_SetConsoleEncodingToUtf8();

	GB_Interval<GB_Date> timeInterval;
	timeInterval.lower = GB_Date::Today();

	timeInterval.upper = timeInterval.lower.AddDays(1);







	

	return 0;
}







