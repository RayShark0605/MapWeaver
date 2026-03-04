#ifndef MAP_WEAVER_CORE_MAP_LAYER_H
#define MAP_WEAVER_CORE_MAP_LAYER_H

#include <string>
#include <vector>
#include <unordered_map>
#include <map>
#include "GeoBoundingBox.h"
#include "GB_DateTime.h"
#include "GB_Interval.h"

struct WmsOnlineResourceAttribute
{
	std::string xlinkHrefUtf8 = "";
};

struct WmsGetProperty
{
	WmsOnlineResourceAttribute onlineResource;
};

struct WmsPostProperty
{
	WmsOnlineResourceAttribute onlineResource;
};

struct WmsHttpProperty
{
	WmsGetProperty get;
	WmsPostProperty post;
};

struct WmsDcpTypeProperty
{
	WmsHttpProperty http;
};

struct WmsOperationType
{
	std::vector<std::string> formatsUtf8;
	std::vector<WmsDcpTypeProperty> dcpTypes;
	std::vector<std::string> allowedEncodingsUtf8;
};

struct WmsRequestProperty
{
	WmsOperationType getMap;
	WmsOperationType getFeatureInfo;
	WmsOperationType getTile;
	WmsOperationType getLegendGraphic;
};

struct WmsExceptionProperty
{
	std::vector<std::string> formatsUtf8;
};

struct WmsContactPersonPrimaryProperty
{
	std::string contactPersonUtf8 = "";
	std::string contactOrganizationUtf8 = "";
};

struct WmsContactAddressProperty
{
	std::string addressTypeUtf8 = "";
	std::string addressUtf8 = "";
	std::string cityUtf8 = "";
	std::string stateOrProvinceUtf8 = "";
	std::string postCodeUtf8 = "";
	std::string countryUtf8 = "";
};

struct WmsContactInformationProperty
{
	WmsContactPersonPrimaryProperty personPrimary;
	std::string positionUtf8 = "";
	WmsContactAddressProperty address;
	std::string voiceTelephoneUtf8 = "";
	std::string facsimileTelephoneUtf8 = "";
	std::string eMailAddressUtf8 = "";
};

struct WmsServiceProperty
{
	std::string titleUtf8 = "";
	std::string abstractUtf8 = "";
	std::vector<std::string> keywordsUtf8;
	WmsOnlineResourceAttribute onlineResource;
	WmsContactInformationProperty contactInformation;
	std::string feesUtf8 = "";
	std::string accessConstraintsUtf8 = "";
	size_t layerLimit = 0;
	size_t maxWidth = 0;
	size_t maxHeight = 0;
};

struct WmsDimensionProperty
{
	std::string nameUtf8 = "";
	std::string unitsUtf8 = "";
	std::string unitSymbolUtf8 = "";
	std::string defaultValueUtf8 = "";
	std::string extentUtf8 = "";
	bool multipleValues = false;
	bool nearestValue = false;
	bool current = false;

	bool operator==(const WmsDimensionProperty& other) const;
};

struct WmsLogoUrlProperty
{
	std::string formatUtf8 = "";
	WmsOnlineResourceAttribute onlineResource;
	size_t width = 0;
	size_t height = 0;
};

struct WmsAttributionProperty
{
	std::string titleUtf8 = "";
	WmsOnlineResourceAttribute onlineResource;
	WmsLogoUrlProperty logoUrl;
};

struct WmsLegendUrlProperty
{
	std::string formatUtf8 = "";
	WmsOnlineResourceAttribute onlineResource;
	size_t width = 0;
	size_t height = 0;
};

struct WmsStyleSheetUrlProperty
{
	std::string formatUtf8 = "";
	WmsOnlineResourceAttribute onlineResource;
};

struct WmsStyleUrlProperty
{
	std::string formatUtf8 = "";
	WmsOnlineResourceAttribute onlineResource;
};

struct WmsStyleProperty
{
	std::string nameUtf8 = "";
	std::string titleUtf8 = "";
	std::string abstractUtf8 = "";
	std::vector<WmsLegendUrlProperty> legendUrls;
	WmsStyleSheetUrlProperty styleSheetUrl;
	WmsStyleUrlProperty styleUrl;
};

