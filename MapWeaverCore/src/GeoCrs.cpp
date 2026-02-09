#include "GeoCrs.h"
#include "GeoBoundingBox.h"
#include "Geometry/GB_Point2d.h"
#include "Geometry/GB_Rectangle.h"
#include "GB_Logger.h"

#include "cpl_conv.h"
#include "ogr_spatialref.h"
#include "ogr_srs_api.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cerrno>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace
{
	struct CplCharDeleter
	{
		void operator()(char* ptr) const noexcept
		{
			if (ptr != nullptr)
			{
				CPLFree(ptr);
			}
		}
	};

	using CplCharPtr = std::unique_ptr<char, CplCharDeleter>;

	struct CoordinateTransformationDeleter
	{
		void operator()(OGRCoordinateTransformation* transform) const noexcept
		{
			if (transform != nullptr)
			{
				OCTDestroyCoordinateTransformation(transform);
			}
		}
	};

	using CoordinateTransformationPtr = std::unique_ptr<OGRCoordinateTransformation, CoordinateTransformationDeleter>;

	static bool EqualsIgnoreCaseAscii(const char* left, const char* right)
	{
		if (left == nullptr || right == nullptr)
		{
			return false;
		}

		while (*left != '\0' && *right != '\0')
		{
			char leftChar = *left;
			char rightChar = *right;

			if (leftChar >= 'A' && leftChar <= 'Z')
			{
				leftChar = static_cast<char>(leftChar - 'A' + 'a');
			}

			if (rightChar >= 'A' && rightChar <= 'Z')
			{
				rightChar = static_cast<char>(rightChar - 'A' + 'a');
			}

			if (leftChar != rightChar)
			{
				return false;
			}

			left++;
			right++;
		}

		return *left == '\0' && *right == '\0';
	}

	static int ParsePositiveInt(const char* text)
	{
		if (text == nullptr || *text == '\0')
		{
			return 0;
		}

		errno = 0;
		char* endPtr = nullptr;
		const long long value = std::strtoll(text, &endPtr, 10);

		if (endPtr == text || endPtr == nullptr || *endPtr != '\0')
		{
			return 0;
		}

		if (errno == ERANGE)
		{
			return 0;
		}

		if (value <= 0 || value > static_cast<long long>(std::numeric_limits<int>::max()))
		{
			return 0;
		}

		return static_cast<int>(value);
	}

	static int ExtractEpsgCodeFromSrs(const OGRSpatialReference& srs)
	{
		// 只检查根节点的 AUTHORITY（pszTargetKey=nullptr）
		// 避免从子节点（例如基础 GEOGCRS）“捡到”EPSG，从而把自定义 PROJCRS 误判为 EPSG:4326。
		const char* authorityName = srs.GetAuthorityName(nullptr);
		const char* authorityCode = srs.GetAuthorityCode(nullptr);
		if (authorityName == nullptr || authorityCode == nullptr)
		{
			return 0;
		}

		if (!EqualsIgnoreCaseAscii(authorityName, "EPSG"))
		{
			return 0;
		}

		return ParsePositiveInt(authorityCode);
	}

	static bool IsFinite(double value)
	{
		return std::isfinite(value);
	}

	static uint64_t Fnv1a64(const void* data, size_t size)
	{
		const uint8_t* bytes = static_cast<const uint8_t*>(data);
		uint64_t hashValue = 14695981039346656037ull;

		for (size_t i = 0; i < size; i++)
		{
			hashValue ^= bytes[i];
			hashValue *= 1099511628211ull;
		}

		return hashValue;
	}

	static std::string ToHex64(uint64_t value)
	{
		static const char* const hexChars = "0123456789abcdef";
		std::string result(16, '0');

		for (int i = 15; i >= 0; i--)
		{
			result[static_cast<size_t>(i)] = hexChars[static_cast<size_t>(value & 0xFULL)];
			value >>= 4;
		}

		return result;
	}

	static bool IsUnknownAreaOfUseValue(double value)
	{
		// GDAL 的 area of use 若不可用，常以 -1000 表示未知。
		// 这里用一个小阈值做容错。
		return value <= -999.5;
	}



	static double ClampDouble(double value, double minValue, double maxValue)
	{
		return std::max(minValue, std::min(maxValue, value));
	}

	static void AppendLonLatGridSamples(
		double west,
		double south,
		double east,
		double north,
		int gridCount,
		std::vector<double>& longitudes,
		std::vector<double>& latitudes)
	{
		const int count = std::max(2, gridCount);

		for (int yIndex = 0; yIndex < count; yIndex++)
		{
			const double yT = (count <= 1) ? 0.0 : static_cast<double>(yIndex) / static_cast<double>(count - 1);
			const double lat = south + (north - south) * yT;

			for (int xIndex = 0; xIndex < count; xIndex++)
			{
				const double xT = (count <= 1) ? 0.0 : static_cast<double>(xIndex) / static_cast<double>(count - 1);
				const double lon = west + (east - west) * xT;

				longitudes.push_back(lon);
				latitudes.push_back(lat);
			}
		}
	}

	static inline GeoBoundingBox MakeInvalidGeoBoundingBox()
	{
		return GeoBoundingBox::Invalid;
	}

	static OGRSpatialReference* CreateOgrSpatialReference()
	{
		const OGRSpatialReferenceH handle = OSRNewSpatialReference(nullptr);
		return OGRSpatialReference::FromHandle(handle);
	}

	static void ApplyAxisOrderStrategy(OGRSpatialReference& srs, bool useTraditionalGisAxisOrder)
	{
		srs.SetAxisMappingStrategy(useTraditionalGisAxisOrder ? OAMS_TRADITIONAL_GIS_ORDER : OAMS_AUTHORITY_COMPLIANT);
	}

	static void EnsureTraditionalGisAxisOrder(OGRSpatialReference& srs)
	{
		// 强制使用传统 GIS 顺序 (X=经度/Easting, Y=纬度/Northing)。
		srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
	}
	static std::string ExportSrsToWktUtf8(const OGRSpatialReference& srs, const char* const* options)
	{
		char* wktRaw = nullptr;
		const OGRErr err = srs.exportToWkt(&wktRaw, options);
		CplCharPtr wkt(wktRaw);

		if (err != OGRERR_NONE || wkt == nullptr)
		{
			return "";
		}

		return std::string(wkt.get());
	}

}

