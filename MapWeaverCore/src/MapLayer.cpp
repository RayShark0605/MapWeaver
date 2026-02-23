#include "MapLayer.h"
#include <unordered_set>

bool WmsDimensionProperty::operator==(const WmsDimensionProperty& other) const
{
	return nameUtf8 == other.nameUtf8 && unitsUtf8 == other.unitsUtf8 && unitSymbolUtf8 == other.unitSymbolUtf8 && defaultValueUtf8 == other.defaultValueUtf8 && extentUtf8 == other.extentUtf8 && multipleValues == other.multipleValues && nearestValue == other.nearestValue && current == other.current;
}

bool WmsLayerProperty::IsEqual(const WmsLayerProperty& other) const
{
	if (nameUtf8 != other.nameUtf8)
	{
		return false;
	}

	if (titleUtf8 != other.titleUtf8)
	{
		return false;
	}

	if (abstractUtf8 != other.abstractUtf8)
	{
		return false;
	}

	if (dimensions != other.dimensions)
	{
		return false;
	}

	return true;
}

bool WmsLayerProperty::HasDimension(const std::string& dimensionNameUtf8) const
{
	if (dimensions.empty())
	{
		return false;
	}

	for (const WmsDimensionProperty& dimension : dimensions)
	{
		if (dimension.nameUtf8 == dimensionNameUtf8)
		{
			return true;
		}
	}
	return false;
}

std::string WmsLayerProperty::PreferredAvailableCrs() const
{
	static const std::unordered_set<std::string> skipList{ "EPSG:900913" };
	for (const std::string& crs : crsUtf8)
	{
		if (skipList.find(crs) != skipList.end() || crs.empty())
		{
			continue;
		}

		return crs;
	}

	return (crsUtf8.empty() ? "" : crsUtf8[0]);
}

WmstDates::WmstDates()
{
}

WmstDates::WmstDates(const std::vector<GB_DateTime>& dateTimes) : dateTimes(dateTimes)
{
}

bool WmstDates::operator==(const WmstDates& other) const
{
	return dateTimes == other.dateTimes;
}

WmstExtentPair::WmstExtentPair()
{
}

WmstExtentPair::WmstExtentPair(const WmstDates& dates, const GB_TimeDuration& resolution) : dates(dates), resolution(resolution)
{
}

bool WmstExtentPair::operator==(const WmstExtentPair& other) const
{
	return dates == other.dates && resolution == other.resolution;
}

WmtsTheme::WmtsTheme()
{
}

WmtsTheme::~WmtsTheme()
{
	if (subTheme)
	{
		delete subTheme;
		subTheme = nullptr;
	}
}

GB_Rectangle WmtsTileMatrix::TileRect(int tileCol, int tileRow) const
{
	const double tileWidthLength = tileWidth * tres;
	const double tileHeightLength = tileHeight * tres;

	const double minX = topLeft.x + tileCol * tileWidthLength;
	const double minY = topLeft.y - (tileRow + 1) * tileHeightLength;
	return GB_Rectangle(minX, minY, minX + tileWidthLength, minY + tileHeightLength);
}

bool WmtsTileMatrix::Intersects(const GB_Rectangle& rect, const WmtsTileMatrixLimits* tileMatrixLimits, GB_IntInterval& colIndexInterval, GB_IntInterval& rowIndexInterval) const
{
	const double tileWidthLength = tileWidth * tres;
	const double tileHeightLength = tileHeight * tres;
	if (tileWidthLength <= 0 || tileHeightLength <= 0)
	{
		colIndexInterval.Reset();
		rowIndexInterval.Reset();
		return false;
	}

	int minTileCol = 0;
	int maxTileCol = matrixWidth - 1;
	int minTileRow = 0;
	int maxTileRow = matrixHeight - 1;
	if (tileMatrixLimits && tileMatrixLimits->colIndexInterval.IsValid() && tileMatrixLimits->rowIndexInterval.IsValid())
	{
		minTileCol = tileMatrixLimits->colIndexInterval.lower;
		maxTileCol = tileMatrixLimits->colIndexInterval.upper;
		minTileRow = tileMatrixLimits->rowIndexInterval.lower;
		maxTileRow = tileMatrixLimits->rowIndexInterval.upper;
	}

	colIndexInterval.lower = GB_Clamp(static_cast<int>(std::floor((rect.minX - topLeft.x) / tileWidthLength)), minTileCol, maxTileCol);
	colIndexInterval.upper = GB_Clamp(static_cast<int>(std::floor((rect.maxX - topLeft.x) / tileWidthLength)), minTileCol, maxTileCol);
	rowIndexInterval.lower = GB_Clamp(static_cast<int>(std::floor((topLeft.y - rect.maxY) / tileHeightLength)), minTileRow, maxTileRow);
	rowIndexInterval.upper = GB_Clamp(static_cast<int>(std::floor((topLeft.y - rect.minY) / tileHeightLength)), minTileRow, maxTileRow);
	return true;
}

const WmtsTileMatrix* WmtsTileMatrixSet::FindNearestResolution(double targetTileResolution) const
{
	auto it = tileMatrices.begin();
	auto pre = it;
	while (it != tileMatrices.end() && it->first < targetTileResolution)
	{
		pre = it;
		it++;
	}

	if (it == tileMatrices.end() || (it != tileMatrices.begin() && targetTileResolution - pre->first < it->first - targetTileResolution))
	{
		it = pre;
	}

	return &(it->second);
}

const WmtsTileMatrix* WmtsTileMatrixSet::FindOtherResolution(double targetTileResolution, int offset) const
{
	auto it = tileMatrices.find(targetTileResolution);
	if (it == tileMatrices.end())
	{
		return nullptr;
	}

	while (true)
	{
		if (offset > 0)
		{
			it++;
			offset--;
		}
		else if (offset < 0)
		{
			if (it == tileMatrices.begin())
			{
				return nullptr;
			}
			it--;
			offset++;
		}
		else
		{
			break;
		}

		if (it == tileMatrices.end())
		{
			return nullptr;
		}
	}

	return &(it->second);
}