struct WmsAuthorityUrlProperty
{
	WmsOnlineResourceAttribute onlineResource;
	std::string nameUtf8 = "";
};

struct WmsIdentifierProperty
{
	std::string authorityUtf8 = "";
};

struct WmsMetadataUrlProperty
{
	std::string formatUtf8 = "";
	WmsOnlineResourceAttribute onlineResource;
	std::string typeUtf8 = "";
};

struct WmsDataListUrlProperty
{
	std::string formatUtf8 = "";
	WmsOnlineResourceAttribute onlineResource;
};

struct WmsFeatureListUrlProperty
{
	std::string formatUtf8 = "";
	WmsOnlineResourceAttribute onlineResource;
};

struct WmsLayerProperty
{
	int orderId = -1;
	std::string nameUtf8 = "";
	std::string titleUtf8 = "";
	std::string abstractUtf8 = "";
	std::vector<std::string> keywordsUtf8;
	std::vector<std::string> crsUtf8;
	GB_Rectangle exGeographicBBox;
	std::vector<GeoBoundingBox> boundingBoxes;
	WmsAttributionProperty attribution;
	std::vector<WmsAuthorityUrlProperty> authorityUrls;
	std::vector<WmsIdentifierProperty> identifiers;
	std::vector<WmsDimensionProperty> dimensions;
	std::vector<WmsMetadataUrlProperty> metadataUrls;
	std::vector<WmsDataListUrlProperty> dataListUrls;
	std::vector<WmsFeatureListUrlProperty> featureListUrls;
	std::vector<WmsStyleProperty> styles;
	double minScaleDenominator = 0;
	double maxScaleDenominator = 0;
	std::vector<WmsLayerProperty> subLayers;
	bool queryable = false;
	int cascaded = 0;
	bool opaque = false;
	bool noSubsets = false;
	int fixedWidth = 0;
	int fixedHeight = 0;

	bool IsEqual(const WmsLayerProperty& other) const;

	bool HasDimension(const std::string& dimensionNameUtf8) const;

	std::string PreferredAvailableCrs() const;
};

class GB_DateTime;
struct WmstDates
{
	std::vector<GB_DateTime> dateTimes;

	WmstDates();
	WmstDates(const std::vector<GB_DateTime>& dateTimes);
	bool operator==(const WmstDates& other) const;
};

struct WmstExtentPair
{
	WmstDates dates;
	GB_TimeDuration resolution;

	WmstExtentPair();

	WmstExtentPair(const WmstDates& dates, const GB_TimeDuration& resolution);

	bool operator==(const WmstExtentPair& other) const;
};

struct WmstDimensionExtent
{
	std::vector<WmstExtentPair> datesResolutionList;
};

struct WmtsTheme
{
	std::string identifierUtf8 = "";
	std::string titleUtf8 = "";
	std::string abstractUtf8 = "";
	std::vector<std::string> keywordsUtf8;
	WmtsTheme* subTheme = nullptr;
	std::vector<std::string> layerRefs;

	WmtsTheme();
	~WmtsTheme();
};

struct WmtsTileMatrixLimits;
struct WmtsTileMatrix
{
	std::string identifierUtf8 = "";
	std::string titleUtf8 = "";
	std::string abstractUtf8 = "";
	std::vector<std::string> keywordsUtf8;
	double scaleDenominator = 0;
	GB_Point2d topLeft;
	int tileWidth = 0;
	int tileHeight = 0;
	int matrixWidth = 0;
	int matrixHeight = 0;
	double tres = 0;

	GB_Rectangle TileRect(int tileCol, int tileRow) const;

	bool Intersects(const GB_Rectangle& rect, const WmtsTileMatrixLimits* tileMatrixLimits, GB_IntInterval& colIndexInterval, GB_IntInterval& rowIndexInterval) const;
};

struct WmtsTileMatrixSet
{
	std::string identifierUtf8 = "";
	std::string titleUtf8 = "";
	std::string abstractUtf8 = "";
	std::vector<std::string> keywordsUtf8;
	std::string crsUtf8 = "";
	std::string wkScaleSetUtf8 = "";
	std::map<double, WmtsTileMatrix> tileMatrices;