void GeoCrsOgrSrsDeleter::operator()(OGRSpatialReference* srs) const noexcept
{
	if (srs == nullptr)
	{
		return;
	}

	// OGRSpatialReference 是引用计数对象。
	srs->Release();
}

GeoCrs::GeoCrs(): spatialReference(nullptr)
{
	spatialReference.reset(CreateOgrSpatialReference());
	if (spatialReference)
	{
		ApplyAxisOrderStrategy(*spatialReference, useTraditionalGisAxisOrder);
	}

	InvalidateCaches();
}

GeoCrs::GeoCrs(const GeoCrs& other) : spatialReference(nullptr), useTraditionalGisAxisOrder(other.useTraditionalGisAxisOrder)
{
	if (other.spatialReference)
	{
		spatialReference.reset(other.spatialReference->Clone());
	}
	else
	{
		spatialReference.reset(CreateOgrSpatialReference());
	}

	if (spatialReference)
	{
		ApplyAxisOrderStrategy(*spatialReference, useTraditionalGisAxisOrder);
	}

	InvalidateCaches();
}

GeoCrs::GeoCrs(GeoCrs&& other) noexcept : spatialReference(std::move(other.spatialReference)), useTraditionalGisAxisOrder(other.useTraditionalGisAxisOrder)
{
	if (spatialReference)
	{
		ApplyAxisOrderStrategy(*spatialReference, useTraditionalGisAxisOrder);
	}

	InvalidateCaches();

	// 让 moved-from 对象保持“可用的空 CRS”状态，避免后续误用导致空指针问题。
	other.useTraditionalGisAxisOrder = true;
	if (other.spatialReference == nullptr)
	{
		other.spatialReference.reset(CreateOgrSpatialReference());
		if (other.spatialReference)
		{
			ApplyAxisOrderStrategy(*other.spatialReference, other.useTraditionalGisAxisOrder);
		}
	}
	other.InvalidateCaches();
}

void GeoCrs::InvalidateCaches() const
{
	hasCachedDefaultEpsgCode = false;
	cachedDefaultEpsgCode = 0;

	cachedUidKind = -2;
	cachedUidWktHash = 0;
}

GeoCrs::~GeoCrs() = default;

GeoCrs& GeoCrs::operator=(const GeoCrs& other)
{
	if (this == &other)
	{
		return *this;
	}

	useTraditionalGisAxisOrder = other.useTraditionalGisAxisOrder;

	std::unique_ptr<OGRSpatialReference, GeoCrsOgrSrsDeleter> newSrs(nullptr);
	if (other.spatialReference)
	{
		newSrs.reset(other.spatialReference->Clone());
	}
	else
	{
		newSrs.reset(CreateOgrSpatialReference());
	}

	if (newSrs)
	{
		ApplyAxisOrderStrategy(*newSrs, useTraditionalGisAxisOrder);
	}

	spatialReference.swap(newSrs);

	InvalidateCaches();
	return *this;
}

