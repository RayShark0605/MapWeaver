#include "MapLayer.h"

bool WmsDimensionProperty::operator==(const WmsDimensionProperty& other) const
{
	return nameUtf8 == other.nameUtf8 && unitsUtf8 == other.unitsUtf8 && unitSymbolUtf8 == other.unitSymbolUtf8 && defaultValueUtf8 == other.defaultValueUtf8 && extentUtf8 == other.extentUtf8 && multipleValues == other.multipleValues && nearestValue == other.nearestValue && current == other.current;
}