	const WmtsTileMatrix* FindNearestResolution(double targetTileResolution) const;

	const WmtsTileMatrix* FindOtherResolution(double targetTileResolution, int offset) const;
};

enum class MapTileMode
{
	WMTS,
	WMSC,
	XYZ
};

struct WmtsTileMatrixLimits
{
	std::string tileMatrixIdentifierUtf8 = "";
	GB_IntInterval rowIndexInterval;
	GB_IntInterval colIndexInterval;
};

struct WmtsTileMatrixSetLink
{
	std::string tileMatrixSetIdentifierUtf8 = "";
	std::unordered_map<std::string, WmtsTileMatrixLimits> limits;
};

struct WmtsLegendUrl
{
	std::string formatUtf8 = "";
	double minScale = 0, maxScale = 0;
	std::string hrefUtf8 = "";
	int width = 0, height = 0;
};

struct WmtsStyle
{
	std::string identifierUtf8 = "";
	std::string titleUtf8 = "", abstractUtf8 = "";
	std::vector<std::string> keywordsUtf8;
	bool isDefault = false;
	std::vector<WmtsLegendUrl> legendUrls;
};

struct WmtsDimension
{
	std::string identifierUtf8 = "";
	std::string titleUtf8 = "";
	std::string abstractUtf8 = "";
	std::vector<std::string> keywordsUtf8;
	std::string unitOfMeasureUtf8 = "";
	std::string unitSymbolUtf8 = "";
	std::string defaultValueUtf8 = "";
	bool current = false;
	std::vector<std::string> values;
};

struct WmtsTileLayer
{
	MapTileMode tileMode;
	std::string identifierUtf8 = "";
	std::string titleUtf8 = "";
	std::string abstractUtf8 = "";
	std::vector<std::string> keywordsUtf8;
	std::vector<GeoBoundingBox> boundingBoxes;
	std::vector<std::string> formats;
	std::vector<std::string> infoFormats;
	std::string defaultStyleUtf8 = "";
	int dpi = -1;
	std::unordered_map<std::string, WmtsDimension> dimensions;
	std::unordered_map<std::string, WmtsStyle> styles;
	std::unordered_map<std::string, WmtsTileMatrixSetLink> setLinks;
	std::unordered_map<std::string, std::string> getTileUrls;
	std::unordered_map<std::string, std::string> getFeatureInfoUrls;
	std::string timeDimensionIdentifierUtf8 = "";
	std::vector<GB_DateTimeInterval> allTimeRanges;
	GB_DateTimeInterval temporalExtent;
	GB_TimeDuration temporalResolution;

	enum class WmtsTimeFormat
	{
		YYYYMMDD,
		YYYY_MM_DD,
		YYYY,
		YYYYMMDDYYYYMMDDPxx,
		YYYYMMDDTHHMMSSZ
	};
	WmtsTimeFormat timeFormat = WmtsTimeFormat::YYYYMMDD;

	enum class TemporalCapabilityFlag
	{
		RequestedTimesMustExactlyMatchAllAvailableTemporalRanges
	};
	TemporalCapabilityFlag temporalCapabilityFlags;

	std::string defaultTimeDimensionValueUtf8 = "";
};

struct WmsCapabilityProperty
{
	WmsRequestProperty request;
	WmsExceptionProperty exception;
	std::vector<WmsLayerProperty> layers;
	std::vector<WmtsTileLayer> tileLayers;
	std::unordered_map<std::string, WmtsTileMatrixSet> tileMatrixSets;
};

struct WmsCapabilitiesProperty
{
	WmsServiceProperty service;
	WmsCapabilityProperty capability;
	std::string versionUtf8 = "";
};

struct WmsSupportedFormat
{
	std::string formatUtf8 = "";
	std::string labelUtf8 = "";
};

enum class WmsDpiMode
{
	DpiNone = 0,
	DpiQGIS = 1,
	DpiUMN = 2,
	DpiGeoServer = 4,
	DpiAll = DpiQGIS | DpiUMN | DpiGeoServer,
};



#endif