GeoCrs& GeoCrs::operator=(GeoCrs&& other) noexcept
{
	if (this == &other)
	{
		return *this;
	}

	spatialReference = std::move(other.spatialReference);
	useTraditionalGisAxisOrder = other.useTraditionalGisAxisOrder;

	if (spatialReference)
	{
		ApplyAxisOrderStrategy(*spatialReference, useTraditionalGisAxisOrder);
	}

	InvalidateCaches();

	// 让 moved-from 对象保持“可用的空 CRS”状态，避免后续误用导致空指针问题。
	other.useTraditionalGisAxisOrder = true;
	if (other.spatialReference == nullptr)
	{
		other.spatialReference.reset(CreateOgrSpatialReference());
		if (other.spatialReference)
		{
			ApplyAxisOrderStrategy(*other.spatialReference, other.useTraditionalGisAxisOrder);
		}
	}
	other.InvalidateCaches();

	return *this;
}

GeoCrs GeoCrs::CreateFromWkt(const std::string& wktUtf8)
{
	GeoCrs crs;
	crs.SetFromWkt(wktUtf8);
	return crs;
}

GeoCrs GeoCrs::CreateFromEpsgCode(int epsgCode)
{
	GeoCrs crs;
	crs.SetFromEpsgCode(epsgCode);
	return crs;
}

GeoCrs GeoCrs::CreateFromUserInput(const std::string& definitionUtf8, bool allowNetworkAccess, bool allowFileAccess)
{
	GeoCrs crs;
	crs.SetFromUserInput(definitionUtf8, allowNetworkAccess, allowFileAccess);
	return crs;
}

bool GeoCrs::Reset()
{
	OGRSpatialReference* srs = Get();
	if (srs == nullptr)
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::Reset】空的 srs。"));
		InvalidateCaches();
		return false;
	}

	srs->Clear();
	ApplyAxisOrderStrategy(*srs, useTraditionalGisAxisOrder);
	InvalidateCaches();
	return true;
}

bool GeoCrs::SetFromWkt(const std::string& wktUtf8)
{
	Reset();

	if (wktUtf8.empty())
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::SetFromWkt】空的 wktUtf8。"));
		return false;
	}

	OGRSpatialReference* srs = Get();
	if (srs == nullptr)
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::SetFromWkt】空的 srs。"));
		return false;
	}

	const OGRErr err = srs->importFromWkt(wktUtf8.c_str());
	if (err != OGRERR_NONE)
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::SetFromWkt】importFromWkt 出错: ") + std::to_string(static_cast<int>(err)));
		Reset();
		return false;
	}

	ApplyAxisOrderStrategy(*srs, useTraditionalGisAxisOrder);
	InvalidateCaches();
	return IsValid();
}

bool GeoCrs::SetFromEpsgCode(int epsgCode)
{
	Reset();

	if (epsgCode <= 0)
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::SetFromEpsgCode】epsgCode非正: ") + std::to_string(epsgCode));
		return false;
	}

	OGRSpatialReference* srs = Get();
	if (srs == nullptr)
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::SetFromEpsgCode】空的 srs。"));
		return false;
	}

	const OGRErr err = srs->importFromEPSG(epsgCode);
	if (err != OGRERR_NONE)
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::SetFromEpsgCode】importFromWkt 出错: ") + std::to_string(static_cast<int>(err)));
		Reset();
		return false;
	}

	ApplyAxisOrderStrategy(*srs, useTraditionalGisAxisOrder);
	InvalidateCaches();
	return IsValid();
}

bool GeoCrs::SetFromUserInput(const std::string& definitionUtf8, bool allowNetworkAccess, bool allowFileAccess)
{
	Reset();

	if (definitionUtf8.empty())
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::SetFromUserInput】definitionUtf8 为空。"));
		return false;
	}

	OGRSpatialReference* srs = Get();
	if (srs == nullptr)
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::SetFromUserInput】srs 为空。"));
		return false;
	}

	const char* const options[] =
	{
		allowNetworkAccess ? "ALLOW_NETWORK_ACCESS=YES" : "ALLOW_NETWORK_ACCESS=NO",
		allowFileAccess ? "ALLOW_FILE_ACCESS=YES" : "ALLOW_FILE_ACCESS=NO",
		nullptr
	};

	const OGRErr err = srs->SetFromUserInput(definitionUtf8.c_str(), options);
	if (err != OGRERR_NONE)
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::SetFromUserInput】SetFromUserInput 出错: ") + std::to_string(static_cast<int>(err)));
		Reset();
		return false;
	}

	ApplyAxisOrderStrategy(*srs, useTraditionalGisAxisOrder);
	InvalidateCaches();
	return IsValid();
}

bool GeoCrs::IsEmpty() const
{
	if (!spatialReference)
	{
		return true;
	}

	return spatialReference->IsEmpty();
}

bool GeoCrs::IsValid() const
{
	if (IsEmpty())
	{
		return false;
	}

	return spatialReference->Validate() == OGRERR_NONE;
}

std::string GeoCrs::GetNameUtf8() const
{
	if (IsEmpty())
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::GetNameUtf8】变量为空。"));
		return "";
	}

	const char* name = spatialReference->GetName();
	return name ? std::string(name) : std::string();
}

std::string GeoCrs::GetUidUtf8() const
{
	if (IsEmpty())
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::GetNameUtf8】变量为空。"));
		return "";
	}

	// 快速返回缓存
	if (cachedUidKind != -2)
	{
		if (cachedUidKind > 0)
		{
			return std::string("EPSG:") + std::to_string(cachedUidKind);
		}

		if (cachedUidKind == 0)
		{
			return std::string("WKT2_2018:FNV1A64:") + ToHex64(cachedUidWktHash);
		}

		GBLOG_WARNING(GB_STR("【GeoCrs::GetNameUtf8】未知的 cachedUidKind。"));
		return "";
	}

	const int epsgCode = TryGetEpsgCode(true, false, 90);
	if (epsgCode > 0)
	{
		cachedUidKind = epsgCode;
		return std::string("EPSG:") + std::to_string(epsgCode);
	}

	const std::string wkt = ExportToWktUtf8(WktFormat::Wkt2_2018, false);
	if (wkt.empty())
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::GetNameUtf8】ExportToWktUtf8 结果为空。"));
		cachedUidKind = -1;
		return "";
	}

	cachedUidWktHash = Fnv1a64(wkt.data(), wkt.size());
	cachedUidKind = 0;

	return std::string("WKT2_2018:FNV1A64:") + ToHex64(cachedUidWktHash);
}

bool GeoCrs::operator==(const GeoCrs& other) const
{
	if (IsEmpty() && other.IsEmpty())
	{
		return true;
	}

	if (IsEmpty() != other.IsEmpty())
	{
		return false;
	}


	// 忽略 axis mapping 差异：本类允许通过 SetTraditionalGisAxisOrder() 在两种策略间切换。
	// GDAL 的 IsSame(...) 默认会考虑 data axis <-> CRS axis 的 mapping；这里显式忽略它。
	const char* const options[] =
	{
		"IGNORE_DATA_AXIS_TO_SRS_AXIS_MAPPING=YES",
		"CRITERION=EQUIVALENT_EXCEPT_AXIS_ORDER_GEOGCRS",
		nullptr
	};

	return spatialReference->IsSame(other.spatialReference.get(), options) != 0;
}

bool GeoCrs::operator!=(const GeoCrs& other) const
{
	return !(*this == other);
}

bool GeoCrs::IsGeographic() const
{
	if (IsEmpty())
	{
		return false;
	}

	return spatialReference->IsGeographic() != 0;
}

bool GeoCrs::IsProjected() const
{
	if (IsEmpty())
	{
		return false;
	}

	return spatialReference->IsProjected() != 0;
}

bool GeoCrs::IsLocal() const
{
	if (IsEmpty())
	{
		return false;
	}

	return spatialReference->IsLocal() != 0;
}

void GeoCrs::SetTraditionalGisAxisOrder(bool enable)
{
	if (useTraditionalGisAxisOrder == enable)
	{
		return;
	}

	useTraditionalGisAxisOrder = enable;

	if (spatialReference)
	{
		ApplyAxisOrderStrategy(*spatialReference, useTraditionalGisAxisOrder);
	}

	InvalidateCaches();
}

std::string GeoCrs::ExportToWktUtf8(WktFormat format, bool multiline) const
{
	if (IsEmpty())
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::ExportToWktUtf8】变量为空。"));
		return "";
	}

	const char* formatOption = nullptr;
	switch (format)
	{
	case WktFormat::Default:
		formatOption = nullptr;
		break;
	case WktFormat::Wkt1Gdal:
		formatOption = "FORMAT=WKT1_GDAL";
		break;
	case WktFormat::Wkt1Esri:
		formatOption = "FORMAT=WKT1_ESRI";
		break;
	case WktFormat::Wkt2_2015:
		formatOption = "FORMAT=WKT2_2015";
		break;
	case WktFormat::Wkt2_2018:
		formatOption = "FORMAT=WKT2_2018";
		break;
	case WktFormat::Wkt2:
		formatOption = "FORMAT=WKT2";
		break;
	default:
		formatOption = "FORMAT=WKT2_2018";
		break;
	}

	const char* const multilineOption = multiline ? "MULTILINE=YES" : "MULTILINE=NO";

	if (formatOption == nullptr)
	{
		const char* const options[] = { multilineOption, nullptr };
		return ExportSrsToWktUtf8(*spatialReference, options);
	}

	const char* const options[] = { formatOption, multilineOption, nullptr };
	return ExportSrsToWktUtf8(*spatialReference, options);
}

std::string GeoCrs::ExportToPrettyWktUtf8(bool simplify) const
{
	if (IsEmpty())
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::ExportToPrettyWktUtf8】变量为空。"));
		return "";
	}

	char* wktRaw = nullptr;
	const OGRErr err = spatialReference->exportToPrettyWkt(&wktRaw, simplify ? TRUE : FALSE);
	CplCharPtr wkt(wktRaw);

	if (err != OGRERR_NONE || wkt == nullptr)
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::ExportToPrettyWktUtf8】exportToPrettyWkt 失败: err=") +
			std::to_string(static_cast<int>(err)) +
			GB_STR(", wkt=") +
			(wktRaw ? std::string(wktRaw) : ""));
		return "";
	}

	return std::string(wkt.get());
}

std::string GeoCrs::ExportToProj4Utf8() const
{
	if (IsEmpty())
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::ExportToProj4Utf8】变量为空。"));
		return "";
	}

	char* proj4Raw = nullptr;
	const OGRErr err = spatialReference->exportToProj4(&proj4Raw);
	CplCharPtr proj4(proj4Raw);

	if (err != OGRERR_NONE || proj4 == nullptr)
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::ExportToProj4Utf8】exportToProj4 失败: err=") +
			std::to_string(static_cast<int>(err)) +
			GB_STR(", proj4=") +
			(proj4Raw ? std::string(proj4Raw) : ""));
		return "";
	}

	return std::string(proj4.get());
}

std::string GeoCrs::ExportToProjJsonUtf8() const
{
	if (IsEmpty())
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::ExportToProjJsonUtf8】变量为空。"));
		return "";
	}

	char* projJsonRaw = nullptr;
	const OGRErr err = spatialReference->exportToPROJJSON(&projJsonRaw, nullptr);
	CplCharPtr projJson(projJsonRaw);

	if (err != OGRERR_NONE || projJson == nullptr)
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::ExportToProjJsonUtf8】exportToPROJJSON 失败: err=") +
			std::to_string(static_cast<int>(err)) +
			GB_STR(", projJson=") +
			(projJsonRaw ? std::string(projJsonRaw) : ""));
		return "";
	}

	return std::string(projJson.get());
}

int GeoCrs::TryGetEpsgCode(bool tryAutoIdentify, bool tryFindBestMatch, int minMatchConfidence) const
{
	if (IsEmpty())
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::TryGetEpsgCode】变量为空。"));
		return 0;
	}

	const bool isDefaultQuery = (tryAutoIdentify && !tryFindBestMatch && minMatchConfidence == 90);
	if (isDefaultQuery && hasCachedDefaultEpsgCode)
	{
		return cachedDefaultEpsgCode;
	}

	int epsgCode = ExtractEpsgCodeFromSrs(*spatialReference);
	if (epsgCode > 0)
	{
		if (isDefaultQuery)
		{
			cachedDefaultEpsgCode = epsgCode;
			hasCachedDefaultEpsgCode = true;
		}
		return epsgCode;
	}

	if (tryAutoIdentify)
	{
		std::unique_ptr<OGRSpatialReference, GeoCrsOgrSrsDeleter> cloned(spatialReference->Clone());
		if (cloned)
		{
			cloned->AutoIdentifyEPSG();
			epsgCode = ExtractEpsgCodeFromSrs(*cloned);
			if (epsgCode > 0)
			{
				if (isDefaultQuery)
				{
					cachedDefaultEpsgCode = epsgCode;
					hasCachedDefaultEpsgCode = true;
				}
				return epsgCode;
			}
		}
	}

	if (tryFindBestMatch)
	{
		OGRSpatialReference* bestMatch = spatialReference->FindBestMatch(minMatchConfidence, "EPSG", nullptr);
		std::unique_ptr<OGRSpatialReference, GeoCrsOgrSrsDeleter> bestMatchHolder(bestMatch);
		if (bestMatchHolder)
		{
			epsgCode = ExtractEpsgCodeFromSrs(*bestMatchHolder);
			if (epsgCode > 0)
			{
				return epsgCode;
			}
		}
	}

	if (isDefaultQuery)
	{
		cachedDefaultEpsgCode = 0;
		hasCachedDefaultEpsgCode = true;
	}

	GBLOG_WARNING(GB_STR("【GeoCrs::TryGetEpsgCode】无法找到 EPSG Code。"));
	return 0;
}

std::string GeoCrs::ToEpsgStringUtf8() const
{
	const int epsgCode = TryGetEpsgCode(true);
	if (epsgCode <= 0)
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::ToEpsgStringUtf8】epsgCode 无效。"));
		return "";
	}

	return std::string("EPSG:") + std::to_string(epsgCode);
}

std::string GeoCrs::ToOgcUrnStringUtf8() const
{
	if (IsEmpty())
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::ToOgcUrnStringUtf8】对象为空。"));
		return "";
	}

	char* urnRaw = spatialReference->GetOGCURN();
	CplCharPtr urn(urnRaw);
	if (urn == nullptr)
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::ToOgcUrnStringUtf8】空的 urn。"));
		return "";
	}

	return std::string(urn.get());
}

GeoCrs::UnitsInfo GeoCrs::GetLinearUnits() const
{
	UnitsInfo info;
	if (IsEmpty())
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::GetLinearUnits】对象为空。"));
		return info;
	}

	const char* unitName = nullptr;
	const double toMeters = spatialReference->GetLinearUnits(&unitName);
	info.toSI = toMeters;
	info.nameUtf8 = unitName ? std::string(unitName) : std::string();
	return info;
}

GeoCrs::UnitsInfo GeoCrs::GetAngularUnits() const
{
	UnitsInfo info;
	if (IsEmpty())
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::GetAngularUnits】对象为空。"));
		return info;
	}

	const char* unitName = nullptr;
	const double toRadians = spatialReference->GetAngularUnits(&unitName);
	info.toSI = toRadians;
	info.nameUtf8 = unitName ? std::string(unitName) : std::string();
	return info;
}

std::vector<GeoCrs::LonLatAreaSegment> GeoCrs::GetValidAreaLonLatSegments() const
{
	std::vector<LonLatAreaSegment> segments;

	if (IsEmpty())
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::GetValidAreaLonLatSegments】对象为空。"));
		return segments;
	}

	double west = 0;
	double south = 0;
	double east = 0;
	double north = 0;
	const char* areaName = nullptr;

	const bool ok = spatialReference->GetAreaOfUse(&west, &south, &east, &north, &areaName);
	if (!ok)
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::GetValidAreaLonLatSegments】GetAreaOfUse 失败。"));
		return segments;
	}

	if (IsUnknownAreaOfUseValue(west) || IsUnknownAreaOfUseValue(south) || IsUnknownAreaOfUseValue(east) || IsUnknownAreaOfUseValue(north))
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::GetValidAreaLonLatSegments】未知区域。"));
		return segments;
	}

	// 容错：约束在经纬度常用范围内
	west = ClampDouble(west, -180.0, 180.0);
	east = ClampDouble(east, -180.0, 180.0);
	south = ClampDouble(south, -90.0, 90.0);
	north = ClampDouble(north, -90.0, 90.0);

	if (south > north)
	{
		std::swap(south, north);
	}

	// 跨越日期变更线时 west 可能大于 east。此时返回 2 段，以便上层按两段采样/计算。
	if (west <= east)
	{
		LonLatAreaSegment segment;
		segment.west = west;
		segment.south = south;
		segment.east = east;
		segment.north = north;
		segments.push_back(segment);
	}
	else
	{
		LonLatAreaSegment segment1;
		segment1.west = west;
		segment1.south = south;
		segment1.east = 180.0;
		segment1.north = north;

		LonLatAreaSegment segment2;
		segment2.west = -180.0;
		segment2.south = south;
		segment2.east = east;
		segment2.north = north;

		if (segment1.west <= segment1.east)
		{
			segments.push_back(segment1);
		}
		if (segment2.west <= segment2.east)
		{
			segments.push_back(segment2);
		}
	}

	return segments;
}

GeoBoundingBox GeoCrs::GetValidArea() const
{
	if (IsEmpty())
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::GetValidArea】对象为空。"));
		return MakeInvalidGeoBoundingBox();
	}

	// ---- Geographic CRS：直接返回经纬度范围（注意：跨日期线时 GetValidAreaLonLat() 会返回保守的全球经度范围）----
	if (IsGeographic())
	{
		GeoBoundingBox lonLatArea = GetValidAreaLonLat();
		if (!lonLatArea.IsValid())
		{
			GeoBoundingBox fallback;
			fallback.wktUtf8 = ExportToWktUtf8(WktFormat::Wkt2_2018, false);
			fallback.rect = useTraditionalGisAxisOrder
				? GB_Rectangle(-180.0, -90.0, 180.0, 90.0)   // X=经度, Y=纬度
				: GB_Rectangle(-90.0, -180.0, 90.0, 180.0);  // X=纬度, Y=经度（权威机构顺序）
			GBLOG_WARNING(GB_STR("【GeoCrs::GetValidArea】GetValidAreaLonLat无效，返回全球范围。"));
			return fallback;
		}

		GeoBoundingBox result = lonLatArea;
		result.wktUtf8 = ExportToWktUtf8(WktFormat::Wkt2_2018, false);

		if (!useTraditionalGisAxisOrder)
		{
			// useTraditionalGisAxisOrder=false 时，数据轴顺序为“纬度/经度”，因此将 (lon,lat) 转为 (lat,lon)。
			result.rect = GB_Rectangle(lonLatArea.rect.minY, lonLatArea.rect.minX, lonLatArea.rect.maxY, lonLatArea.rect.maxX);
		}

		return result;
	}

	// ---- Projected/Local CRS：以经纬度 area of use 采样，再投影到目标 CRS 以估算其有效范围 ----
	const std::vector<LonLatAreaSegment> lonLatSegments = GetValidAreaLonLatSegments();
	if (lonLatSegments.empty())
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::GetValidArea】lonLatSegments为空。"));
		return MakeInvalidGeoBoundingBox();
	}

	OGRSpatialReference sourceSrs;
	if (sourceSrs.importFromEPSG(4326) != OGRERR_NONE)
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::GetValidArea】WGS 84 坐标系导入失败。"));
		return MakeInvalidGeoBoundingBox();
	}
	EnsureTraditionalGisAxisOrder(sourceSrs);

	std::unique_ptr<OGRSpatialReference, GeoCrsOgrSrsDeleter> targetSrs(spatialReference->Clone());
	if (!targetSrs)
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::GetValidArea】坐标系克隆失败。"));
		return MakeInvalidGeoBoundingBox();
	}

	// 计算 BoundingBox 时，为避免“权威轴序”导致 X/Y 含义混淆，这里统一用传统 GIS 顺序进行转换。
	// 然后再根据 useTraditionalGisAxisOrder 的配置，决定是否需要把结果交换为 northing/easting 顺序。
	EnsureTraditionalGisAxisOrder(*targetSrs);

	CoordinateTransformationPtr transform(OGRCreateCoordinateTransformation(&sourceSrs, targetSrs.get()));
	if (transform == nullptr)
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::GetValidArea】OGRCreateCoordinateTransformation 失败。"));
		return MakeInvalidGeoBoundingBox();
	}

	// 采样密度：21x21。相较 3x3 更能覆盖非线性投影边界的弯曲情况。
	const int gridCount = 21;

	std::vector<double> longitudes;
	std::vector<double> latitudes;
	longitudes.reserve(static_cast<size_t>(gridCount) * static_cast<size_t>(gridCount) * lonLatSegments.size());
	latitudes.reserve(static_cast<size_t>(gridCount) * static_cast<size_t>(gridCount) * lonLatSegments.size());

	for (const LonLatAreaSegment& seg : lonLatSegments)
	{
		if (!IsFinite(seg.west) || !IsFinite(seg.south) || !IsFinite(seg.east) || !IsFinite(seg.north))
		{
			continue;
		}

		if (seg.south > seg.north || seg.west > seg.east)
		{
			continue;
		}

		AppendLonLatGridSamples(seg.west, seg.south, seg.east, seg.north, gridCount, longitudes, latitudes);
	}

	const size_t numPoints = longitudes.size();
	if (numPoints == 0 || numPoints > static_cast<size_t>(std::numeric_limits<int>::max()))
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::GetValidArea】longitudes 为空。"));
		return MakeInvalidGeoBoundingBox();
	}

	const int numPointsInt = static_cast<int>(numPoints);
	std::vector<int> successFlags(numPoints, FALSE);

	// 注意：Transform 返回值在历史版本 GDAL 中可能不足以判断“部分点失败”，应以 pabSuccess 为准。
	transform->Transform(numPointsInt, longitudes.data(), latitudes.data(), nullptr, successFlags.data());

	double minX = std::numeric_limits<double>::infinity();
	double minY = std::numeric_limits<double>::infinity();
	double maxX = -std::numeric_limits<double>::infinity();
	double maxY = -std::numeric_limits<double>::infinity();
	bool hasAnyPoint = false;

	for (size_t i = 0; i < numPoints; i++)
	{
		if (successFlags[i] == FALSE)
		{
			continue;
		}

		const double x = longitudes[i];
		const double y = latitudes[i];

		if (!IsFinite(x) || !IsFinite(y))
		{
			continue;
		}

		hasAnyPoint = true;
		minX = std::min(minX, x);
		minY = std::min(minY, y);
		maxX = std::max(maxX, x);
		maxY = std::max(maxY, y);
	}

	if (!hasAnyPoint)
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::GetValidArea】hasAnyPoint == false。"));
		return MakeInvalidGeoBoundingBox();
	}

	GeoBoundingBox result;
	result.wktUtf8 = ExportToWktUtf8(WktFormat::Wkt2_2018, false);
	result.rect = GB_Rectangle(minX, minY, maxX, maxY);

	// 若采用权威轴序，且该投影 CRS 定义为 northing/easting，则交换 X/Y。
	if (!useTraditionalGisAxisOrder && spatialReference->EPSGTreatsAsNorthingEasting() != 0)
	{
		result.rect = GB_Rectangle(result.rect.minY, result.rect.minX, result.rect.maxY, result.rect.maxX);
	}

	return result;
}

GeoBoundingBox GeoCrs::GetValidAreaLonLat() const
{
	if (IsEmpty())
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::GetValidAreaLonLat】对象为空。"));
		return MakeInvalidGeoBoundingBox();
	}

	const std::vector<LonLatAreaSegment> segments = GetValidAreaLonLatSegments();
	if (segments.empty())
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::GetValidAreaLonLat】segments为空。"));
		return MakeInvalidGeoBoundingBox();
	}

	double west = segments[0].west;
	double south = segments[0].south;
	double east = segments[0].east;
	double north = segments[0].north;

	if (segments.size() > 1)
	{
		// GeoBoundingBox 只能表达一个矩形。跨日期变更线时，取保守的全球经度范围。
		west = -180.0;
		east = 180.0;

		for (const LonLatAreaSegment& seg : segments)
		{
			south = std::min(south, seg.south);
			north = std::max(north, seg.north);
		}
	}

	OGRSpatialReference epsg4326;
	if (epsg4326.importFromEPSG(4326) != OGRERR_NONE)
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::GetValidAreaLonLat】WGS 84 坐标系导入失败。"));
		return MakeInvalidGeoBoundingBox();
	}
	EnsureTraditionalGisAxisOrder(epsg4326);

	const char* const options[] = { "FORMAT=WKT2_2018", "MULTILINE=NO", nullptr };

	GeoBoundingBox result;
	result.wktUtf8 = ExportSrsToWktUtf8(epsg4326, options);
	result.rect = GB_Rectangle(west, south, east, north);
	return result;
}

const OGRSpatialReference* GeoCrs::GetConst() const
{
	return spatialReference.get();
}

const OGRSpatialReference& GeoCrs::GetConstRef() const
{
	// 约定上 spatialReference 在构造/Reset 后都应存在；此处仍做兜底，避免空指针解引用。
	if (spatialReference == nullptr)
	{
		static const OGRSpatialReference emptySrs;
		return emptySrs;
	}

	return *spatialReference;
}

OGRSpatialReference& GeoCrs::GetRef()
{
	OGRSpatialReference* srs = Get();
	// Get() 保证返回非空（除非底层创建失败），这里按“不抛异常”风格做最后兜底。
	if (srs == nullptr)
	{
		static OGRSpatialReference emptySrs;
		return emptySrs;
	}
	return *srs;
}

OGRSpatialReference* GeoCrs::Get()
{
	const bool needCreate = (spatialReference == nullptr);

	if (needCreate)
	{
		spatialReference.reset(CreateOgrSpatialReference());
	}

	if (needCreate && spatialReference)
	{
		ApplyAxisOrderStrategy(*spatialReference, useTraditionalGisAxisOrder);
	}

	// 由于返回的是可写指针，调用方可能修改内部 SRS。
	// 为避免缓存被“外部修改”污染，这里统一将缓存置为失效。
	InvalidateCaches();

	return spatialReference.get();
}
