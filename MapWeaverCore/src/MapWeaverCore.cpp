#include "MapWeaverCore.h"
#include "GB_Utf8String.h"
#include "GB_Network.h"
#include "GB_Logger.h"
#include "GeoCrsManager.h"
#include "GeoCrsTransform.h"

#include "cpl_minixml.h"
#include "cpl_error.h"
#include "cpl_string.h"
#include "cpl_json.h"

#include "ogr_spatialref.h"
#include "ogr_srs_api.h"
#include "cpl_conv.h"

#include "GB_Crypto.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <iomanip>
#include <limits>
#include <ogr_core.h>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

constexpr static double tiandituRenderingPixelSize = 0.0254 / 96;	// 天地图：0.0254 米（1 英寸）除以 96 像素（常见屏幕分辨率）
constexpr static double standardRenderingPixelSize = 0.00028;		// 标准：0.28 毫米（0.00028 米）

constexpr static double oldArcGISServerMetersPerUnit = 111194.8722222222405;		// ArcGIS Server 10.3 及以前的版本的“每度多少米”

enum class RasterIdentifyFormat
{
	Unknown = 0,
	Value = 1,
	Text = 1 << 1,
	Html = 1 << 2,
	Feature = 1 << 3
};

bool IsUrlForWMTS(const std::string& urlUtf8)
{
	// 包含 SERVICE=WMTS 键值对（KVP 风格）
	if (GB_Utf8Find(urlUtf8, GB_STR("SERVICE=WMTS"), false) >= 0)
	{
		return true;
	}

	// RESTful 风格的 WMTS
	if (GB_Utf8Find(urlUtf8, GB_STR("/WMTSCapabilities.xml"), false) >= 0)
	{
		return true;
	}

	// 匹配 "/wmts?", "/wmts/"
	if (GB_Utf8Find(urlUtf8, GB_STR("/wmts?"), false) >= 0 || GB_Utf8Find(urlUtf8, GB_STR("/wmts/"), false) >= 0)
	{
		return true;
	}

	// 匹配正好以 "/wmts" 结尾的情况
	if (GB_Utf8EndsWith(urlUtf8, GB_STR("/wmts"), false))
	{
		return true;
	}

	return false;
}

static inline std::string ConvertRawBytesToUtf8(const std::string& rawBytes)
{
	if (rawBytes.empty())
	{
		return rawBytes;
	}

	try
	{
		const size_t searchLength = std::min<size_t>(rawBytes.size(), 512);
		const std::string xmlHead = rawBytes.substr(0, searchLength);

		const int64_t encodingKeyPos = GB_Utf8Find(xmlHead, GB_STR("encoding"), false);
		if (encodingKeyPos < 0)
		{
			return rawBytes;
		}

		const int64_t equalPos = GB_Utf8Find(xmlHead, GB_STR("="), false, encodingKeyPos + 8);
		if (equalPos < 0)
		{
			return rawBytes;
		}

		int64_t quotePos = -1;
		char quoteCharacter = '\0';
		for (int64_t i = equalPos + 1; i < static_cast<int64_t>(xmlHead.size()); i++)
		{
			const char currentCharacter = xmlHead[static_cast<size_t>(i)];
			if (currentCharacter == '"' || currentCharacter == '\'')
			{
				quoteCharacter = currentCharacter;
				quotePos = i;
				break;
			}
			if (!std::isspace(static_cast<unsigned char>(currentCharacter)))
			{
				break;
			}
		}

		if (quotePos < 0)
		{
			return rawBytes;
		}

		int64_t endQuotePos = -1;
		for (int64_t i = quotePos + 1; i < static_cast<int64_t>(xmlHead.size()); i++)
		{
			if (xmlHead[static_cast<size_t>(i)] == quoteCharacter)
			{
				endQuotePos = i;
				break;
			}
		}

		if (endQuotePos <= quotePos + 1)
		{
			return rawBytes;
		}

		const std::string encoding = GB_Utf8Substr(xmlHead, quotePos + 1, endQuotePos - quotePos - 1);
		if (GB_Utf8Equals(encoding, GB_STR("utf-8"), false) || GB_Utf8Equals(encoding, GB_STR("utf8"), false))
		{
			return rawBytes;
		}

		return GB_BytesToUtf8(rawBytes, encoding);
	}
	catch (const std::exception& ex)
	{
		GBLOG_WARNING(GB_Utf8Format("Exception while converting raw bytes to UTF-8: %s. Defaulting to original bytes.", ex.what()));
		return rawBytes;
	}
}

bool DownloadWmsCapabilities(const std::string& rawUrlUtf8, std::string& outCapabilitiesXmlUtf8, const GB_NetworkRequestOptions& options)
{
	outCapabilitiesXmlUtf8.clear();

	std::string urlUtf8 = rawUrlUtf8;
	if (!IsUrlForWMTS(urlUtf8))
	{
		urlUtf8 = GB_UrlOperator::SetUrlQueryValue(urlUtf8, GB_STR("SERVICE"), GB_STR("WMS"));
		urlUtf8 = GB_UrlOperator::SetUrlQueryValue(urlUtf8, GB_STR("REQUEST"), GB_STR("GetCapabilities"));
	}

	GB_NetworkResponse response;
	for (int i = 0; i < 2; i++)
	{
		response = GB_RequestUrlData(urlUtf8, options);
		if (response.ok)
		{
			const std::string& rawString = response.body;
			outCapabilitiesXmlUtf8 = ConvertRawBytesToUtf8(rawString);
			return true;
		}
	}
	GBLOG_WARNING(GB_Utf8Format("Failed to download WMS capabilities from URL: '%s'. HTTP status code: %ld. Error message: %s", urlUtf8.c_str(), response.httpStatusCode, response.errorMessageUtf8.c_str()));

	response = GB_RequestUrlData(rawUrlUtf8, options);
	if (response.ok)
	{
		const std::string rawString = response.body;
		outCapabilitiesXmlUtf8 = ConvertRawBytesToUtf8(rawString);
		return true;
	}

	GBLOG_WARNING(GB_Utf8Format("Failed to download WMS capabilities from original URL: '%s'. HTTP status code: %ld. Error message: %s", rawUrlUtf8.c_str(), response.httpStatusCode, response.errorMessageUtf8.c_str()));

	return false;
}

namespace
{
	static std::string Trim(const std::string& text)
	{
		size_t beginIndex = 0;
		while (beginIndex < text.size() && std::isspace(static_cast<unsigned char>(text[beginIndex])))
		{
			beginIndex++;
		}

		size_t endIndex = text.size();
		while (endIndex > beginIndex && std::isspace(static_cast<unsigned char>(text[endIndex - 1])))
		{
			endIndex--;
		}

		return text.substr(beginIndex, endIndex - beginIndex);
	}

	static inline std::string ToLowerAscii(const std::string& text)
	{
		std::string lowerText = text;
		std::transform(lowerText.begin(), lowerText.end(), lowerText.begin(), [](unsigned char character) -> char
			{
				return static_cast<char>(std::tolower(character));
			});
		return lowerText;
	}

	static std::vector<std::string> SplitCommaSeparated(const std::string& text)
	{
		std::vector<std::string> values;
		std::stringstream stream(text);
		std::string item;
		while (std::getline(stream, item, ','))
		{
			const std::string trimmedItem = Trim(item);
			if (!trimmedItem.empty())
			{
				values.push_back(trimmedItem);
			}
		}
		return values;
	}

	static inline bool ParseInt64(const std::string& text, long long* value)
	{
		if (!value)
		{
			return false;
		}

		const std::string trimmedText = Trim(text);
		if (trimmedText.empty())
		{
			return false;
		}

		errno = 0;
		char* endPointer = nullptr;
		const long long parsedValue = std::strtoll(trimmedText.c_str(), &endPointer, 10);
		if (ERANGE == errno || nullptr == endPointer || '\0' != *endPointer)
		{
			return false;
		}

		*value = parsedValue;
		return true;
	}

	static inline bool ParseDouble(const std::string& text, double* value)
	{
		if (!value)
		{
			return false;
		}

		const std::string trimmedText = Trim(text);
		if (trimmedText.empty())
		{
			return false;
		}

		errno = 0;
		char* endPointer = nullptr;
		const double parsedValue = std::strtod(trimmedText.c_str(), &endPointer);
		if (ERANGE == errno || nullptr == endPointer || '\0' != *endPointer)
		{
			return false;
		}

		*value = parsedValue;
		return true;
	}

	static inline bool HasUsableValue(const CPLJSONObject& jsonValue)
	{
		if (!jsonValue.IsValid())
		{
			return false;
		}

		const CPLJSONObject::Type valueType = jsonValue.GetType();
		return CPLJSONObject::Type::Unknown != valueType && CPLJSONObject::Type::Null != valueType;
	}

	static inline bool TryGetChild(const CPLJSONObject& jsonObject, const std::string& key, CPLJSONObject* childValue)
	{
		if (!childValue)
		{
			return false;
		}

		const CPLJSONObject value = jsonObject[key];
		if (!HasUsableValue(value))
		{
			return false;
		}

		*childValue = value;
		return true;
	}

	static inline bool TryGetString(const CPLJSONObject& jsonValue, std::string* text)
	{
		if (!text || !HasUsableValue(jsonValue))
		{
			return false;
		}

		const CPLJSONObject::Type valueType = jsonValue.GetType();
		switch (valueType)
		{
		case CPLJSONObject::Type::String:
		case CPLJSONObject::Type::Integer:
		case CPLJSONObject::Type::Long:
		case CPLJSONObject::Type::Double:
		case CPLJSONObject::Type::Boolean:
			*text = jsonValue.ToString();
			return true;
		default:
			return false;
		}
	}

	static inline bool TryGetInt(const CPLJSONObject& jsonValue, int* integerValue)
	{
		if (!integerValue || !HasUsableValue(jsonValue))
		{
			return false;
		}

		const CPLJSONObject::Type valueType = jsonValue.GetType();
		switch (valueType)
		{
		case CPLJSONObject::Type::Integer:
		case CPLJSONObject::Type::Long:
		{
			const long long value = jsonValue.ToLong();
			if (value < static_cast<long long>(std::numeric_limits<int>::min()) ||
				value > static_cast<long long>(std::numeric_limits<int>::max()))
			{
				return false;
			}

			*integerValue = static_cast<int>(value);
			return true;
		}

		case CPLJSONObject::Type::String:
		{
			long long value = 0;
			if (!ParseInt64(jsonValue.ToString(), &value))
			{
				return false;
			}

			if (value < static_cast<long long>(std::numeric_limits<int>::min()) ||
				value > static_cast<long long>(std::numeric_limits<int>::max()))
			{
				return false;
			}

			*integerValue = static_cast<int>(value);
			return true;
		}

		default:
			return false;
		}
	}

	static inline bool TryGetDouble(const CPLJSONObject& jsonValue, double* doubleValue)
	{
		if (!doubleValue || !HasUsableValue(jsonValue))
		{
			return false;
		}

		const CPLJSONObject::Type valueType = jsonValue.GetType();
		switch (valueType)
		{
		case CPLJSONObject::Type::Integer:
		case CPLJSONObject::Type::Long:
		case CPLJSONObject::Type::Double:
			*doubleValue = jsonValue.ToDouble();
			return true;

		case CPLJSONObject::Type::String:
			return ParseDouble(jsonValue.ToString(), doubleValue);

		default:
			return false;
		}
	}

	static inline bool TryGetBool(const CPLJSONObject& jsonValue, bool* boolValue)
	{
		if (!boolValue || !HasUsableValue(jsonValue))
		{
			return false;
		}

		const CPLJSONObject::Type valueType = jsonValue.GetType();
		switch (valueType)
		{
		case CPLJSONObject::Type::Boolean:
			*boolValue = jsonValue.ToBool();
			return true;

		case CPLJSONObject::Type::Integer:
		case CPLJSONObject::Type::Long:
		{
			const long long value = jsonValue.ToLong();
			if (0 == value)
			{
				*boolValue = false;
				return true;
			}
			if (1 == value)
			{
				*boolValue = true;
				return true;
			}
			return false;
		}

		case CPLJSONObject::Type::String:
		{
			const std::string lowerText = ToLowerAscii(Trim(jsonValue.ToString()));
			if ("true" == lowerText || "1" == lowerText)
			{
				*boolValue = true;
				return true;
			}
			if ("false" == lowerText || "0" == lowerText)
			{
				*boolValue = false;
				return true;
			}
			return false;
		}

		default:
			return false;
		}
	}

	static inline bool TryGetMemberString(const CPLJSONObject& jsonObject, const std::string& key, std::string* text)
	{
		CPLJSONObject childValue;
		if (!TryGetChild(jsonObject, key, &childValue))
		{
			return false;
		}

		return TryGetString(childValue, text);
	}

	static inline bool TryGetMemberInt(const CPLJSONObject& jsonObject, const std::string& key, int* integerValue)
	{
		CPLJSONObject childValue;
		if (!TryGetChild(jsonObject, key, &childValue))
		{
			return false;
		}

		return TryGetInt(childValue, integerValue);
	}

	static inline bool TryGetMemberDouble(const CPLJSONObject& jsonObject, const std::string& key, double* doubleValue)
	{
		CPLJSONObject childValue;
		if (!TryGetChild(jsonObject, key, &childValue))
		{
			return false;
		}

		return TryGetDouble(childValue, doubleValue);
	}

	static inline bool TryGetMemberBool(const CPLJSONObject& jsonObject, const std::string& key, bool* boolValue)
	{
		CPLJSONObject childValue;
		if (!TryGetChild(jsonObject, key, &childValue))
		{
			return false;
		}

		return TryGetBool(childValue, boolValue);
	}

	static inline bool ParseSpatialReference(const CPLJSONObject& jsonObject, ArcGISSpatialReference* spatialReference)
	{
		if (!spatialReference || !HasUsableValue(jsonObject) || CPLJSONObject::Type::Object != jsonObject.GetType())
		{
			return false;
		}

		ArcGISSpatialReference parsedSpatialReference;
		bool hasAnyField = false;

		if (TryGetMemberInt(jsonObject, "wkid", &parsedSpatialReference.m_wkid))
		{
			hasAnyField = true;
		}

		if (TryGetMemberInt(jsonObject, "latestWkid", &parsedSpatialReference.m_latestWkid))
		{
			hasAnyField = true;
		}

		if (TryGetMemberString(jsonObject, "wkt", &parsedSpatialReference.m_wkt))
		{
			hasAnyField = true;
		}

		if (!hasAnyField)
		{
			return false;
		}

		*spatialReference = parsedSpatialReference;
		return true;
	}

	static inline bool ParseExtent(const CPLJSONObject& jsonObject, ArcGISExtent* extent)
	{
		if (!extent || !HasUsableValue(jsonObject) || CPLJSONObject::Type::Object != jsonObject.GetType())
		{
			return false;
		}

		ArcGISExtent parsedExtent;

		const bool hasXmin = TryGetMemberDouble(jsonObject, "xmin", &parsedExtent.m_xmin);
		const bool hasYmin = TryGetMemberDouble(jsonObject, "ymin", &parsedExtent.m_ymin);
		const bool hasXmax = TryGetMemberDouble(jsonObject, "xmax", &parsedExtent.m_xmax);
		const bool hasYmax = TryGetMemberDouble(jsonObject, "ymax", &parsedExtent.m_ymax);

		parsedExtent.m_isValid = hasXmin && hasYmin && hasXmax && hasYmax;

		CPLJSONObject spatialReferenceValue;
		if (TryGetChild(jsonObject, "spatialReference", &spatialReferenceValue))
		{
			ParseSpatialReference(spatialReferenceValue, &parsedExtent.m_spatialReference);
		}

		*extent = parsedExtent;
		return true;
	}

	static inline bool ParseLayerInfo(const CPLJSONObject& jsonObject, ArcGISLayerInfo* layerInfo)
	{
		if (!layerInfo || !HasUsableValue(jsonObject) || CPLJSONObject::Type::Object != jsonObject.GetType())
		{
			return false;
		}

		ArcGISLayerInfo parsedLayerInfo;

		TryGetMemberInt(jsonObject, "id", &parsedLayerInfo.m_id);
		TryGetMemberString(jsonObject, "name", &parsedLayerInfo.m_name);
		TryGetMemberInt(jsonObject, "parentLayerId", &parsedLayerInfo.m_parentLayerId);
		TryGetMemberBool(jsonObject, "defaultVisibility", &parsedLayerInfo.m_defaultVisibility);
		TryGetMemberDouble(jsonObject, "minScale", &parsedLayerInfo.m_minScale);
		TryGetMemberDouble(jsonObject, "maxScale", &parsedLayerInfo.m_maxScale);

		CPLJSONObject subLayerIdsValue;
		if (TryGetChild(jsonObject, "subLayerIds", &subLayerIdsValue) && CPLJSONObject::Type::Array == subLayerIdsValue.GetType())
		{
			const CPLJSONArray subLayerIdsArray = subLayerIdsValue.ToArray();
			parsedLayerInfo.m_subLayerIds.reserve(subLayerIdsArray.Size());

			for (int i = 0; i < subLayerIdsArray.Size(); i++)
			{
				const CPLJSONObject subLayerIdValue = subLayerIdsArray[i];
				int subLayerId = -1;
				if (TryGetInt(subLayerIdValue, &subLayerId))
				{
					parsedLayerInfo.m_subLayerIds.push_back(subLayerId);
				}
			}
		}

		*layerInfo = parsedLayerInfo;
		return true;
	}

	static void ParseLayerInfoArray(const CPLJSONObject& jsonValue, std::vector<ArcGISLayerInfo>* layerInfos)
	{
		if (!layerInfos)
		{
			return;
		}

		layerInfos->clear();

		if (!HasUsableValue(jsonValue) || CPLJSONObject::Type::Array != jsonValue.GetType())
		{
			return;
		}

		const CPLJSONArray jsonArray = jsonValue.ToArray();
		layerInfos->reserve(jsonArray.Size());

		for (int i = 0; i < jsonArray.Size(); i++)
		{
			ArcGISLayerInfo layerInfo;
			if (ParseLayerInfo(jsonArray[i], &layerInfo))
			{
				layerInfos->push_back(layerInfo);
			}
		}
	}

	static inline bool ParseLodInfo(const CPLJSONObject& jsonObject, ArcGISLodInfo* lodInfo)
	{
		if (!lodInfo || !HasUsableValue(jsonObject) || CPLJSONObject::Type::Object != jsonObject.GetType())
		{
			return false;
		}

		ArcGISLodInfo parsedLodInfo;
		TryGetMemberInt(jsonObject, "level", &parsedLodInfo.m_level);
		TryGetMemberDouble(jsonObject, "resolution", &parsedLodInfo.m_resolution);
		TryGetMemberDouble(jsonObject, "scale", &parsedLodInfo.m_scale);

		*lodInfo = parsedLodInfo;
		return true;
	}

	static bool ParseTileInfo(const CPLJSONObject& jsonObject, ArcGISTileInfo* tileInfo)
	{
		if (!tileInfo || !HasUsableValue(jsonObject) || CPLJSONObject::Type::Object != jsonObject.GetType())
		{
			return false;
		}

		ArcGISTileInfo parsedTileInfo;

		TryGetMemberInt(jsonObject, "rows", &parsedTileInfo.m_rows);
		TryGetMemberInt(jsonObject, "cols", &parsedTileInfo.m_cols);
		TryGetMemberInt(jsonObject, "dpi", &parsedTileInfo.m_dpi);
		TryGetMemberString(jsonObject, "format", &parsedTileInfo.m_format);
		TryGetMemberInt(jsonObject, "compressionQuality", &parsedTileInfo.m_compressionQuality);

		CPLJSONObject originValue;
		if (TryGetChild(jsonObject, "origin", &originValue) && CPLJSONObject::Type::Object == originValue.GetType())
		{
			TryGetMemberDouble(originValue, "x", &parsedTileInfo.m_origin.x);
			TryGetMemberDouble(originValue, "y", &parsedTileInfo.m_origin.y);
		}

		CPLJSONObject spatialReferenceValue;
		if (TryGetChild(jsonObject, "spatialReference", &spatialReferenceValue))
		{
			ParseSpatialReference(spatialReferenceValue, &parsedTileInfo.m_spatialReference);
		}

		CPLJSONObject lodsValue;
		if (TryGetChild(jsonObject, "lods", &lodsValue) && CPLJSONObject::Type::Array == lodsValue.GetType())
		{
			const CPLJSONArray lodsArray = lodsValue.ToArray();
			parsedTileInfo.m_lods.reserve(lodsArray.Size());

			for (int i = 0; i < lodsArray.Size(); i++)
			{
				ArcGISLodInfo lodInfo;
				if (ParseLodInfo(lodsArray[i], &lodInfo))
				{
					parsedTileInfo.m_lods.push_back(lodInfo);
				}
			}
		}

		*tileInfo = parsedTileInfo;
		return true;
	}

	static inline void ParseDocumentInfo(const CPLJSONObject& jsonObject, ArcGISDocumentInfo* documentInfo)
	{
		if (!documentInfo)
		{
			return;
		}

		ArcGISDocumentInfo parsedDocumentInfo;

		if (HasUsableValue(jsonObject) && CPLJSONObject::Type::Object == jsonObject.GetType())
		{
			TryGetMemberString(jsonObject, "Title", &parsedDocumentInfo.m_title);
			TryGetMemberString(jsonObject, "Author", &parsedDocumentInfo.m_author);
			TryGetMemberString(jsonObject, "Comments", &parsedDocumentInfo.m_comments);
			TryGetMemberString(jsonObject, "Subject", &parsedDocumentInfo.m_subject);
			TryGetMemberString(jsonObject, "Category", &parsedDocumentInfo.m_category);
			TryGetMemberString(jsonObject, "AntialiasingMode", &parsedDocumentInfo.m_antialiasingMode);
			TryGetMemberString(jsonObject, "TextAntialiasingMode", &parsedDocumentInfo.m_textAntialiasingMode);
			TryGetMemberString(jsonObject, "Keywords", &parsedDocumentInfo.m_keywords);
		}

		*documentInfo = parsedDocumentInfo;
	}

	static void PrintJsonParseDebugInfo(const std::string& jsonText)
	{
		std::cout << "GDALVersion = " << GDALVersionInfo("RELEASE_NAME") << std::endl;
		std::cout << "JsonSize = " << jsonText.size() << std::endl;

		const size_t previewLength = std::min<size_t>(jsonText.size(), 128);
		std::cout << "JsonPreview = [" << jsonText.substr(0, previewLength) << "]" << std::endl;

		std::cout << "JsonHexPreview = ";
		for (size_t i = 0; i < previewLength; i++)
		{
			const unsigned char byteValue = static_cast<unsigned char>(jsonText[i]);
			std::printf("%02X ", byteValue);
		}
		std::printf("\n");

		CPLErrorReset();

		CPLJSONDocument jsonDocument;
		const bool loadSucceeded =
			jsonDocument.LoadMemory(reinterpret_cast<const GByte*>(jsonText.data()),
				static_cast<int>(jsonText.size()));

		std::cout << "LoadSucceeded = " << loadSucceeded << std::endl;
		std::cout << "LastErrorType = " << static_cast<int>(CPLGetLastErrorType()) << std::endl;
		std::cout << "LastErrorNo = " << static_cast<int>(CPLGetLastErrorNo()) << std::endl;
		std::cout << "LastErrorMsg = [" << CPLGetLastErrorMsg() << "]" << std::endl;
	}

	static bool ParseArcGISMapServerJson(const std::string& jsonText, ArcGISMapServiceInfo* mapServiceInfo, std::string* errorMessage)
	{
		if (!errorMessage)
		{
			return false;
		}
		errorMessage->clear();

		if (!mapServiceInfo)
		{
			*errorMessage = "mapServiceInfo is null.";
			return false;
		}
		*mapServiceInfo = ArcGISMapServiceInfo();
		if (jsonText.empty())
		{
			*errorMessage = "jsonText is empty.";
			return false;
		}

		CPLErrorReset();
		CPLJSONDocument jsonDocument;
		if (!jsonDocument.LoadMemory(jsonText))
			//if (!jsonDocument.LoadMemory(reinterpret_cast<const GByte*>(jsonText.data()), static_cast<int>(jsonText.size())))
		{
			const char* lastErrorMessage = CPLGetLastErrorMsg();
			*errorMessage = (nullptr != lastErrorMessage && '\0' != lastErrorMessage[0]) ? lastErrorMessage : "GDAL failed to parse json text.";
			return false;
		}

		const CPLJSONObject rootObject = jsonDocument.GetRoot();
		if (!HasUsableValue(rootObject) || CPLJSONObject::Type::Object != rootObject.GetType())
		{
			*errorMessage = "Root JSON value is not an object.";
			return false;
		}

		ArcGISMapServiceInfo parsedInfo;
		TryGetMemberString(rootObject, "currentVersion", &parsedInfo.m_currentVersion);
		TryGetMemberString(rootObject, "serviceDescription", &parsedInfo.m_serviceDescription);
		TryGetMemberString(rootObject, "mapName", &parsedInfo.m_mapName);
		TryGetMemberString(rootObject, "description", &parsedInfo.m_description);
		TryGetMemberString(rootObject, "copyrightText", &parsedInfo.m_copyrightText);
		TryGetMemberBool(rootObject, "supportsDynamicLayers", &parsedInfo.m_supportsDynamicLayers);

		CPLJSONObject layersValue;
		if (TryGetChild(rootObject, "layers", &layersValue))
		{
			ParseLayerInfoArray(layersValue, &parsedInfo.m_layers);
		}

		CPLJSONObject tablesValue;
		if (TryGetChild(rootObject, "tables", &tablesValue))
		{
			ParseLayerInfoArray(tablesValue, &parsedInfo.m_tables);
		}

		CPLJSONObject spatialReferenceValue;
		if (TryGetChild(rootObject, "spatialReference", &spatialReferenceValue))
		{
			parsedInfo.m_hasSpatialReference = ParseSpatialReference(spatialReferenceValue, &parsedInfo.m_spatialReference);
		}

		TryGetMemberBool(rootObject, "singleFusedMapCache", &parsedInfo.m_singleFusedMapCache);

		CPLJSONObject tileInfoValue;
		if (TryGetChild(rootObject, "tileInfo", &tileInfoValue))
		{
			parsedInfo.m_hasTileInfo = ParseTileInfo(tileInfoValue, &parsedInfo.m_tileInfo);
		}

		CPLJSONObject initialExtentValue;
		if (TryGetChild(rootObject, "initialExtent", &initialExtentValue))
		{
			parsedInfo.m_hasInitialExtent = ParseExtent(initialExtentValue, &parsedInfo.m_initialExtent);
		}

		CPLJSONObject fullExtentValue;
		if (TryGetChild(rootObject, "fullExtent", &fullExtentValue))
		{
			parsedInfo.m_hasFullExtent = ParseExtent(fullExtentValue, &parsedInfo.m_fullExtent);
		}

		TryGetMemberDouble(rootObject, "minScale", &parsedInfo.m_minScale);
		TryGetMemberDouble(rootObject, "maxScale", &parsedInfo.m_maxScale);
		TryGetMemberString(rootObject, "units", &parsedInfo.m_units);

		std::string supportedImageFormatTypesText;
		if (TryGetMemberString(rootObject, "supportedImageFormatTypes", &supportedImageFormatTypesText))
		{
			parsedInfo.m_supportedImageFormatTypes = SplitCommaSeparated(supportedImageFormatTypesText);
		}

		CPLJSONObject documentInfoValue;
		if (TryGetChild(rootObject, "documentInfo", &documentInfoValue))
		{
			ParseDocumentInfo(documentInfoValue, &parsedInfo.m_documentInfo);
		}

		std::string capabilitiesText;
		if (TryGetMemberString(rootObject, "capabilities", &capabilitiesText))
		{
			parsedInfo.m_capabilities = SplitCommaSeparated(capabilitiesText);
		}

		std::string supportedQueryFormatsText;
		if (TryGetMemberString(rootObject, "supportedQueryFormats", &supportedQueryFormatsText))
		{
			parsedInfo.m_supportedQueryFormats = SplitCommaSeparated(supportedQueryFormatsText);
		}

		TryGetMemberBool(rootObject, "exportTilesAllowed", &parsedInfo.m_exportTilesAllowed);
		TryGetMemberInt(rootObject, "maxRecordCount", &parsedInfo.m_maxRecordCount);
		TryGetMemberInt(rootObject, "maxImageHeight", &parsedInfo.m_maxImageHeight);
		TryGetMemberInt(rootObject, "maxImageWidth", &parsedInfo.m_maxImageWidth);

		std::string supportedExtensionsText;
		if (TryGetMemberString(rootObject, "supportedExtensions", &supportedExtensionsText))
		{
			parsedInfo.m_supportedExtensions = SplitCommaSeparated(supportedExtensionsText);
		}

		*mapServiceInfo = parsedInfo;
		return true;
	}

	static std::string MakeStableDoubleText(double value)
	{
		std::ostringstream stream;
		stream.imbue(std::locale::classic());
		stream << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
		return stream.str();
	}

	static std::string BuildRectangleBboxText(const GB_Rectangle& rectangle)
	{
		if (!rectangle.IsValid())
		{
			return "";
		}

		return MakeStableDoubleText(rectangle.minX) + "," + MakeStableDoubleText(rectangle.minY) + "," + MakeStableDoubleText(rectangle.maxX) + "," + MakeStableDoubleText(rectangle.maxY);
	}

	static bool TryParseIntStrict(const std::string& text, int& outValue)
	{
		outValue = 0;
		if (text.empty())
		{
			return false;
		}

		errno = 0;
		char* endPointer = nullptr;
		const long parsedValue = std::strtol(text.c_str(), &endPointer, 10);
		if (errno == ERANGE || !endPointer || *endPointer != '\0')
		{
			return false;
		}

		if (parsedValue < static_cast<long>(std::numeric_limits<int>::min()) || parsedValue > static_cast<long>(std::numeric_limits<int>::max()))
		{
			return false;
		}

		outValue = static_cast<int>(parsedValue);
		return true;
	}

	static bool IsZoomLevelAllowed(int zoomLevel, int minZoomLevel, int maxZoomLevel)
	{
		if (zoomLevel < 0)
		{
			return minZoomLevel < 0 && maxZoomLevel < 0;
		}

		if (minZoomLevel >= 0 && zoomLevel < minZoomLevel)
		{
			return false;
		}

		if (maxZoomLevel >= 0 && zoomLevel > maxZoomLevel)
		{
			return false;
		}

		return true;
	}

	static int GetTileMatrixZoomLevel(const WmtsTileMatrixSet& tileMatrixSet, const WmtsTileMatrix& tileMatrix)
	{
		int parsedZoomLevel = -1;
		if (TryParseIntStrict(tileMatrix.identifierUtf8, parsedZoomLevel))
		{
			return parsedZoomLevel;
		}

		int derivedZoomLevel = static_cast<int>(tileMatrixSet.tileMatrices.size()) - 1;
		for (auto it = tileMatrixSet.tileMatrices.begin(); it != tileMatrixSet.tileMatrices.end(); it++)
		{
			if (&(it->second) == &tileMatrix)
			{
				return derivedZoomLevel;
			}
			derivedZoomLevel--;
		}

		return -1;
	}

	static const WmtsTileMatrix* FindTileMatrixByZoomLevel(const WmtsTileMatrixSet& tileMatrixSet, int zoomLevel, int minZoomLevel, int maxZoomLevel)
	{
		if (zoomLevel < 0 || !IsZoomLevelAllowed(zoomLevel, minZoomLevel, maxZoomLevel))
		{
			return nullptr;
		}

		for (auto it = tileMatrixSet.tileMatrices.begin(); it != tileMatrixSet.tileMatrices.end(); it++)
		{
			const int currentZoomLevel = GetTileMatrixZoomLevel(tileMatrixSet, it->second);
			if (currentZoomLevel == zoomLevel)
			{
				return &(it->second);
			}
		}

		return nullptr;
	}

	static const WmtsTileMatrix* SelectTileMatrix(const WmtsTileMatrixSet& tileMatrixSet, double targetResolution, int requestedZoomLevel, MapRenderTargetOptions::LodSelectMode lodSelectMode, int minZoomLevel, int maxZoomLevel)
	{
		if (requestedZoomLevel >= 0)
		{
			return FindTileMatrixByZoomLevel(tileMatrixSet, requestedZoomLevel, minZoomLevel, maxZoomLevel);
		}

		if (!std::isfinite(targetResolution) || targetResolution <= 0 || tileMatrixSet.tileMatrices.empty())
		{
			return nullptr;
		}

		const WmtsTileMatrix* nearestTileMatrix = nullptr;
		const WmtsTileMatrix* notCoarserTileMatrix = nullptr;
		const WmtsTileMatrix* notFinerTileMatrix = nullptr;
		const WmtsTileMatrix* finestTileMatrix = nullptr;
		const WmtsTileMatrix* coarsestTileMatrix = nullptr;
		double nearestDelta = std::numeric_limits<double>::max();
		double bestNotCoarserResolution = -std::numeric_limits<double>::max();
		double bestNotFinerResolution = std::numeric_limits<double>::max();
		double finestResolution = std::numeric_limits<double>::max();
		double coarsestResolution = -std::numeric_limits<double>::max();

		for (auto it = tileMatrixSet.tileMatrices.begin(); it != tileMatrixSet.tileMatrices.end(); it++)
		{
			const WmtsTileMatrix& tileMatrix = it->second;
			const int zoomLevel = GetTileMatrixZoomLevel(tileMatrixSet, tileMatrix);
			if (!IsZoomLevelAllowed(zoomLevel, minZoomLevel, maxZoomLevel))
			{
				continue;
			}

			const double currentResolution = tileMatrix.tres;
			if (!std::isfinite(currentResolution) || currentResolution <= 0)
			{
				continue;
			}

			if (currentResolution < finestResolution)
			{
				finestResolution = currentResolution;
				finestTileMatrix = &tileMatrix;
			}

			if (currentResolution > coarsestResolution)
			{
				coarsestResolution = currentResolution;
				coarsestTileMatrix = &tileMatrix;
			}

			const double currentDelta = std::fabs(currentResolution - targetResolution);
			if (currentDelta < nearestDelta)
			{
				nearestDelta = currentDelta;
				nearestTileMatrix = &tileMatrix;
			}

			if (currentResolution <= targetResolution && currentResolution > bestNotCoarserResolution)
			{
				bestNotCoarserResolution = currentResolution;
				notCoarserTileMatrix = &tileMatrix;
			}

			if (currentResolution >= targetResolution && currentResolution < bestNotFinerResolution)
			{
				bestNotFinerResolution = currentResolution;
				notFinerTileMatrix = &tileMatrix;
			}
		}

		switch (lodSelectMode)
		{
		case MapRenderTargetOptions::LodSelectMode::Nearest:
			return nearestTileMatrix;
		case MapRenderTargetOptions::LodSelectMode::NotCoarserThanTarget:
			return notCoarserTileMatrix ? notCoarserTileMatrix : finestTileMatrix;
		case MapRenderTargetOptions::LodSelectMode::NotFinerThanTarget:
			return notFinerTileMatrix ? notFinerTileMatrix : coarsestTileMatrix;
		default:
			return nearestTileMatrix;
		}
	}

	static bool TryCanonicalizeCrsDefinition(const std::string& crsDefinitionUtf8, std::string& outCanonicalWktUtf8)
	{
		outCanonicalWktUtf8.clear();

		if (crsDefinitionUtf8.empty())
		{
			return false;
		}

		const std::shared_ptr<const GeoCrs> crs = GeoCrsManager::GetFromDefinitionCached(crsDefinitionUtf8);
		if (!crs || !crs->IsValid())
		{
			return false;
		}

		outCanonicalWktUtf8 = crs->ExportToWktUtf8(GeoCrs::WktFormat::Wkt2_2018, false);
		if (outCanonicalWktUtf8.empty())
		{
			outCanonicalWktUtf8 = crs->ExportToWktUtf8(GeoCrs::WktFormat::Default, false);
		}

		return !outCanonicalWktUtf8.empty();
	}

	static bool TryResolveOutputImageSize(const MapRenderTargetOptions& renderTarget, double bboxWidth, double bboxHeight, size_t inputWidth, size_t inputHeight, size_t& outWidth, size_t& outHeight)
	{
		outWidth = inputWidth;
		outHeight = inputHeight;

		if (!std::isfinite(bboxWidth) || !std::isfinite(bboxHeight) || bboxWidth <= 0 || bboxHeight <= 0)
		{
			return false;
		}

		if (outWidth == 0 && outHeight == 0)
		{
			return false;
		}

		if (renderTarget.keepAspectRatio)
		{
			if (outWidth == 0 && outHeight > 0)
			{
				const double widthValue = static_cast<double>(outHeight) * bboxWidth / bboxHeight;
				outWidth = static_cast<size_t>(std::max<double>(1.0, std::llround(widthValue)));
			}
			else if (outHeight == 0 && outWidth > 0)
			{
				const double heightValue = static_cast<double>(outWidth) * bboxHeight / bboxWidth;
				outHeight = static_cast<size_t>(std::max<double>(1.0, std::llround(heightValue)));
			}
		}

		return outWidth > 0 && outHeight > 0;
	}

	static bool ResolveTargetResolution(const BuildVisibleMapRequestItemsInput& input, const std::string& targetCrsDefinitionUtf8, const GB_Rectangle& requestBoundingBox, const WmtsTileMatrixSet* tileMatrixSet, double& outTargetResolution, int& outRequestedZoomLevel)
	{
		outTargetResolution = 0;
		outRequestedZoomLevel = -1;

		if (!requestBoundingBox.IsValid())
		{
			return false;
		}

		const double bboxWidth = requestBoundingBox.Width();
		const double bboxHeight = requestBoundingBox.Height();
		if (!std::isfinite(bboxWidth) || !std::isfinite(bboxHeight) || bboxWidth <= 0 || bboxHeight <= 0)
		{
			return false;
		}

		MapRenderTargetOptions::ResolutionMode effectiveMode = input.renderTarget.resolutionMode;
		if (effectiveMode == MapRenderTargetOptions::ResolutionMode::Auto)
		{
			if (input.renderTarget.outputImageWidth > 0 || input.renderTarget.outputImageHeight > 0)
			{
				effectiveMode = MapRenderTargetOptions::ResolutionMode::ByOutputImageSize;
			}
			else if (std::isfinite(input.renderTarget.targetResolution) && input.renderTarget.targetResolution > 0)
			{
				effectiveMode = MapRenderTargetOptions::ResolutionMode::ByResolution;
			}
			else if (std::isfinite(input.renderTarget.scaleDenominator) && input.renderTarget.scaleDenominator > 0)
			{
				effectiveMode = MapRenderTargetOptions::ResolutionMode::ByScaleDenominator;
			}
			else if (input.renderTarget.zoomLevel >= 0)
			{
				effectiveMode = MapRenderTargetOptions::ResolutionMode::ByZoomLevel;
			}
			else
			{
				return false;
			}
		}

		switch (effectiveMode)
		{
		case MapRenderTargetOptions::ResolutionMode::ByZoomLevel:
		{
			if (!tileMatrixSet)
			{
				return false;
			}

			const WmtsTileMatrix* tileMatrix = FindTileMatrixByZoomLevel(*tileMatrixSet, input.renderTarget.zoomLevel, input.minZoomLevel, input.maxZoomLevel);
			if (!tileMatrix || !std::isfinite(tileMatrix->tres) || tileMatrix->tres <= 0)
			{
				return false;
			}

			outTargetResolution = tileMatrix->tres;
			outRequestedZoomLevel = GetTileMatrixZoomLevel(*tileMatrixSet, *tileMatrix);
			return outRequestedZoomLevel >= 0 && outTargetResolution > 0;
		}

		case MapRenderTargetOptions::ResolutionMode::ByResolution:
			if (!std::isfinite(input.renderTarget.targetResolution) || input.renderTarget.targetResolution <= 0)
			{
				return false;
			}
			outTargetResolution = input.renderTarget.targetResolution;
			return true;

		case MapRenderTargetOptions::ResolutionMode::ByScaleDenominator:
		{
			if (!std::isfinite(input.renderTarget.scaleDenominator) || input.renderTarget.scaleDenominator <= 0)
			{
				return false;
			}

			const std::shared_ptr<const GeoCrs> targetCrs = GeoCrsManager::GetFromDefinitionCached(targetCrsDefinitionUtf8);
			if (!targetCrs || !targetCrs->IsValid())
			{
				return false;
			}

			const double metersPerUnit = targetCrs->GetMetersPerUnit();
			if (!std::isfinite(metersPerUnit) || metersPerUnit <= 0)
			{
				return false;
			}

			double renderingPixelSizeMeters = input.renderTarget.renderingPixelSizeMeters;
			if (!std::isfinite(renderingPixelSizeMeters) || renderingPixelSizeMeters <= 0)
			{
				renderingPixelSizeMeters = standardRenderingPixelSize;
			}

			outTargetResolution = input.renderTarget.scaleDenominator * renderingPixelSizeMeters / metersPerUnit;
			return std::isfinite(outTargetResolution) && outTargetResolution > 0;
		}

		case MapRenderTargetOptions::ResolutionMode::ByOutputImageSize:
		{
			size_t outputImageWidth = 0;
			size_t outputImageHeight = 0;
			if (!TryResolveOutputImageSize(input.renderTarget, bboxWidth, bboxHeight, input.renderTarget.outputImageWidth, input.renderTarget.outputImageHeight, outputImageWidth, outputImageHeight))
			{
				return false;
			}

			const double resolutionX = bboxWidth / static_cast<double>(outputImageWidth);
			const double resolutionY = bboxHeight / static_cast<double>(outputImageHeight);
			outTargetResolution = std::max(resolutionX, resolutionY);
			return std::isfinite(outTargetResolution) && outTargetResolution > 0;
		}

		case MapRenderTargetOptions::ResolutionMode::Auto:
			return false;
		}

		return false;
	}

	static bool ComputeWmsImageSize(const BuildVisibleMapRequestItemsInput& input, const WmsLayerProperty& wmsLayer, const GB_Rectangle& requestBoundingBox, double targetResolution, size_t serviceMaxWidth, size_t serviceMaxHeight, size_t& outWidth, size_t& outHeight)
	{
		outWidth = 0;
		outHeight = 0;

		if (!requestBoundingBox.IsValid())
		{
			return false;
		}

		const double bboxWidth = requestBoundingBox.Width();
		const double bboxHeight = requestBoundingBox.Height();
		if (!std::isfinite(bboxWidth) || !std::isfinite(bboxHeight) || bboxWidth <= 0 || bboxHeight <= 0)
		{
			return false;
		}

		size_t width = 0;
		size_t height = 0;
		switch (input.renderTarget.wmsImageSizeMode)
		{
		case MapRenderTargetOptions::WmsImageSizeMode::ByResolution:
			if (!std::isfinite(targetResolution) || targetResolution <= 0)
			{
				return false;
			}
			width = static_cast<size_t>(std::max<double>(1.0, std::ceil(bboxWidth / targetResolution)));
			height = static_cast<size_t>(std::max<double>(1.0, std::ceil(bboxHeight / targetResolution)));
			break;

		case MapRenderTargetOptions::WmsImageSizeMode::ExactWidthHeight:
			if (!TryResolveOutputImageSize(input.renderTarget, bboxWidth, bboxHeight, input.renderTarget.outputImageWidth, input.renderTarget.outputImageHeight, width, height))
			{
				return false;
			}
			break;

		case MapRenderTargetOptions::WmsImageSizeMode::FixedLongEdge:
			if (input.renderTarget.longEdgePixels == 0)
			{
				return false;
			}
			if (bboxWidth >= bboxHeight)
			{
				width = input.renderTarget.longEdgePixels;
				height = static_cast<size_t>(std::max<double>(1.0, std::llround(static_cast<double>(width) * bboxHeight / bboxWidth)));
			}
			else
			{
				height = input.renderTarget.longEdgePixels;
				width = static_cast<size_t>(std::max<double>(1.0, std::llround(static_cast<double>(height) * bboxWidth / bboxHeight)));
			}
			break;

		case MapRenderTargetOptions::WmsImageSizeMode::FitWithinMaxSize:
		{
			const size_t maxOutputImageWidth = input.renderTarget.maxOutputImageWidth;
			const size_t maxOutputImageHeight = input.renderTarget.maxOutputImageHeight;
			if (maxOutputImageWidth == 0 && maxOutputImageHeight == 0)
			{
				return false;
			}

			if (!input.renderTarget.keepAspectRatio)
			{
				width = std::max<size_t>(1, maxOutputImageWidth);
				height = std::max<size_t>(1, maxOutputImageHeight);
				break;
			}

			if (maxOutputImageWidth == 0)
			{
				height = std::max<size_t>(1, maxOutputImageHeight);
				width = static_cast<size_t>(std::max<double>(1.0, std::llround(static_cast<double>(height) * bboxWidth / bboxHeight)));
				break;
			}

			if (maxOutputImageHeight == 0)
			{
				width = std::max<size_t>(1, maxOutputImageWidth);
				height = static_cast<size_t>(std::max<double>(1.0, std::llround(static_cast<double>(width) * bboxHeight / bboxWidth)));
				break;
			}

			width = std::max<size_t>(1, maxOutputImageWidth);
			height = static_cast<size_t>(std::max<double>(1.0, std::llround(static_cast<double>(width) * bboxHeight / bboxWidth)));
			if (height > maxOutputImageHeight)
			{
				height = std::max<size_t>(1, maxOutputImageHeight);
				width = static_cast<size_t>(std::max<double>(1.0, std::llround(static_cast<double>(height) * bboxWidth / bboxHeight)));
			}
			break;
		}
		default:
			return false;
		}

		if (wmsLayer.fixedWidth > 0 && wmsLayer.fixedHeight > 0)
		{
			width = static_cast<size_t>(wmsLayer.fixedWidth);
			height = static_cast<size_t>(wmsLayer.fixedHeight);
		}
		else if (wmsLayer.fixedWidth > 0)
		{
			width = static_cast<size_t>(wmsLayer.fixedWidth);
			if (input.renderTarget.keepAspectRatio)
			{
				height = static_cast<size_t>(std::max<double>(1.0, std::llround(static_cast<double>(width) * bboxHeight / bboxWidth)));
			}
		}
		else if (wmsLayer.fixedHeight > 0)
		{
			height = static_cast<size_t>(wmsLayer.fixedHeight);
			if (input.renderTarget.keepAspectRatio)
			{
				width = static_cast<size_t>(std::max<double>(1.0, std::llround(static_cast<double>(height) * bboxWidth / bboxHeight)));
			}
		}

		if (serviceMaxWidth > 0 && width > serviceMaxWidth)
		{
			if (input.renderTarget.keepAspectRatio)
			{
				const double scale = static_cast<double>(serviceMaxWidth) / static_cast<double>(width);
				width = serviceMaxWidth;
				height = static_cast<size_t>(std::max<double>(1.0, std::llround(static_cast<double>(height) * scale)));
			}
			else
			{
				width = serviceMaxWidth;
			}
		}

		if (serviceMaxHeight > 0 && height > serviceMaxHeight)
		{
			if (input.renderTarget.keepAspectRatio)
			{
				const double scale = static_cast<double>(serviceMaxHeight) / static_cast<double>(height);
				height = serviceMaxHeight;
				width = static_cast<size_t>(std::max<double>(1.0, std::llround(static_cast<double>(width) * scale)));
			}
			else
			{
				height = serviceMaxHeight;
			}
		}

		outWidth = std::max<size_t>(1, width);
		outHeight = std::max<size_t>(1, height);
		return true;
	}


	enum class PolygonRectangleCoverageRelation
	{
		Outside = 0,
		Partial,
		Full
	};

	static bool IsFinitePoint2d(const GB_Point2d& point)
	{
		return std::isfinite(point.x) && std::isfinite(point.y);
	}

	static bool IsFiniteRectangle(const GB_Rectangle& rectangle)
	{
		return std::isfinite(rectangle.minX) && std::isfinite(rectangle.minY) && std::isfinite(rectangle.maxX) && std::isfinite(rectangle.maxY);
	}

	static bool TryGetPolygonBoundingBox(const GB_Polygon& polygon, GB_Rectangle& outBoundingBox)
	{
		outBoundingBox.Reset();
		if (!polygon.IsValid())
		{
			return false;
		}

		outBoundingBox = polygon.GetBoundingBox();
		return outBoundingBox.IsValid() && IsFiniteRectangle(outBoundingBox);
	}

	static void RemoveDuplicateAdjacentVertices(std::vector<GB_Point2d>& vertices)
	{
		if (vertices.empty())
		{
			return;
		}

		std::vector<GB_Point2d> uniqueVertices;
		uniqueVertices.reserve(vertices.size());
		for (size_t i = 0; i < vertices.size(); i++)
		{
			const GB_Point2d& vertex = vertices[i];
			if (!IsFinitePoint2d(vertex))
			{
				continue;
			}

			if (!uniqueVertices.empty())
			{
				const GB_Point2d& lastVertex = uniqueVertices.back();
				if (vertex.x == lastVertex.x && vertex.y == lastVertex.y)
				{
					continue;
				}
			}
			uniqueVertices.push_back(vertex);
		}

		if (uniqueVertices.size() >= 2)
		{
			const GB_Point2d& firstVertex = uniqueVertices.front();
			const GB_Point2d& lastVertex = uniqueVertices.back();
			if (firstVertex.x == lastVertex.x && firstVertex.y == lastVertex.y)
			{
				uniqueVertices.pop_back();
			}
		}

		vertices.swap(uniqueVertices);
	}

	static double ComputePolygonAreaFromVertices(const std::vector<GB_Point2d>& vertices)
	{
		if (vertices.size() < 3)
		{
			return 0.0;
		}

		long double signedAreaTwice = 0.0L;
		for (size_t i = 0; i < vertices.size(); i++)
		{
			const GB_Point2d& currentVertex = vertices[i];
			const GB_Point2d& nextVertex = vertices[(i + 1) % vertices.size()];
			if (!IsFinitePoint2d(currentVertex) || !IsFinitePoint2d(nextVertex))
			{
				return 0.0;
			}
			signedAreaTwice += static_cast<long double>(currentVertex.x) * static_cast<long double>(nextVertex.y) -
				static_cast<long double>(nextVertex.x) * static_cast<long double>(currentVertex.y);
		}

		const long double area = std::fabs(signedAreaTwice) * 0.5L;
		return std::isfinite(static_cast<double>(area)) ? static_cast<double>(area) : 0.0;
	}

	static bool IsInsideLeftBoundary(const GB_Point2d& point, double boundaryValue)
	{
		return point.x >= boundaryValue;
	}

	static bool IsInsideRightBoundary(const GB_Point2d& point, double boundaryValue)
	{
		return point.x <= boundaryValue;
	}

	static bool IsInsideBottomBoundary(const GB_Point2d& point, double boundaryValue)
	{
		return point.y >= boundaryValue;
	}

	static bool IsInsideTopBoundary(const GB_Point2d& point, double boundaryValue)
	{
		return point.y <= boundaryValue;
	}

	static GB_Point2d IntersectSegmentWithVerticalBoundary(const GB_Point2d& startPoint, const GB_Point2d& endPoint, double boundaryX)
	{
		const double dx = endPoint.x - startPoint.x;
		if (!std::isfinite(dx) || dx == 0.0)
		{
			return GB_Point2d(boundaryX, startPoint.y);
		}

		const double t = (boundaryX - startPoint.x) / dx;
		const double y = startPoint.y + (endPoint.y - startPoint.y) * t;
		return GB_Point2d(boundaryX, y);
	}

	static GB_Point2d IntersectSegmentWithHorizontalBoundary(const GB_Point2d& startPoint, const GB_Point2d& endPoint, double boundaryY)
	{
		const double dy = endPoint.y - startPoint.y;
		if (!std::isfinite(dy) || dy == 0.0)
		{
			return GB_Point2d(startPoint.x, boundaryY);
		}

		const double t = (boundaryY - startPoint.y) / dy;
		const double x = startPoint.x + (endPoint.x - startPoint.x) * t;
		return GB_Point2d(x, boundaryY);
	}

	static void ClipPolygonWithBoundary(
		const std::vector<GB_Point2d>& inputVertices,
		std::vector<GB_Point2d>& outputVertices,
		bool (*isInside)(const GB_Point2d&, double),
		GB_Point2d(*computeIntersection)(const GB_Point2d&, const GB_Point2d&, double),
		double boundaryValue)
	{
		outputVertices.clear();
		if (inputVertices.empty())
		{
			return;
		}

		GB_Point2d previousVertex = inputVertices.back();
		bool previousInside = isInside(previousVertex, boundaryValue);
		for (size_t i = 0; i < inputVertices.size(); i++)
		{
			const GB_Point2d& currentVertex = inputVertices[i];
			const bool currentInside = isInside(currentVertex, boundaryValue);
			if (currentInside)
			{
				if (!previousInside)
				{
					outputVertices.push_back(computeIntersection(previousVertex, currentVertex, boundaryValue));
				}
				outputVertices.push_back(currentVertex);
			}
			else if (previousInside)
			{
				outputVertices.push_back(computeIntersection(previousVertex, currentVertex, boundaryValue));
			}

			previousVertex = currentVertex;
			previousInside = currentInside;
		}

		RemoveDuplicateAdjacentVertices(outputVertices);
	}

	static bool TryClipPolygonToRectangle(const GB_Polygon& polygon, const GB_Rectangle& rectangle, std::vector<GB_Point2d>& outVertices)
	{
		outVertices.clear();
		if (!polygon.IsValid() || !rectangle.IsValid())
		{
			return false;
		}

		std::vector<GB_Point2d> workingVertices = polygon.GetVerticesAsDouble();
		RemoveDuplicateAdjacentVertices(workingVertices);
		if (workingVertices.size() < 3)
		{
			return false;
		}

		std::vector<GB_Point2d> clippedVertices;
		ClipPolygonWithBoundary(workingVertices, clippedVertices, IsInsideLeftBoundary, IntersectSegmentWithVerticalBoundary, rectangle.minX);
		if (clippedVertices.size() < 3)
		{
			return true;
		}

		ClipPolygonWithBoundary(clippedVertices, workingVertices, IsInsideRightBoundary, IntersectSegmentWithVerticalBoundary, rectangle.maxX);
		if (workingVertices.size() < 3)
		{
			return true;
		}

		ClipPolygonWithBoundary(workingVertices, clippedVertices, IsInsideBottomBoundary, IntersectSegmentWithHorizontalBoundary, rectangle.minY);
		if (clippedVertices.size() < 3)
		{
			return true;
		}

		ClipPolygonWithBoundary(clippedVertices, workingVertices, IsInsideTopBoundary, IntersectSegmentWithHorizontalBoundary, rectangle.maxY);
		RemoveDuplicateAdjacentVertices(workingVertices);
		outVertices.swap(workingVertices);
		return true;
	}

	static double ComputePolygonRegionsNetArea(const std::vector<GB_Polygon>& outerAreas, const std::vector<std::vector<GB_Polygon>>& holeAreas)
	{
		double totalArea = 0.0;
		for (size_t outerIndex = 0; outerIndex < outerAreas.size(); outerIndex++)
		{
			const double outerArea = outerAreas[outerIndex].GetUnsignedArea();
			if (std::isfinite(outerArea) && outerArea > 0.0)
			{
				totalArea += outerArea;
			}

			if (outerIndex < holeAreas.size())
			{
				for (size_t holeIndex = 0; holeIndex < holeAreas[outerIndex].size(); holeIndex++)
				{
					const double holeArea = holeAreas[outerIndex][holeIndex].GetUnsignedArea();
					if (std::isfinite(holeArea) && holeArea > 0.0)
					{
						totalArea -= holeArea;
					}
				}
			}
		}

		return totalArea;
	}

	static PolygonRectangleCoverageRelation ClassifyPolygonRectangleCoverage(const GB_Polygon& polygon, const GB_Rectangle& rectangle)
	{
		if (!polygon.IsValid() || !rectangle.IsValid())
		{
			return PolygonRectangleCoverageRelation::Outside;
		}

		const GB_Rectangle polygonBoundingBox = polygon.GetBoundingBox();
		if (!polygonBoundingBox.IsValid())
		{
			return PolygonRectangleCoverageRelation::Outside;
		}

		const GB_Rectangle intersectionBoundingBox = polygonBoundingBox.Intersected(rectangle);
		if (!intersectionBoundingBox.IsValid())
		{
			return PolygonRectangleCoverageRelation::Outside;
		}

		const double bboxIntersectionArea = intersectionBoundingBox.Area();
		if (!std::isfinite(bboxIntersectionArea) || bboxIntersectionArea <= 0.0)
		{
			return PolygonRectangleCoverageRelation::Outside;
		}

		const double rectangleArea = rectangle.Area();
		if (!std::isfinite(rectangleArea) || rectangleArea <= 0.0)
		{
			return PolygonRectangleCoverageRelation::Outside;
		}

		const double fullCoverageTolerance = std::max(1e-12 * rectangleArea, 1e-9);
		std::vector<GB_Polygon> intersectionAreas;
		std::vector<std::vector<GB_Polygon>> holeAreas;
		if (polygon.ComputeIntersection(GB_Polygon(rectangle), intersectionAreas, holeAreas))
		{
			const double totalIntersectionArea = ComputePolygonRegionsNetArea(intersectionAreas, holeAreas);
			if (!std::isfinite(totalIntersectionArea) || totalIntersectionArea <= fullCoverageTolerance)
			{
				return PolygonRectangleCoverageRelation::Outside;
			}

			if (std::fabs(totalIntersectionArea - rectangleArea) <= fullCoverageTolerance)
			{
				return PolygonRectangleCoverageRelation::Full;
			}

			return PolygonRectangleCoverageRelation::Partial;
		}

		std::vector<GB_Point2d> clippedVertices;
		if (!TryClipPolygonToRectangle(polygon, rectangle, clippedVertices))
		{
			return PolygonRectangleCoverageRelation::Outside;
		}

		const double clippedArea = ComputePolygonAreaFromVertices(clippedVertices);
		if (!std::isfinite(clippedArea) || clippedArea <= fullCoverageTolerance)
		{
			return PolygonRectangleCoverageRelation::Outside;
		}

		if (std::fabs(clippedArea - rectangleArea) <= fullCoverageTolerance)
		{
			return PolygonRectangleCoverageRelation::Full;
		}

		return PolygonRectangleCoverageRelation::Partial;
	}

	static bool TryMergeAdjacentRectangles(std::vector<GB_Rectangle>& rectangles)
	{
		if (rectangles.size() < 2)
		{
			return false;
		}

		const auto nearlyEqual = [](double left, double right) -> bool
			{
				const double tolerance = std::max(1e-12 * std::max(std::fabs(left), std::fabs(right)), 1e-9);
				return std::fabs(left - right) <= tolerance;
			};

		bool mergedAny = false;

		std::sort(rectangles.begin(), rectangles.end(), [&](const GB_Rectangle& left, const GB_Rectangle& right)
			{
				if (!nearlyEqual(left.minY, right.minY))
				{
					return left.minY < right.minY;
				}
				if (!nearlyEqual(left.maxY, right.maxY))
				{
					return left.maxY < right.maxY;
				}
				if (!nearlyEqual(left.minX, right.minX))
				{
					return left.minX < right.minX;
				}
				return left.maxX < right.maxX;
			});

		std::vector<GB_Rectangle> horizontallyMerged;
		horizontallyMerged.reserve(rectangles.size());
		for (size_t i = 0; i < rectangles.size(); i++)
		{
			GB_Rectangle currentRectangle = rectangles[i];
			if (!currentRectangle.IsValid())
			{
				continue;
			}

			while (i + 1 < rectangles.size())
			{
				const GB_Rectangle& nextRectangle = rectangles[i + 1];
				if (!nextRectangle.IsValid() ||
					!nearlyEqual(currentRectangle.minY, nextRectangle.minY) ||
					!nearlyEqual(currentRectangle.maxY, nextRectangle.maxY) ||
					!nearlyEqual(currentRectangle.maxX, nextRectangle.minX))
				{
					break;
				}

				currentRectangle.maxX = nextRectangle.maxX;
				mergedAny = true;
				i++;
			}

			horizontallyMerged.push_back(currentRectangle);
		}

		std::sort(horizontallyMerged.begin(), horizontallyMerged.end(), [&](const GB_Rectangle& left, const GB_Rectangle& right)
			{
				if (!nearlyEqual(left.minX, right.minX))
				{
					return left.minX < right.minX;
				}
				if (!nearlyEqual(left.maxX, right.maxX))
				{
					return left.maxX < right.maxX;
				}
				if (!nearlyEqual(left.minY, right.minY))
				{
					return left.minY < right.minY;
				}
				return left.maxY < right.maxY;
			});

		std::vector<GB_Rectangle> verticallyMerged;
		verticallyMerged.reserve(horizontallyMerged.size());
		for (size_t i = 0; i < horizontallyMerged.size(); i++)
		{
			GB_Rectangle currentRectangle = horizontallyMerged[i];
			if (!currentRectangle.IsValid())
			{
				continue;
			}

			while (i + 1 < horizontallyMerged.size())
			{
				const GB_Rectangle& nextRectangle = horizontallyMerged[i + 1];
				if (!nextRectangle.IsValid() ||
					!nearlyEqual(currentRectangle.minX, nextRectangle.minX) ||
					!nearlyEqual(currentRectangle.maxX, nextRectangle.maxX) ||
					!nearlyEqual(currentRectangle.maxY, nextRectangle.minY))
				{
					break;
				}

				currentRectangle.maxY = nextRectangle.maxY;
				mergedAny = true;
				i++;
			}

			verticallyMerged.push_back(currentRectangle);
		}

		rectangles.swap(verticallyMerged);
		return mergedAny;
	}

	static void BuildAdaptivePolygonRequestRectangles(const GB_Polygon& polygon, const GB_Rectangle& polygonBoundingBox, double targetResolution, std::vector<GB_Rectangle>& outRectangles)
	{
		outRectangles.clear();
		if (!polygon.IsValid() || !polygonBoundingBox.IsValid())
		{
			return;
		}

		const double polygonArea = polygonBoundingBox.Area();
		if (!std::isfinite(polygonArea) || polygonArea <= 0.0 || !std::isfinite(targetResolution) || targetResolution <= 0.0)
		{
			return;
		}

		constexpr double minBoundaryPixels = 128.0;
		std::deque<GB_Rectangle> pendingRectangles;
		pendingRectangles.push_back(polygonBoundingBox);

		while (!pendingRectangles.empty())
		{
			const GB_Rectangle currentRectangle = pendingRectangles.front();
			pendingRectangles.pop_front();
			if (!currentRectangle.IsValid())
			{
				continue;
			}

			const PolygonRectangleCoverageRelation coverageRelation = ClassifyPolygonRectangleCoverage(polygon, currentRectangle);
			if (coverageRelation == PolygonRectangleCoverageRelation::Outside)
			{
				continue;
			}

			const double pixelWidth = currentRectangle.Width() / targetResolution;
			const double pixelHeight = currentRectangle.Height() / targetResolution;
			const bool isSmallEnough =
				(std::isfinite(pixelWidth) && pixelWidth <= minBoundaryPixels) ||
				(std::isfinite(pixelHeight) && pixelHeight <= minBoundaryPixels);

			if (coverageRelation == PolygonRectangleCoverageRelation::Full || isSmallEnough)
			{
				outRectangles.push_back(currentRectangle);
				continue;
			}

			const double centerX = 0.5 * (currentRectangle.minX + currentRectangle.maxX);
			const double centerY = 0.5 * (currentRectangle.minY + currentRectangle.maxY);
			if (!std::isfinite(centerX) || !std::isfinite(centerY) ||
				centerX <= currentRectangle.minX || centerX >= currentRectangle.maxX ||
				centerY <= currentRectangle.minY || centerY >= currentRectangle.maxY)
			{
				outRectangles.push_back(currentRectangle);
				continue;
			}

			pendingRectangles.push_back(GB_Rectangle(currentRectangle.minX, currentRectangle.minY, centerX, centerY));
			pendingRectangles.push_back(GB_Rectangle(centerX, currentRectangle.minY, currentRectangle.maxX, centerY));
			pendingRectangles.push_back(GB_Rectangle(currentRectangle.minX, centerY, centerX, currentRectangle.maxY));
			pendingRectangles.push_back(GB_Rectangle(centerX, centerY, currentRectangle.maxX, currentRectangle.maxY));
		}

		while (TryMergeAdjacentRectangles(outRectangles))
		{
		}
	}

	static std::string ReplaceAllText(const std::string& text, const std::string& oldValue, const std::string& newValue)
	{
		if (oldValue.empty())
		{
			return text;
		}

		std::string result = text;
		size_t startPosition = 0;
		while (true)
		{
			const size_t foundPosition = result.find(oldValue, startPosition);
			if (foundPosition == std::string::npos)
			{
				break;
			}

			result.replace(foundPosition, oldValue.size(), newValue);
			startPosition = foundPosition + newValue.size();
		}
		return result;
	}

	static std::string GetDefaultDimensionValue(const WmtsDimension& dimension)
	{
		if (!dimension.defaultValueUtf8.empty())
		{
			return dimension.defaultValueUtf8;
		}
		return dimension.values.empty() ? "" : dimension.values[0];
	}

	static std::string BuildWmtsRestfulUrl(const WmtsTileLayer& tileLayer, const std::string& formatName, const std::string& styleName, const std::string& tileMatrixSetName, const WmtsTileMatrix& tileMatrix, int tileRow, int tileCol)
	{
		auto templateIterator = tileLayer.getTileUrls.find(formatName);
		if (templateIterator == tileLayer.getTileUrls.end() || templateIterator->second.empty())
		{
			return "";
		}

		std::string urlUtf8 = templateIterator->second;
		const std::vector<GB_UrlOperator::UrlKeyValue> urlKeyValues = {
			GB_UrlOperator::UrlKeyValue("Layer", tileLayer.identifierUtf8),
			GB_UrlOperator::UrlKeyValue("Style", styleName),
			GB_UrlOperator::UrlKeyValue("TileMatrixSet", tileMatrixSetName),
			GB_UrlOperator::UrlKeyValue("TileMatrix", tileMatrix.identifierUtf8),
			GB_UrlOperator::UrlKeyValue("TileRow", std::to_string(tileRow)),
			GB_UrlOperator::UrlKeyValue("TileCol", std::to_string(tileCol)),
			GB_UrlOperator::UrlKeyValue("Format", formatName)
		};
		urlUtf8 = GB_UrlOperator::ReplaceUrlPathParams(urlUtf8, urlKeyValues);

		for (auto it = tileLayer.dimensions.begin(); it != tileLayer.dimensions.end(); it++)
		{
			const std::string dimensionValue = GetDefaultDimensionValue(it->second);
			if (dimensionValue.empty())
			{
				continue;
			}

			urlUtf8 = ReplaceAllText(urlUtf8, "{" + it->second.identifierUtf8 + "}", dimensionValue);
			urlUtf8 = ReplaceAllText(urlUtf8, "{" + it->first + "}", dimensionValue);
		}

		return urlUtf8;
	}

	static std::string GetFirstHttpGetUrl(const std::vector<WmsDcpTypeProperty>& dcpTypes)
	{
		for (size_t i = 0; i < dcpTypes.size(); i++)
		{
			const std::string& urlUtf8 = dcpTypes[i].http.get.onlineResource.xlinkHrefUtf8;
			if (!urlUtf8.empty())
			{
				return urlUtf8;
			}
		}
		return "";
	}

	static std::string BuildWmtsKvpUrl(const BuildVisibleMapRequestItemsInput& input, const WmtsTileLayer& tileLayer, const std::string& styleName, const std::string& tileMatrixSetName, const std::string& formatName, const WmtsTileMatrix& tileMatrix, int tileRow, int tileCol)
	{
		std::string urlUtf8 = GetFirstHttpGetUrl(input.capabilities->capability.request.getTile.dcpTypes);
		if (urlUtf8.empty())
		{
			urlUtf8 = input.mapServiceUrlUtf8;
		}
		if (urlUtf8.empty())
		{
			return "";
		}

		const std::string versionUtf8 = input.capabilities->versionUtf8.empty() ? "1.0.0" : input.capabilities->versionUtf8;
		urlUtf8 = GB_UrlOperator::SetUrlQueryValue(urlUtf8, "SERVICE", "WMTS");
		urlUtf8 = GB_UrlOperator::SetUrlQueryValue(urlUtf8, "REQUEST", "GetTile");
		urlUtf8 = GB_UrlOperator::SetUrlQueryValue(urlUtf8, "VERSION", versionUtf8);
		urlUtf8 = GB_UrlOperator::SetUrlQueryValue(urlUtf8, "LAYER", tileLayer.identifierUtf8);
		urlUtf8 = GB_UrlOperator::SetUrlQueryValue(urlUtf8, "STYLE", styleName);
		urlUtf8 = GB_UrlOperator::SetUrlQueryValue(urlUtf8, "TILEMATRIXSET", tileMatrixSetName);
		urlUtf8 = GB_UrlOperator::SetUrlQueryValue(urlUtf8, "TILEMATRIX", tileMatrix.identifierUtf8);
		urlUtf8 = GB_UrlOperator::SetUrlQueryValue(urlUtf8, "TILEROW", std::to_string(tileRow));
		urlUtf8 = GB_UrlOperator::SetUrlQueryValue(urlUtf8, "TILECOL", std::to_string(tileCol));
		urlUtf8 = GB_UrlOperator::SetUrlQueryValue(urlUtf8, "FORMAT", formatName);

		for (auto it = tileLayer.dimensions.begin(); it != tileLayer.dimensions.end(); it++)
		{
			const std::string dimensionValue = GetDefaultDimensionValue(it->second);
			if (!dimensionValue.empty())
			{
				urlUtf8 = GB_UrlOperator::SetUrlQueryValue(urlUtf8, it->second.identifierUtf8, dimensionValue);
			}
		}

		return urlUtf8;
	}

	static std::string BuildWmsRequestBboxText(const std::string& versionUtf8, const std::string& crsDefinitionUtf8, const GB_Rectangle& requestBoundingBox)
	{
		if (!requestBoundingBox.IsValid())
		{
			return "";
		}

		const bool isWms130OrLater = GB_Utf8StartsWith(versionUtf8, "1.3", true) || GB_Utf8StartsWith(versionUtf8, "2.", true);
		const bool shouldReverseAxis =
			isWms130OrLater &&
			GeoCrsManager::IsDefinitionValidCached(crsDefinitionUtf8) &&
			GeoCrsManager::IsDefinitionAxisOrderReversedCached(crsDefinitionUtf8);

		if (!shouldReverseAxis)
		{
			return BuildRectangleBboxText(requestBoundingBox);
		}

		return MakeStableDoubleText(requestBoundingBox.minY) + "," +
			MakeStableDoubleText(requestBoundingBox.minX) + "," +
			MakeStableDoubleText(requestBoundingBox.maxY) + "," +
			MakeStableDoubleText(requestBoundingBox.maxX);
	}

	static std::string BuildWmsGetMapUrl(const BuildVisibleMapRequestItemsInput& input, const WmsLayerProperty& wmsLayer, const std::string& styleName, const std::string& crsDefinitionUtf8, const std::string& formatName, const GB_Rectangle& requestBoundingBox, size_t imageWidth, size_t imageHeight)
	{
		std::string urlUtf8 = GetFirstHttpGetUrl(input.capabilities->capability.request.getMap.dcpTypes);
		if (urlUtf8.empty())
		{
			urlUtf8 = input.mapServiceUrlUtf8;
		}
		if (urlUtf8.empty())
		{
			return "";
		}

		const std::string versionUtf8 = input.capabilities->versionUtf8.empty() ? "1.3.0" : input.capabilities->versionUtf8;
		const bool isWms130OrLater = GB_Utf8StartsWith(versionUtf8, "1.3", true) || GB_Utf8StartsWith(versionUtf8, "2.", true);
		const std::string crsParameterName = isWms130OrLater ? "CRS" : "SRS";

		urlUtf8 = GB_UrlOperator::SetUrlQueryValue(urlUtf8, "SERVICE", "WMS");
		urlUtf8 = GB_UrlOperator::SetUrlQueryValue(urlUtf8, "REQUEST", "GetMap");
		urlUtf8 = GB_UrlOperator::SetUrlQueryValue(urlUtf8, "VERSION", versionUtf8);
		urlUtf8 = GB_UrlOperator::SetUrlQueryValue(urlUtf8, "LAYERS", wmsLayer.nameUtf8);
		urlUtf8 = GB_UrlOperator::SetUrlQueryValue(urlUtf8, "STYLES", styleName);
		urlUtf8 = GB_UrlOperator::SetUrlQueryValue(urlUtf8, crsParameterName, crsDefinitionUtf8);
		urlUtf8 = GB_UrlOperator::SetUrlQueryValue(urlUtf8, "FORMAT", formatName);
		urlUtf8 = GB_UrlOperator::SetUrlQueryValue(urlUtf8, "WIDTH", std::to_string(imageWidth));
		urlUtf8 = GB_UrlOperator::SetUrlQueryValue(urlUtf8, "HEIGHT", std::to_string(imageHeight));
		urlUtf8 = GB_UrlOperator::SetUrlQueryValue(urlUtf8, "BBOX", BuildWmsRequestBboxText(versionUtf8, crsDefinitionUtf8, requestBoundingBox));

		for (size_t i = 0; i < wmsLayer.dimensions.size(); i++)
		{
			const WmsDimensionProperty& dimension = wmsLayer.dimensions[i];
			if (!dimension.nameUtf8.empty() && !dimension.defaultValueUtf8.empty())
			{
				urlUtf8 = GB_UrlOperator::SetUrlQueryValue(urlUtf8, dimension.nameUtf8, dimension.defaultValueUtf8);
			}
		}

		return urlUtf8;
	}
}

bool RequestArcGISServerJson(const std::string& urlUtf8, ArcGISMapServiceInfo& mapServiceInfo, const GB_NetworkRequestOptions& options)
{
	mapServiceInfo = ArcGISMapServiceInfo();

	const static std::vector<std::string> knownArcGISServerFeatureCodes =
	{
		"/FeatureServer",
		"/ImageServer",
		"/MapServer",
		"/SceneServer",
		"/VectorTileServer"
	};

	std::vector<std::string> candidateUrls;
	candidateUrls.reserve(knownArcGISServerFeatureCodes.size() + 1);

	const size_t queryPos = urlUtf8.find('?');
	const std::string pathPart = (std::string::npos == queryPos) ? urlUtf8 : urlUtf8.substr(0, queryPos);
	const std::string queryPart = (std::string::npos == queryPos) ? "" : urlUtf8.substr(queryPos);

	for (size_t i = 0; i < knownArcGISServerFeatureCodes.size(); i++)
	{
		const std::string& featureCode = knownArcGISServerFeatureCodes[i];
		const int64_t index = GB_Utf8FindLast(pathPart, featureCode, false);
		if (index < 0)
		{
			continue;
		}

		const std::string serviceRootUrl = pathPart.substr(0, static_cast<size_t>(index) + featureCode.size()) + queryPart;
		const std::string candidateUrl = GB_UrlOperator::SetUrlQueryValue(serviceRootUrl, "f", "pjson");
		if (std::find(candidateUrls.begin(), candidateUrls.end(), candidateUrl) == candidateUrls.end())
		{
			candidateUrls.push_back(candidateUrl);
		}
	}

	const std::string originalPjsonUrl = GB_UrlOperator::SetUrlQueryValue(urlUtf8, "f", "pjson");
	if (std::find(candidateUrls.begin(), candidateUrls.end(), originalPjsonUrl) == candidateUrls.end())
	{
		candidateUrls.push_back(originalPjsonUrl);
	}

	GB_NetworkResponse lastResponse;
	std::string lastParseErrorMessage;
	for (size_t i = 0; i < candidateUrls.size(); i++)
	{
		const std::string& candidateUrl = candidateUrls[i];
		lastResponse = GB_RequestUrlData(candidateUrl, options);
		if (!lastResponse.ok)
		{
			continue;
		}

		std::string parseErrorMessage;
		if (ParseArcGISMapServerJson(lastResponse.body, &mapServiceInfo, &parseErrorMessage))
		{
			return true;
		}

		lastParseErrorMessage = parseErrorMessage;
	}

	GBLOG_WARNING(GB_Utf8Format(
		"Failed to request or parse ArcGIS Server JSON from URL '%s'. Last HTTP status code: %ld. Last network error: %s. Last parse error: %s",
		urlUtf8.c_str(),
		lastResponse.httpStatusCode,
		lastResponse.errorMessageUtf8.c_str(),
		lastParseErrorMessage.c_str()));
	return false;
}

namespace
{
	static inline std::string GetXmlNodeTagName(const CPLXMLNode* node)
	{
		if (!node || node->eType != CXT_Element || !node->pszValue)
		{
			return "";
		}
		return node->pszValue;
	}

	static inline std::string GetXmlNodeAttribute(const CPLXMLNode* node, const std::string& attributeName)
	{
		if (!node || node->eType != CXT_Element)
		{
			return "";
		}

		const char* value = CPLGetXMLValue(node, attributeName.c_str(), "");
		if (!value)
		{
			return "";
		}

		return value;
	}

	static inline std::string GetXmlNodeValue(const CPLXMLNode* node)
	{
		if (!node)
		{
			return "";
		}

		if (node->eType == CXT_Text)
		{
			return node->pszValue ? node->pszValue : "";
		}

		const char* value = CPLGetXMLValue(node, nullptr, "");
		if (!value)
		{
			return "";
		}
		return value;
	}

	static CPLXMLNode* FindChildElement(const CPLXMLNode* parent, const std::string& childName)
	{
		if (!parent || parent->eType != CXT_Element)
		{
			return nullptr;
		}

		for (CPLXMLNode* curNode = parent->psChild; curNode != nullptr; curNode = curNode->psNext)
		{
			if (curNode->eType == CXT_Element)
			{
				const std::string nodeName = GetXmlNodeTagName(curNode);
				if (GB_Utf8Equals(nodeName, childName, false))
				{
					return curNode;
				}
			}
		}
		return nullptr;
	}

	static inline bool IsAsciiDigit(char ch)
	{
		return ch >= '0' && ch <= '9';
	}

	static bool ParseVersionComponent(const std::string& versionText, size_t& position, const unsigned int capValue, unsigned int& componentValue)
	{
		if (position >= versionText.size() || !IsAsciiDigit(versionText[position]))
		{
			return false;
		}

		unsigned int value = 0;

		while (position < versionText.size() && IsAsciiDigit(versionText[position]))
		{
			const unsigned int digit = static_cast<unsigned int>(versionText[position] - '0');

			if (value < capValue)
			{
				if (value > (capValue - digit) / 10)
				{
					value = capValue;
				}
				else
				{
					value = value * 10 + digit;
					if (value > capValue)
					{
						value = capValue;
					}
				}
			}

			position++;
		}

		componentValue = value;
		return true;
	}

	// 判断 versionText 是否是“10.3 及以前的版本”
	static bool IsVersionAtMost10_3(const std::string& versionText)
	{
		if (versionText.empty())
		{
			return false;
		}

		size_t position = 0;

		unsigned int majorVersion = 0;
		if (!ParseVersionComponent(versionText, position, 11, majorVersion))
		{
			return false;
		}

		// 只有一个分段，例如 "10"、"9"、"11"
		if (position == versionText.size())
		{
			return majorVersion <= 10;
		}

		if (versionText[position] != '.')
		{
			return false;
		}
		position++;

		unsigned int minorVersion = 0;
		if (!ParseVersionComponent(versionText, position, 4, minorVersion))
		{
			return false;
		}

		// 剩余分段只需要保证“格式合法”，不需要真的参与比较。
		while (position < versionText.size())
		{
			if (versionText[position] != '.')
			{
				return false;
			}
			position++;

			if (position >= versionText.size() || !IsAsciiDigit(versionText[position]))
			{
				return false;
			}

			while (position < versionText.size() && IsAsciiDigit(versionText[position]))
			{
				position++;
			}
		}

		return (majorVersion < 10) || (majorVersion == 10 && minorVersion <= 3);
	}

	class WmsCapabilitiesParser
	{
	public:
		WmsCapabilitiesParser() : valid(false), parserOptions(), numLayers(-1) {}

		void SetArcGISMapServiceInfo(const ArcGISMapServiceInfo* arcGISMapServiceInfo)
		{
			this->arcGISMapServiceInfo = arcGISMapServiceInfo;
		}

		bool Parse(const std::string& capabilitiesXmlUtf8, const WmsParserOptions& options, const std::string& baseUrl)
		{
			parserOptions = options;
			valid = false;

			if (capabilitiesXmlUtf8.empty())
			{
				GBLOG_WARNING("Empty capabilities XML.");
				return false;
			}

			if (GB_Utf8StartsWith(capabilitiesXmlUtf8, GB_STR("<html>"), false))
			{
				GBLOG_WARNING("Capabilities XML appears to be an HTML page, likely an error response.");
				return false;
			}

			if (capabilitiesXmlUtf8.find('\0') != std::string::npos)
			{
				GBLOG_WARNING("Capabilities XML contains null bytes, which is unexpected and may indicate a malformed response.");
				return false;
			}

			if (!ParseDom(capabilitiesXmlUtf8, capabilities))
			{
				GBLOG_WARNING("Failed to parse capabilities XML.");
				return false;
			}

			for (const std::string& formatValue : capabilities.capability.request.getFeatureInfo.formatsUtf8)
			{
				RasterIdentifyFormat format = RasterIdentifyFormat::Unknown;
				if (GB_Utf8Equals(formatValue, GB_STR("MIME"), false) || GB_Utf8Equals(formatValue, GB_STR("text/plain"), false))
				{
					format = RasterIdentifyFormat::Text;
				}
				else if (GB_Utf8Equals(formatValue, GB_STR("text/html"), false))
				{
					format = RasterIdentifyFormat::Html;
				}
				else if (GB_Utf8StartsWith(formatValue, GB_STR("GML."), false) || GB_Utf8Equals(formatValue, GB_STR("application/vnd.ogc.gml"), false) || GB_Utf8Equals(formatValue, GB_STR("application/json"), false) ||
					GB_Utf8Equals(formatValue, GB_STR("application/geojson"), false) || GB_Utf8Equals(formatValue, GB_STR("application/geo+json"), false) || GB_Utf8Find(formatValue, GB_STR("gml"), false) > 0 ||
					(GB_Utf8Equals(formatValue, GB_STR("text/xml"), false) && GB_Utf8Find(baseUrl, GB_STR("MapServer"), false) < 0))
				{
					format = RasterIdentifyFormat::Feature;
				}
				identifyFormats[format] = formatValue;
			}
			return true;
		}

		WmsCapabilitiesProperty GetCapabilities() const
		{
			return capabilities;
		}

		std::vector<WmsLayerProperty> GetWmsLayers() const
		{
			return layersSupported;
		}

		std::vector<std::string> GetSupportedImageEncodings() const
		{
			return capabilities.capability.request.getMap.formatsUtf8;
		}

		void GetLayerParents(std::unordered_map<int, int>& parents) const
		{
			parents = layerParentIdMap;
		}

		std::vector<WmtsTileLayer> GetTileLayers() const
		{
			return tileLayersSupported;
		}

		std::unordered_map<std::string, WmtsTileMatrixSet> GetTileMatrixSets() const
		{
			return tileMatrixSets;
		}

	private:
		bool valid = false;
		const ArcGISMapServiceInfo* arcGISMapServiceInfo = nullptr;
		WmsParserOptions parserOptions;
		int numLayers = -1;
		std::unordered_map<int, int> layerParentIdMap; // layerId -> parentLayerId
		std::unordered_map<int, std::vector<std::string>> layerParentNamesMap; // layerId -> parentLayerNames
		std::unordered_map<std::string, bool> queryableLayerIdCache; // layerId -> isQueryable
		std::vector<WmsLayerProperty> layersSupported;
		std::vector<WmtsTileLayer> tileLayersSupported;
		std::vector<WmtsTheme> tileThemes;
		std::unordered_map<std::string, WmtsTileMatrixSet> tileMatrixSets;
		std::string firstTileMatrixSetId = "";
		WmsCapabilitiesProperty capabilities;
		std::unordered_map<RasterIdentifyFormat, std::string> identifyFormats;

		bool ParseDom(const std::string& capabilitiesXmlUtf8, WmsCapabilitiesProperty& capabilitiesProperty)
		{
			CPLErrorReset();
			CPLPushErrorHandler(CPLQuietErrorHandler);
			CPLXMLNode* xmlTreePtr = CPLParseXMLString(capabilitiesXmlUtf8.c_str());
			CPLPopErrorHandler();
			if (!xmlTreePtr)
			{
				const std::string errorMessage = CPLGetLastErrorMsg();
				GBLOG_WARNING(GB_Utf8Format("Failed to parse capabilities XML. Error message: %s", errorMessage.c_str()));
				return false;
			}

			CPLXMLTreeCloser xmlDom(xmlTreePtr);
			CPLXMLNode* rootNode = xmlDom.getDocumentElement();
			if (!rootNode)
			{
				GBLOG_WARNING("Capabilities XML does not contain a root element.");
				return false;
			}

			// 检查根元素名称
			{
				const std::string rootName = GetXmlNodeTagName(rootNode);
				if (!GB_Utf8Equals(rootName, GB_STR("WMS_Capabilities"), false) && !GB_Utf8Equals(rootName, GB_STR("WMT_MS_Capabilities"), false) &&
					!GB_Utf8Equals(rootName, GB_STR("Capabilities"), false))
				{
					GBLOG_WARNING(GB_Utf8Format("Unexpected root element name in capabilities XML: '%s'.", rootName.c_str()));
					return false;
				}
			}

			capabilitiesProperty.versionUtf8 = GetXmlNodeAttribute(rootNode, GB_STR("version"));

			for (CPLXMLNode* curNode = rootNode->psChild; curNode != nullptr; curNode = curNode->psNext)
			{
				if (curNode->eType != CXT_Element)
				{
					continue;
				}

				const std::string nodeName = GetXmlNodeTagName(curNode);
				if (GB_Utf8Equals(nodeName, GB_STR("Service"), false) || GB_Utf8Equals(nodeName, GB_STR("ows:ServiceProvider"), false) ||
					GB_Utf8Equals(nodeName, GB_STR("ows:ServiceIdentification"), false))
				{
					ParseService(curNode, capabilitiesProperty.service);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("Capability"), false) || GB_Utf8Equals(nodeName, GB_STR("ows:OperationsMetadata"), false))
				{
					ParseCapability(curNode, capabilitiesProperty.capability);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("Contents"), false))
				{
					ParseWMTSContents(curNode);
				}
			}
			return true;
		}

		void ParseService(const CPLXMLNode* rootNode, WmsServiceProperty& serviceProperty)
		{
			if (!rootNode)
			{
				return;
			}

			for (CPLXMLNode* curNode = rootNode->psChild; curNode != nullptr; curNode = curNode->psNext)
			{
				if (curNode->eType != CXT_Element)
				{
					continue;
				}

				std::string nodeName = GetXmlNodeTagName(curNode);
				if (GB_Utf8StartsWith(nodeName, GB_STR("wms:"), false) || GB_Utf8StartsWith(nodeName, GB_STR("ows:"), false))
				{
					nodeName = GB_Utf8Substr(nodeName, 4);
				}

				if (GB_Utf8Equals(nodeName, GB_STR("Title"), false))
				{
					serviceProperty.titleUtf8 = GetXmlNodeValue(curNode);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("Abstract"), false))
				{
					serviceProperty.abstractUtf8 = GetXmlNodeValue(curNode);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("KeywordList"), false) || GB_Utf8Equals(nodeName, GB_STR("Keywords"), false))
				{
					ParseKeywordList(curNode, serviceProperty.keywordsUtf8);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("OnlineResource"), false))
				{
					ParseOnlineResource(curNode, serviceProperty.onlineResource);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("ContactInformation"), false) || GB_Utf8Equals(nodeName, GB_STR("ServiceContact"), false))
				{
					ParseContactInformation(curNode, serviceProperty.contactInformation);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("Fees"), false))
				{
					serviceProperty.feesUtf8 = GetXmlNodeValue(curNode);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("AccessConstraints"), false))
				{
					serviceProperty.accessConstraintsUtf8 = GetXmlNodeValue(curNode);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("LayerLimit"), false))
				{
					try
					{
						serviceProperty.layerLimit = static_cast<size_t>(std::stoull(GetXmlNodeValue(curNode)));
					}
					catch (const std::exception& e)
					{
						GBLOG_WARNING(GB_Utf8Format("Failed to parse LayerLimit value: '%s'. Error message: %s", GetXmlNodeValue(curNode).c_str(), e.what()));
						serviceProperty.layerLimit = 0;
					}
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("MaxWidth"), false))
				{
					try
					{
						serviceProperty.maxWidth = static_cast<size_t>(std::stoull(GetXmlNodeValue(curNode)));
					}
					catch (const std::exception& e)
					{
						GBLOG_WARNING(GB_Utf8Format("Failed to parse MaxWidth value: '%s'. Error message: %s", GetXmlNodeValue(curNode).c_str(), e.what()));
						serviceProperty.maxWidth = 0;
					}
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("MaxHeight"), false))
				{
					try
					{
						serviceProperty.maxHeight = static_cast<size_t>(std::stoull(GetXmlNodeValue(curNode)));
					}
					catch (const std::exception& e)
					{
						GBLOG_WARNING(GB_Utf8Format("Failed to parse MaxHeight value: '%s'. Error message: %s", GetXmlNodeValue(curNode).c_str(), e.what()));
						serviceProperty.maxHeight = 0;
					}
				}
			}
		}

		void ParseCapability(const CPLXMLNode* rootNode, WmsCapabilityProperty& capabilityProperty)
		{
			if (!rootNode)
			{
				return;
			}

			for (CPLXMLNode* curNode = rootNode->psChild; curNode != nullptr; curNode = curNode->psNext)
			{
				if (curNode->eType != CXT_Element)
				{
					continue;
				}

				std::string nodeName = GetXmlNodeTagName(curNode);
				if (GB_Utf8StartsWith(nodeName, GB_STR("wms:"), false))
				{
					nodeName = GB_Utf8Substr(nodeName, 4);
				}

				if (GB_Utf8Equals(nodeName, GB_STR("Request"), false))
				{
					ParseRequest(curNode, capabilityProperty.request);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("Layer"), false))
				{
					WmsLayerProperty layer;
					ParseLayer(curNode, layer);
					capabilityProperty.layers.push_back(std::move(layer));
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("VendorSpecificCapabilities"), false))
				{
					for (CPLXMLNode* vendorNode = curNode->psChild; vendorNode != nullptr; vendorNode = vendorNode->psNext)
					{
						if (vendorNode->eType != CXT_Element)
						{
							continue;
						}

						std::string vendorNodeName = GetXmlNodeTagName(vendorNode);
						if (GB_Utf8StartsWith(vendorNodeName, GB_STR("wms:"), false))
						{
							vendorNodeName = GB_Utf8Substr(vendorNodeName, 4);
						}

						if (GB_Utf8Equals(vendorNodeName, GB_STR("TileSet"), false))
						{
							ParseTileSetProfile(vendorNode);
						}
					}
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("ows:Operation"), false) || GB_Utf8Equals(nodeName, GB_STR("Operation"), false))
				{
					const std::string operationName = GetXmlNodeAttribute(curNode, GB_STR("name"));
					CPLXMLNode* dcpNode = FindChildElement(curNode, GB_STR("ows:DCP"));
					if (dcpNode)
					{
						CPLXMLNode* httpNode = FindChildElement(dcpNode, GB_STR("ows:HTTP"));
						if (httpNode)
						{
							CPLXMLNode* getNode = FindChildElement(httpNode, GB_STR("ows:Get"));
							if (getNode)
							{
								const std::string href = GetXmlNodeAttribute(getNode, GB_STR("xlink:href"));
								WmsDcpTypeProperty dcp;
								dcp.http.get.onlineResource.xlinkHrefUtf8 = href;

								WmsOperationType* operationType = nullptr;
								if (!href.empty())
								{
									if (GB_Utf8Equals(operationName, GB_STR("GetTile"), false))
									{
										operationType = &capabilityProperty.request.getTile;
									}
									else if (GB_Utf8Equals(operationName, GB_STR("GetFeatureInfo"), false))
									{
										operationType = &capabilityProperty.request.getFeatureInfo;
									}
									else if (GB_Utf8Equals(operationName, GB_STR("GetLegendGraphic"), false) || GB_Utf8Equals(operationName, GB_STR("sld:GetLegendGraphic"), false))
									{
										operationType = &capabilityProperty.request.getLegendGraphic;
									}
								}

								if (operationType)
								{
									operationType->dcpTypes.push_back(dcp);
									operationType->allowedEncodingsUtf8.clear();

									CPLXMLNode* constraintsNode = FindChildElement(getNode, GB_STR("ows:Constraint"));
									if (constraintsNode)
									{
										CPLXMLNode* allowedValuesNode = FindChildElement(constraintsNode, GB_STR("ows:AllowedValues"));
										if (allowedValuesNode)
										{
											for (CPLXMLNode* valueNode = allowedValuesNode->psChild; valueNode != nullptr; valueNode = valueNode->psNext)
											{
												if (valueNode->eType != CXT_Element)
												{
													continue;
												}

												const std::string valueNodeName = GetXmlNodeTagName(valueNode);
												if (GB_Utf8Equals(valueNodeName, GB_STR("ows:Value"), false))
												{
													const std::string encodingValue = GetXmlNodeValue(valueNode);
													if (!encodingValue.empty())
													{
														operationType->allowedEncodingsUtf8.push_back(encodingValue);
													}
												}
											}
										}
									}
								}
							}
						}
					}
				}
			}

			if (!tileLayersSupported.empty())
			{
				std::unordered_map<std::string, std::string> titles;
				std::unordered_map<std::string, std::string> abstracts;
				for (const WmsLayerProperty& layer : layersSupported)
				{
					if (layer.nameUtf8.empty())
					{
						continue;
					}

					if (!layer.titleUtf8.empty())
					{
						titles[layer.nameUtf8] = layer.titleUtf8;
					}

					if (!layer.abstractUtf8.empty())
					{
						abstracts[layer.nameUtf8] = layer.abstractUtf8;
					}
				}

				for (WmtsTileLayer& tileLayer : tileLayersSupported)
				{
					if (tileLayer.titleUtf8.empty() && titles.find(tileLayer.identifierUtf8) != titles.end())
					{
						tileLayer.titleUtf8 = titles[tileLayer.identifierUtf8];
					}

					if (tileLayer.abstractUtf8.empty() && abstracts.find(tileLayer.identifierUtf8) != abstracts.end())
					{
						tileLayer.abstractUtf8 = abstracts[tileLayer.identifierUtf8];
					}
				}
			}
		}

		void ParseKeywordList(const CPLXMLNode* rootNode, std::vector<std::string>& keywordListProperty)
		{
			if (!rootNode)
			{
				return;
			}

			for (CPLXMLNode* curNode = rootNode->psChild; curNode != nullptr; curNode = curNode->psNext)
			{
				if (curNode->eType != CXT_Element)
				{
					continue;
				}

				std::string nodeName = GetXmlNodeTagName(curNode);
				if (GB_Utf8StartsWith(nodeName, GB_STR("wms:"), false) || GB_Utf8StartsWith(nodeName, GB_STR("ows:"), false))
				{
					nodeName = GB_Utf8Substr(nodeName, 4);
				}

				if (GB_Utf8Equals(nodeName, GB_STR("Keyword"), false))
				{
					keywordListProperty.push_back(GetXmlNodeValue(curNode));
				}
			}
		}

		void ParseKeywords(const CPLXMLNode* rootNode, std::vector<std::string>& keywords)
		{
			if (!rootNode)
			{
				return;
			}

			keywords.clear();

			const CPLXMLNode* keywordsNode = FindChildElement(rootNode, GB_STR("ows:Keywords"));
			if (!keywordsNode)
			{
				return;
			}

			for (CPLXMLNode* keywordNode = keywordsNode->psChild; keywordNode != nullptr; keywordNode = keywordNode->psNext)
			{
				if (keywordNode->eType != CXT_Element)
				{
					continue;
				}

				const std::string keywordNodeName = GetXmlNodeTagName(keywordNode);
				if (!GB_Utf8Equals(keywordNodeName, GB_STR("ows:Keyword"), false))
				{
					continue;
				}

				keywords.push_back(GetXmlNodeValue(keywordNode));
			}
		}

		void ParseOnlineResource(const CPLXMLNode* rootNode, WmsOnlineResourceAttribute& onlineResourceAttribute)
		{
			if (!rootNode)
			{
				return;
			}
			const std::string url = GetXmlNodeAttribute(rootNode, GB_STR("xlink:href"));
			if (url.empty())
			{
				return;
			}
			onlineResourceAttribute.xlinkHrefUtf8 = url;
		}

		void ParseContactInformation(const CPLXMLNode* rootNode, WmsContactInformationProperty& contactInformationProperty)
		{
			if (!rootNode)
			{
				return;
			}

			for (CPLXMLNode* curNode = rootNode->psChild; curNode != nullptr; curNode = curNode->psNext)
			{
				if (curNode->eType != CXT_Element)
				{
					continue;
				}

				std::string nodeName = GetXmlNodeTagName(curNode);
				if (GB_Utf8StartsWith(nodeName, GB_STR("wms:"), false))
				{
					nodeName = GB_Utf8Substr(nodeName, 4);
				}

				if (GB_Utf8Equals(nodeName, GB_STR("ContactPersonPrimary"), false))
				{
					ParseContactPersonPrimary(curNode, contactInformationProperty.personPrimary);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("ContactPosition"), false) || GB_Utf8Equals(nodeName, GB_STR("ows:PositionName"), false))
				{
					contactInformationProperty.positionUtf8 = GetXmlNodeValue(curNode);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("ContactAddress"), false))
				{
					ParseContactAddress(curNode, contactInformationProperty.address);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("ContactVoiceTelephone"), false))
				{
					contactInformationProperty.voiceTelephoneUtf8 = GetXmlNodeValue(curNode);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("ContactFacsimileTelephone"), false))
				{
					contactInformationProperty.facsimileTelephoneUtf8 = GetXmlNodeValue(curNode);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("ContactElectronicMailAddress"), false))
				{
					contactInformationProperty.eMailAddressUtf8 = GetXmlNodeValue(curNode);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("ows:IndividualName"), false))
				{
					contactInformationProperty.personPrimary.contactPersonUtf8 = GetXmlNodeValue(curNode);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("ows:ProviderName"), false))
				{
					contactInformationProperty.personPrimary.contactOrganizationUtf8 = GetXmlNodeValue(curNode);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("ows:ContactInfo"), false))
				{
					const CPLXMLNode* phoneNode = FindChildElement(curNode, GB_STR("ows:Phone"));
					if (phoneNode)
					{
						const CPLXMLNode* voiceNode = FindChildElement(phoneNode, GB_STR("ows:Voice"));
						if (voiceNode)
						{
							contactInformationProperty.voiceTelephoneUtf8 = GetXmlNodeValue(voiceNode);
						}

						const CPLXMLNode* facsimileNode = FindChildElement(phoneNode, GB_STR("ows:Facsimile"));
						if (facsimileNode)
						{
							contactInformationProperty.facsimileTelephoneUtf8 = GetXmlNodeValue(facsimileNode);
						}
					}

					const CPLXMLNode* addressNode = FindChildElement(curNode, GB_STR("ows:Address"));
					if (addressNode)
					{
						const CPLXMLNode* electronicMailAddressNode = FindChildElement(addressNode, GB_STR("ows:ElectronicMailAddress"));
						if (electronicMailAddressNode)
						{
							contactInformationProperty.eMailAddressUtf8 = GetXmlNodeValue(electronicMailAddressNode);
						}

						const CPLXMLNode* deliveryPointNode = FindChildElement(addressNode, GB_STR("ows:DeliveryPoint"));
						if (deliveryPointNode)
						{
							contactInformationProperty.address.addressUtf8 = GetXmlNodeValue(deliveryPointNode);
						}

						const CPLXMLNode* cityNode = FindChildElement(addressNode, GB_STR("ows:City"));
						if (cityNode)
						{
							contactInformationProperty.address.cityUtf8 = GetXmlNodeValue(cityNode);
						}

						const CPLXMLNode* administrativeAreaNode = FindChildElement(addressNode, GB_STR("ows:AdministrativeArea"));
						if (administrativeAreaNode)
						{
							contactInformationProperty.address.stateOrProvinceUtf8 = GetXmlNodeValue(administrativeAreaNode);
						}

						const CPLXMLNode* postalCodeNode = FindChildElement(addressNode, GB_STR("ows:PostalCode"));
						if (postalCodeNode)
						{
							contactInformationProperty.address.postCodeUtf8 = GetXmlNodeValue(postalCodeNode);
						}

						const CPLXMLNode* countryNode = FindChildElement(addressNode, GB_STR("ows:Country"));
						if (countryNode)
						{
							contactInformationProperty.address.countryUtf8 = GetXmlNodeValue(countryNode);
						}
					}
				}
			}
		}

		void ParseContactPersonPrimary(const CPLXMLNode* rootNode, WmsContactPersonPrimaryProperty& contactPersonPrimaryProperty)
		{
			if (!rootNode)
			{
				return;
			}

			for (CPLXMLNode* curNode = rootNode->psChild; curNode != nullptr; curNode = curNode->psNext)
			{
				if (curNode->eType != CXT_Element)
				{
					continue;
				}

				std::string nodeName = GetXmlNodeTagName(curNode);
				if (GB_Utf8StartsWith(nodeName, GB_STR("wms:"), false))
				{
					nodeName = GB_Utf8Substr(nodeName, 4);
				}

				if (GB_Utf8Equals(nodeName, GB_STR("ContactPerson"), false))
				{
					contactPersonPrimaryProperty.contactPersonUtf8 = GetXmlNodeValue(curNode);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("ContactOrganization"), false))
				{
					contactPersonPrimaryProperty.contactOrganizationUtf8 = GetXmlNodeValue(curNode);
				}
			}
		}

		void ParseContactAddress(const CPLXMLNode* rootNode, WmsContactAddressProperty& contactAddressProperty)
		{
			if (!rootNode)
			{
				return;
			}

			for (CPLXMLNode* curNode = rootNode->psChild; curNode != nullptr; curNode = curNode->psNext)
			{
				if (curNode->eType != CXT_Element)
				{
					continue;
				}

				std::string nodeName = GetXmlNodeTagName(curNode);
				if (GB_Utf8StartsWith(nodeName, GB_STR("wms:"), false))
				{
					nodeName = GB_Utf8Substr(nodeName, 4);
				}

				if (GB_Utf8Equals(nodeName, GB_STR("AddressType"), false))
				{
					contactAddressProperty.addressTypeUtf8 = GetXmlNodeValue(curNode);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("Address"), false))
				{
					contactAddressProperty.addressUtf8 = GetXmlNodeValue(curNode);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("City"), false))
				{
					contactAddressProperty.cityUtf8 = GetXmlNodeValue(curNode);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("StateOrProvince"), false))
				{
					contactAddressProperty.stateOrProvinceUtf8 = GetXmlNodeValue(curNode);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("PostCode"), false))
				{
					contactAddressProperty.postCodeUtf8 = GetXmlNodeValue(curNode);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("Country"), false))
				{
					contactAddressProperty.countryUtf8 = GetXmlNodeValue(curNode);
				}
			}
		}

		void ParseRequest(const CPLXMLNode* rootNode, WmsRequestProperty& requestProperty)
		{
			if (!rootNode)
			{
				return;
			}

			for (CPLXMLNode* curNode = rootNode->psChild; curNode != nullptr; curNode = curNode->psNext)
			{
				if (curNode->eType != CXT_Element)
				{
					continue;
				}

				std::string operation = GetXmlNodeTagName(curNode);
				if (GB_Utf8Equals(operation, GB_STR("Operation"), false))
				{
					operation = GetXmlNodeAttribute(curNode, GB_STR("name"));
				}

				if (GB_Utf8StartsWith(operation, GB_STR("GetMap"), false))
				{
					ParseOperationType(curNode, requestProperty.getMap);
				}
				else if (GB_Utf8StartsWith(operation, GB_STR("GetFeatureInfo"), false))
				{
					ParseOperationType(curNode, requestProperty.getFeatureInfo);
				}
				else if (GB_Utf8StartsWith(operation, GB_STR("GetLegendGraphic"), false) || GB_Utf8StartsWith(operation, GB_STR("sld:GetLegendGraphic"), false))
				{
					ParseOperationType(curNode, requestProperty.getLegendGraphic);
				}
			}
		}

		void ParseOperationType(const CPLXMLNode* rootNode, WmsOperationType& operationType)
		{
			if (!rootNode)
			{
				return;
			}

			for (CPLXMLNode* curNode = rootNode->psChild; curNode != nullptr; curNode = curNode->psNext)
			{
				if (curNode->eType != CXT_Element)
				{
					continue;
				}

				std::string nodeName = GetXmlNodeTagName(curNode);
				if (GB_Utf8StartsWith(nodeName, GB_STR("wms:"), false))
				{
					nodeName = GB_Utf8Substr(nodeName, 4);
				}

				if (GB_Utf8Equals(nodeName, GB_STR("Format"), false))
				{
					operationType.formatsUtf8.push_back(GetXmlNodeValue(curNode));
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("DCPType"), false))
				{
					WmsDcpTypeProperty dcpType;
					ParseDcpType(curNode, dcpType);
					operationType.dcpTypes.push_back(dcpType);
				}
			}
		}

		void ParseDcpType(const CPLXMLNode* rootNode, WmsDcpTypeProperty& dcpTypeProperty)
		{
			if (!rootNode)
			{
				return;
			}

			for (CPLXMLNode* curNode = rootNode->psChild; curNode != nullptr; curNode = curNode->psNext)
			{
				if (curNode->eType != CXT_Element)
				{
					continue;
				}

				const std::string nodeName = GetXmlNodeTagName(curNode);
				if (GB_Utf8Equals(nodeName, GB_STR("HTTP"), false))
				{
					ParseHttp(curNode, dcpTypeProperty.http);
				}
			}
		}

		void ParseHttp(const CPLXMLNode* rootNode, WmsHttpProperty& httpProperty)
		{
			if (!rootNode)
			{
				return;
			}

			for (CPLXMLNode* curNode = rootNode->psChild; curNode != nullptr; curNode = curNode->psNext)
			{
				if (curNode->eType != CXT_Element)
				{
					continue;
				}

				std::string nodeName = GetXmlNodeTagName(curNode);
				if (GB_Utf8StartsWith(nodeName, GB_STR("wms:"), false))
				{
					nodeName = GB_Utf8Substr(nodeName, 4);
				}

				if (GB_Utf8Equals(nodeName, GB_STR("Get"), false))
				{
					ParseGet(curNode, httpProperty.get);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("Post"), false))
				{
					ParsePost(curNode, httpProperty.post);
				}
			}
		}

		void ParseGet(const CPLXMLNode* rootNode, WmsGetProperty& getProperty)
		{
			if (!rootNode)
			{
				return;
			}

			for (CPLXMLNode* curNode = rootNode->psChild; curNode != nullptr; curNode = curNode->psNext)
			{
				if (curNode->eType != CXT_Element)
				{
					continue;
				}

				std::string nodeName = GetXmlNodeTagName(curNode);
				if (GB_Utf8StartsWith(nodeName, GB_STR("wms:"), false))
				{
					nodeName = GB_Utf8Substr(nodeName, 4);
				}

				if (GB_Utf8Equals(nodeName, GB_STR("OnlineResource"), false))
				{
					ParseOnlineResource(curNode, getProperty.onlineResource);
				}
			}
		}

		void ParsePost(const CPLXMLNode* rootNode, WmsPostProperty& postProperty)
		{
			if (!rootNode)
			{
				return;
			}
			for (CPLXMLNode* curNode = rootNode->psChild; curNode != nullptr; curNode = curNode->psNext)
			{
				if (curNode->eType != CXT_Element)
				{
					continue;
				}

				std::string nodeName = GetXmlNodeTagName(curNode);
				if (GB_Utf8StartsWith(nodeName, GB_STR("wms:"), false))
				{
					nodeName = GB_Utf8Substr(nodeName, 4);
				}

				if (GB_Utf8Equals(nodeName, GB_STR("OnlineResource"), false))
				{
					ParseOnlineResource(curNode, postProperty.onlineResource);
				}
			}
		}

		void ParseLayer(const CPLXMLNode* rootNode, WmsLayerProperty& layerProperty, WmsLayerProperty* parentProperty = nullptr)
		{
			if (!rootNode)
			{
				return;
			}

			numLayers++;
			layerProperty.orderId = numLayers;

			const std::string queryableAttribute = GetXmlNodeAttribute(rootNode, GB_STR("queryable"));
			layerProperty.queryable = GB_Utf8Equals(queryableAttribute, GB_STR("1"), false) || GB_Utf8Equals(queryableAttribute, GB_STR("true"), false);

			layerProperty.cascaded = static_cast<int>(GB_ToUInt(GetXmlNodeAttribute(rootNode, GB_STR("cascaded")), 0));

			const std::string opaqueAttribute = GetXmlNodeAttribute(rootNode, GB_STR("opaque"));
			layerProperty.opaque = GB_Utf8Equals(opaqueAttribute, GB_STR("1"), false) || GB_Utf8Equals(opaqueAttribute, GB_STR("true"), false);

			const std::string noSubsetsAttribute = GetXmlNodeAttribute(rootNode, GB_STR("noSubsets"));
			layerProperty.noSubsets = GB_Utf8Equals(noSubsetsAttribute, GB_STR("1"), false) || GB_Utf8Equals(noSubsetsAttribute, GB_STR("true"), false);

			layerProperty.fixedWidth = static_cast<int>(GB_ToUInt(GetXmlNodeAttribute(rootNode, GB_STR("fixedWidth")), 0));
			layerProperty.fixedHeight = static_cast<int>(GB_ToUInt(GetXmlNodeAttribute(rootNode, GB_STR("fixedHeight")), 0));

			for (CPLXMLNode* curNode = rootNode->psChild; curNode != nullptr; curNode = curNode->psNext)
			{
				if (curNode->eType != CXT_Element)
				{
					continue;
				}

				std::string nodeName = GetXmlNodeTagName(curNode);
				if (GB_Utf8StartsWith(nodeName, GB_STR("wms:"), false))
				{
					nodeName = GB_Utf8Substr(nodeName, 4);
				}

				if (GB_Utf8Equals(nodeName, GB_STR("Layer"), false))
				{
					std::vector<WmsStyleProperty> inheritedStyles{ layerProperty.styles };

					CPLXMLNode* nameNode = FindChildElement(curNode, GB_STR("Name"));
					if (nameNode)
					{
						const std::string layerName = GetXmlNodeValue(nameNode);
						for (WmsStyleProperty& inheritedStyleProperty : inheritedStyles)
						{
							for (WmsLegendUrlProperty& legendUrlProperty : inheritedStyleProperty.legendUrls)
							{
								std::string legendUrl = legendUrlProperty.onlineResource.xlinkHrefUtf8;
								if (legendUrl.find('?') != std::string::npos) // 已经包含查询参数
								{
									std::string originLayerValue = "";
									if (GB_UrlOperator::TryGetUrlQueryValue(legendUrl, GB_STR("layer"), originLayerValue))
									{
										legendUrl = GB_UrlOperator::SetUrlQueryValue(legendUrl, GB_STR("layer"), layerName);
									}
									legendUrlProperty.onlineResource.xlinkHrefUtf8 = legendUrl;
								}
							}
						}
					}

					WmsLayerProperty subLayerProperty;
					subLayerProperty.styles = inheritedStyles;
					subLayerProperty.crsUtf8 = layerProperty.crsUtf8;
					subLayerProperty.boundingBoxes = layerProperty.boundingBoxes;
					subLayerProperty.exGeographicBBox = layerProperty.exGeographicBBox;
					ParseLayer(curNode, subLayerProperty, &layerProperty);
					layerProperty.subLayers.push_back(std::move(subLayerProperty));
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("Name"), false))
				{
					layerProperty.nameUtf8 = GetXmlNodeValue(curNode);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("Title"), false))
				{
					layerProperty.titleUtf8 = GetXmlNodeValue(curNode);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("Abstract"), false))
				{
					layerProperty.abstractUtf8 = GetXmlNodeValue(curNode);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("KeywordList"), false))
				{
					ParseKeywordList(curNode, layerProperty.keywordsUtf8);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("SRS"), false) || GB_Utf8Equals(nodeName, GB_STR("CRS"), false))
				{
					const std::vector<std::string> crsList = GB_Utf8Split(GetXmlNodeValue(curNode), GB_CHAR(' '));
					for (const std::string& crs : crsList)
					{
						if (std::find(layerProperty.crsUtf8.begin(), layerProperty.crsUtf8.end(), crs) == layerProperty.crsUtf8.end())
						{
							// 只添加不重复的 CRS
							layerProperty.crsUtf8.push_back(crs);
						}
					}
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("LatLonBoundingBox"), false))
				{
					try
					{
						const double minX = GB_ToDouble(GB_Utf8Replace(GetXmlNodeAttribute(curNode, GB_STR("minx")), GB_STR(","), GB_STR(".")));
						const double minY = GB_ToDouble(GB_Utf8Replace(GetXmlNodeAttribute(curNode, GB_STR("miny")), GB_STR(","), GB_STR(".")));
						const double maxX = GB_ToDouble(GB_Utf8Replace(GetXmlNodeAttribute(curNode, GB_STR("maxx")), GB_STR(","), GB_STR(".")));
						const double maxY = GB_ToDouble(GB_Utf8Replace(GetXmlNodeAttribute(curNode, GB_STR("maxy")), GB_STR(","), GB_STR(".")));
						layerProperty.exGeographicBBox.Set(minX, minY, maxX, maxY);

						const std::string srsValue = GetXmlNodeAttribute(curNode, GB_STR("SRS"));
						if (!srsValue.empty() && !GB_Utf8Equals(nodeName, GB_STR("CRS:84"), false) && GeoCrsManager::IsDefinitionValidCached(srsValue))
						{
							// 如果 SRS 属性存在且不是 CRS:84，那么就尝试将 LatLonBoundingBox 转换到 CRS:84 坐标系下
							const GeoBoundingBox originalBBox(GB_ToWkt(srsValue), layerProperty.exGeographicBBox);
							GeoBoundingBox targetBBox;
							if (GeoCrsTransform::TransformBoundingBox(originalBBox, GB_ToWkt("CRS:84"), targetBBox) && targetBBox.IsValid())
							{
								layerProperty.exGeographicBBox = targetBBox.rect;
							}
							else
							{
								GBLOG_WARNING(GB_Utf8Format("Failed to transform LatLonBoundingBox from SRS '%s' to 'CRS:84'. Original SRS: '%s'.", srsValue.c_str(), srsValue.c_str()));
							}
						}
					}
					catch (const std::exception& e)
					{
						GBLOG_WARNING(GB_Utf8Format("Failed to parse LatLonBoundingBox attributes. Error message: %s", e.what()));
					}
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("EX_GeographicBoundingBox"), false))
				{
					const CPLXMLNode* westBoundLongitudeNode = nullptr;
					const CPLXMLNode* eastBoundLongitudeNode = nullptr;
					const CPLXMLNode* southBoundLatitudeNode = nullptr;
					const CPLXMLNode* northBoundLatitudeNode = nullptr;
					{
						const std::string originNodeName = GetXmlNodeTagName(curNode);
						if (GB_Utf8Equals(originNodeName, GB_STR("wms:EX_GeographicBoundingBox"), false))
						{
							westBoundLongitudeNode = FindChildElement(curNode, GB_STR("wms:westBoundLongitude"));
							eastBoundLongitudeNode = FindChildElement(curNode, GB_STR("wms:eastBoundLongitude"));
							southBoundLatitudeNode = FindChildElement(curNode, GB_STR("wms:southBoundLatitude"));
							northBoundLatitudeNode = FindChildElement(curNode, GB_STR("wms:northBoundLatitude"));
						}
						else
						{
							westBoundLongitudeNode = FindChildElement(curNode, GB_STR("westBoundLongitude"));
							eastBoundLongitudeNode = FindChildElement(curNode, GB_STR("eastBoundLongitude"));
							southBoundLatitudeNode = FindChildElement(curNode, GB_STR("southBoundLatitude"));
							northBoundLatitudeNode = FindChildElement(curNode, GB_STR("northBoundLatitude"));
						}
					}

					if (westBoundLongitudeNode && eastBoundLongitudeNode && southBoundLatitudeNode && northBoundLatitudeNode)
					{
						double minX = 0, minY = 0, maxX = 0, maxY = 0;
						bool minXOK = false, minYOK = false, maxXOK = false, maxYOK = false;
						try
						{
							minX = GB_ToDouble(GB_Utf8Replace(GetXmlNodeValue(westBoundLongitudeNode), GB_STR(","), GB_STR(".")), 0, &minXOK);
							minY = GB_ToDouble(GB_Utf8Replace(GetXmlNodeValue(southBoundLatitudeNode), GB_STR(","), GB_STR(".")), 0, &minYOK);
							maxX = GB_ToDouble(GB_Utf8Replace(GetXmlNodeValue(eastBoundLongitudeNode), GB_STR(","), GB_STR(".")), 0, &maxXOK);
							maxY = GB_ToDouble(GB_Utf8Replace(GetXmlNodeValue(northBoundLatitudeNode), GB_STR(","), GB_STR(".")), 0, &maxYOK);
						}
						catch (const std::exception& e)
						{
							GBLOG_WARNING(GB_Utf8Format("Failed to parse EX_GeographicBoundingBox values. Error message: %s", e.what()));
						}

						if (minXOK && minYOK && maxXOK && maxYOK)
						{
							layerProperty.exGeographicBBox.Set(minX, minY, maxX, maxY);
						}
						else
						{
							GBLOG_WARNING("One or more EX_GeographicBoundingBox values are invalid.");
						}
					}
					else
					{
						GBLOG_WARNING("EX_GeographicBoundingBox element is missing one or more required child elements (westBoundLongitude, eastBoundLongitude, southBoundLatitude, northBoundLatitude).");
					}
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("BoundingBox"), false))
				{
					GeoBoundingBox boundingBox;
					try
					{
						boundingBox.rect.minX = GB_ToDouble(GB_Utf8Replace(GetXmlNodeAttribute(curNode, GB_STR("minx")), GB_STR(","), GB_STR(".")));
						boundingBox.rect.minY = GB_ToDouble(GB_Utf8Replace(GetXmlNodeAttribute(curNode, GB_STR("miny")), GB_STR(","), GB_STR(".")));
						boundingBox.rect.maxX = GB_ToDouble(GB_Utf8Replace(GetXmlNodeAttribute(curNode, GB_STR("maxx")), GB_STR(","), GB_STR(".")));
						boundingBox.rect.maxY = GB_ToDouble(GB_Utf8Replace(GetXmlNodeAttribute(curNode, GB_STR("maxy")), GB_STR(","), GB_STR(".")));
					}
					catch (const std::exception& e)
					{
						GBLOG_WARNING(GB_Utf8Format("Failed to parse BoundingBox attributes. Error message: %s", e.what()));
					}

					const std::string crsValue = GetXmlNodeAttribute(curNode, GB_STR("CRS"));
					const std::string srsValue = GetXmlNodeAttribute(curNode, GB_STR("SRS"));
					if (!crsValue.empty() || !srsValue.empty())
					{
						if (!crsValue.empty())
						{
							boundingBox.wktUtf8 = GB_ToWkt(crsValue);
						}
						else
						{
							boundingBox.wktUtf8 = GB_ToWkt(srsValue);
						}

						if (ShouldInvertAxisOrder(boundingBox.wktUtf8))
						{
							boundingBox.rect.Set(boundingBox.rect.minY, boundingBox.rect.minX, boundingBox.rect.maxY, boundingBox.rect.maxX);
						}

						bool inheritedOverwritten = false;
						for (GeoBoundingBox& existingBoundingBox : layerProperty.boundingBoxes)
						{
							if (GB_Utf8Equals(existingBoundingBox.wktUtf8, boundingBox.wktUtf8, false))
							{
								existingBoundingBox.rect = boundingBox.rect;
								inheritedOverwritten = true;
							}
						}
						if (!inheritedOverwritten)
						{
							layerProperty.boundingBoxes.push_back(boundingBox);
						}
					}
					else
					{
						GBLOG_WARNING("BoundingBox element is missing both CRS and SRS attributes, so the bounding box will be ignored.");
					}
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("Dimension"), false))
				{
					WmsDimensionProperty dimensionProperty;
					ParseDimension(curNode, dimensionProperty);
					layerProperty.dimensions.push_back(dimensionProperty);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("Extent"), false))
				{
					ParseExtent(curNode, layerProperty.dimensions);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("Attribution"), false))
				{
					// TODO...
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("AuthorityURL"), false))
				{
					// TODO...
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("Identifier"), false))
				{
					// TODO...
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("MetadataURL"), false))
				{
					WmsMetadataUrlProperty metadataUrlProperty;
					ParseMetadataUrl(curNode, metadataUrlProperty);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("DataURL"), false))
				{
					// TODO...
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("FeatureListURL"), false))
				{
					// TODO...
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("Style"), false))
				{
					WmsStyleProperty styleProperty;
					ParseStyle(curNode, styleProperty);
					for (size_t i = 0; i < layerProperty.styles.size(); i++)
					{
						if (layerProperty.styles[i].nameUtf8 == styleProperty.nameUtf8)
						{
							// 如果当前 Layer 已经存在同名的 Style，那么就用新的 Style 替换旧的 Style
							layerProperty.styles.erase(layerProperty.styles.begin() + i);
							break;
						}
					}
					layerProperty.styles.push_back(styleProperty);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("MinScaleDenominator"), false))
				{
					// TODO...
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("MaxScaleDenominator"), false))
				{
					// TODO...
				}
			}

			if (parentProperty)
			{
				layerParentIdMap[layerProperty.orderId] = parentProperty->orderId;
			}

			if (!layerProperty.nameUtf8.empty())
			{
				queryableLayerIdCache[layerProperty.nameUtf8] = layerProperty.queryable;
				layersSupported.push_back(layerProperty);
				if (layerProperty.subLayers.empty())
				{
					layerProperty.styles.clear();
				}
			}

			if (!layerProperty.subLayers.empty())
			{
				layerParentNamesMap[layerProperty.orderId].clear();
				layerParentNamesMap[layerProperty.orderId].push_back(layerProperty.nameUtf8);
				layerParentNamesMap[layerProperty.orderId].push_back(layerProperty.titleUtf8);
				layerParentNamesMap[layerProperty.orderId].push_back(layerProperty.abstractUtf8);
			}
		}

		void ParseDimension(const CPLXMLNode* rootNode, WmsDimensionProperty& dimensionProperty)
		{
			if (!rootNode)
			{
				return;
			}

			dimensionProperty.nameUtf8 = GetXmlNodeAttribute(rootNode, GB_STR("name"));
			dimensionProperty.unitsUtf8 = GetXmlNodeAttribute(rootNode, GB_STR("units"));
			dimensionProperty.unitSymbolUtf8 = GetXmlNodeAttribute(rootNode, GB_STR("unitSymbol"));
			dimensionProperty.defaultValueUtf8 = GetXmlNodeAttribute(rootNode, GB_STR("default"));

			const std::string multipleValuesAttribute = GetXmlNodeAttribute(rootNode, GB_STR("multipleValues"));
			if (!multipleValuesAttribute.empty())
			{
				dimensionProperty.multipleValues = GB_Utf8Equals(multipleValuesAttribute, GB_STR("1"), false) || GB_Utf8Equals(multipleValuesAttribute, GB_STR("true"), false);
			}

			const std::string nearestValueAttribute = GetXmlNodeAttribute(rootNode, GB_STR("nearestValue"));
			if (!nearestValueAttribute.empty())
			{
				dimensionProperty.nearestValue = GB_Utf8Equals(nearestValueAttribute, GB_STR("1"), false) || GB_Utf8Equals(nearestValueAttribute, GB_STR("true"), false);
			}

			const std::string currentAttribute = GetXmlNodeAttribute(rootNode, GB_STR("current"));
			if (!currentAttribute.empty())
			{
				dimensionProperty.current = GB_Utf8Equals(currentAttribute, GB_STR("1"), false) || GB_Utf8Equals(currentAttribute, GB_STR("true"), false);
			}

			dimensionProperty.extentUtf8 = GB_Utf8Simplified(GetXmlNodeValue(rootNode));
		}

		void ParseExtent(const CPLXMLNode* rootNode, std::vector<WmsDimensionProperty>& dimensionProperties)
		{
			if (!rootNode)
			{
				return;
			}

			const std::string name = GetXmlNodeAttribute(rootNode, GB_STR("name"));
			for (WmsDimensionProperty& dimensionProperty : dimensionProperties)
			{
				if (dimensionProperty.nameUtf8 != name)
				{
					continue;
				}

				dimensionProperty.extentUtf8 = GB_Utf8Simplified(GetXmlNodeValue(rootNode));
				dimensionProperty.defaultValueUtf8 = GetXmlNodeAttribute(rootNode, GB_STR("default"));

				const std::string multipleValuesAttribute = GetXmlNodeAttribute(rootNode, GB_STR("multipleValues"));
				if (!multipleValuesAttribute.empty())
				{
					dimensionProperty.multipleValues = GB_Utf8Equals(multipleValuesAttribute, GB_STR("1"), false) || GB_Utf8Equals(multipleValuesAttribute, GB_STR("true"), false);
				}

				const std::string nearestValueAttribute = GetXmlNodeAttribute(rootNode, GB_STR("nearestValue"));
				if (!nearestValueAttribute.empty())
				{
					dimensionProperty.nearestValue = GB_Utf8Equals(nearestValueAttribute, GB_STR("1"), false) || GB_Utf8Equals(nearestValueAttribute, GB_STR("true"), false);
				}

				const std::string currentAttribute = GetXmlNodeAttribute(rootNode, GB_STR("current"));
				if (!currentAttribute.empty())
				{
					dimensionProperty.current = GB_Utf8Equals(currentAttribute, GB_STR("1"), false) || GB_Utf8Equals(currentAttribute, GB_STR("true"), false);
				}
			}
		}

		void ParseMetadataUrl(const CPLXMLNode* rootNode, WmsMetadataUrlProperty& metadataUrlProperty)
		{
			if (!rootNode)
			{
				return;
			}

			for (CPLXMLNode* curNode = rootNode->psChild; curNode != nullptr; curNode = curNode->psNext)
			{
				if (curNode->eType != CXT_Element)
				{
					continue;
				}

				std::string nodeName = GetXmlNodeTagName(curNode);
				if (GB_Utf8StartsWith(nodeName, GB_STR("wms:"), false))
				{
					nodeName = GB_Utf8Substr(nodeName, 4);
				}

				if (GB_Utf8Equals(nodeName, GB_STR("Format"), false))
				{
					metadataUrlProperty.formatUtf8 = GetXmlNodeValue(curNode);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("OnlineResource"), false))
				{
					ParseOnlineResource(curNode, metadataUrlProperty.onlineResource);
				}
			}
		}

		void ParseStyle(const CPLXMLNode* rootNode, WmsStyleProperty& styleProperty)
		{
			if (!rootNode)
			{
				return;
			}

			for (CPLXMLNode* curNode = rootNode->psChild; curNode != nullptr; curNode = curNode->psNext)
			{
				if (curNode->eType != CXT_Element)
				{
					continue;
				}

				std::string nodeName = GetXmlNodeTagName(curNode);
				if (GB_Utf8StartsWith(nodeName, GB_STR("wms:"), false))
				{
					nodeName = GB_Utf8Substr(nodeName, 4);
				}

				if (GB_Utf8Equals(nodeName, GB_STR("Name"), false))
				{
					styleProperty.nameUtf8 = GetXmlNodeValue(curNode);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("Title"), false))
				{
					styleProperty.titleUtf8 = GetXmlNodeValue(curNode);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("Abstract"), false))
				{
					styleProperty.abstractUtf8 = GetXmlNodeValue(curNode);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("LegendURL"), false))
				{
					WmsLegendUrlProperty legendUrlProperty;
					ParseLegendUrl(curNode, legendUrlProperty);
					styleProperty.legendUrls.push_back(legendUrlProperty);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("StyleSheetURL"), false))
				{
					// TODO...
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("StyleURL"), false))
				{
					// TODO...
				}
			}
		}

		void ParseLegendUrl(const CPLXMLNode* rootNode, WmsLegendUrlProperty& legendUrlProperty)
		{
			if (!rootNode)
			{
				return;
			}

			legendUrlProperty.width = GB_ToULongLong(GetXmlNodeAttribute(rootNode, GB_STR("width")));
			legendUrlProperty.height = GB_ToULongLong(GetXmlNodeAttribute(rootNode, GB_STR("height")));

			for (CPLXMLNode* curNode = rootNode->psChild; curNode != nullptr; curNode = curNode->psNext)
			{
				if (curNode->eType != CXT_Element)
				{
					continue;
				}

				std::string nodeName = GetXmlNodeTagName(curNode);
				if (GB_Utf8StartsWith(nodeName, GB_STR("wms:"), false))
				{
					nodeName = GB_Utf8Substr(nodeName, 4);
				}

				if (GB_Utf8Equals(nodeName, GB_STR("Format"), false))
				{
					legendUrlProperty.formatUtf8 = GetXmlNodeValue(curNode);
				}
				else if (GB_Utf8Equals(nodeName, GB_STR("OnlineResource"), false))
				{
					ParseOnlineResource(curNode, legendUrlProperty.onlineResource);
				}
			}
		}

		void ParseTileSetProfile(const CPLXMLNode* rootNode)
		{
			std::vector<std::string> resolutions, layers, styles;
			GeoBoundingBox boundingBox;
			WmtsTileMatrixSet matrixSet;
			WmtsTileMatrix tileMatrix;
			std::unordered_set<std::string> uniqueFormats;
			std::unordered_set<std::string> uniqueStyles;

			WmtsTileLayer tileLayer;
			tileLayer.dpi = -1;
			tileLayer.timeFormat = WmtsTileLayer::WmtsTimeFormat::YYYYMMDD;
			tileLayer.tileMode = MapTileMode::WMSC;

			if (rootNode)
			{
				for (CPLXMLNode* curNode = rootNode->psChild; curNode != nullptr; curNode = curNode->psNext)
				{
					if (curNode->eType != CXT_Element)
					{
						continue;
					}

					std::string nodeName = GetXmlNodeTagName(curNode);
					if (GB_Utf8StartsWith(nodeName, GB_STR("wms:"), false))
					{
						nodeName = GB_Utf8Substr(nodeName, 4);
					}

					if (GB_Utf8Equals(nodeName, GB_STR("Layers"), false))
					{
						layers.push_back(GetXmlNodeValue(curNode));
					}
					else if (GB_Utf8Equals(nodeName, GB_STR("Styles"), false))
					{
						const std::string styleValue = GetXmlNodeValue(curNode);
						if (uniqueStyles.find(styleValue) == uniqueStyles.end())
						{
							styles.push_back(styleValue);
							uniqueStyles.insert(styleValue);
						}
					}
					else if (GB_Utf8Equals(nodeName, GB_STR("Width"), false))
					{
						tileMatrix.tileWidth = static_cast<int>(GB_ToUInt(GetXmlNodeValue(curNode)));
					}
					else if (GB_Utf8Equals(nodeName, GB_STR("Height"), false))
					{
						tileMatrix.tileHeight = static_cast<int>(GB_ToUInt(GetXmlNodeValue(curNode)));
					}
					else if (GB_Utf8Equals(nodeName, GB_STR("SRS"), false))
					{
						matrixSet.crsUtf8 = GetXmlNodeValue(curNode);
					}
					else if (GB_Utf8Equals(nodeName, GB_STR("Format"), false))
					{
						const std::string formatValue = GetXmlNodeValue(curNode);
						if (uniqueFormats.find(formatValue) == uniqueFormats.end())
						{
							tileLayer.formats.push_back(formatValue);
							uniqueFormats.insert(formatValue);
						}
					}
					else if (GB_Utf8Equals(nodeName, GB_STR("BoundingBox"), false))
					{
						GeoBoundingBox boundingBox;
						try
						{
							boundingBox.rect.minX = GB_ToDouble(GB_Utf8Replace(GetXmlNodeAttribute(curNode, GB_STR("minx")), GB_STR(","), GB_STR(".")));
							boundingBox.rect.minY = GB_ToDouble(GB_Utf8Replace(GetXmlNodeAttribute(curNode, GB_STR("miny")), GB_STR(","), GB_STR(".")));
							boundingBox.rect.maxX = GB_ToDouble(GB_Utf8Replace(GetXmlNodeAttribute(curNode, GB_STR("maxx")), GB_STR(","), GB_STR(".")));
							boundingBox.rect.maxY = GB_ToDouble(GB_Utf8Replace(GetXmlNodeAttribute(curNode, GB_STR("maxy")), GB_STR(","), GB_STR(".")));
						}
						catch (const std::exception& e)
						{
							GBLOG_WARNING(GB_Utf8Format("Failed to parse BoundingBox attributes in TileSetProfile. Error message: %s", e.what()));
						}

						std::string srsValue = GetXmlNodeAttribute(curNode, GB_STR("SRS"));
						if (srsValue.empty())
						{
							srsValue = GetXmlNodeAttribute(curNode, GB_STR("srs"));
						}

						std::string crsValue = GetXmlNodeAttribute(curNode, GB_STR("CRS"));
						if (crsValue.empty())
						{
							crsValue = GetXmlNodeAttribute(curNode, GB_STR("crs"));
						}

						if (!srsValue.empty() && GeoCrsManager::IsDefinitionValidCached(srsValue))
						{
							boundingBox.wktUtf8 = GB_ToWkt(srsValue);
						}
						else if (!crsValue.empty() && GeoCrsManager::IsDefinitionValidCached(crsValue))
						{
							boundingBox.wktUtf8 = GB_ToWkt(crsValue);
						}
						else
						{
							GBLOG_WARNING("BoundingBox element in TileSetProfile is missing both CRS and SRS attributes, so the bounding box will be ignored.");
						}

						if (!boundingBox.wktUtf8.empty())
						{
							tileLayer.boundingBoxes.push_back(boundingBox);
						}
					}
					else if (GB_Utf8Equals(nodeName, GB_STR("Resolutions"), false))
					{
						resolutions = GB_Utf8Split(GB_Utf8Trim(GetXmlNodeValue(curNode)), GB_CHAR(' '));
					}
				}
			}

			{
				std::string layersNamesCombined = "";
				for (const std::string& layerName : layers)
				{
					layersNamesCombined += layerName;
					layersNamesCombined += GB_CHAR('_');
				}
				if (!layersNamesCombined.empty())
				{
					layersNamesCombined.pop_back();
				}

				matrixSet.identifierUtf8 = layersNamesCombined + GB_STR("-wmsc-") + std::to_string(tileLayersSupported.size());
			}

			{
				std::string layersNamesCombined = "";
				for (const std::string& layerName : layers)
				{
					layersNamesCombined += layerName;
					layersNamesCombined += GB_CHAR(',');
				}
				if (!layersNamesCombined.empty())
				{
					layersNamesCombined.pop_back();
				}
				tileLayer.identifierUtf8 = layersNamesCombined;
			}

			WmtsStyle style;
			{
				std::string styleCombined = "";
				for (const std::string& styleName : styles)
				{
					styleCombined += styleName;
					styleCombined += GB_CHAR(',');
				}
				if (!styleCombined.empty())
				{
					styleCombined.pop_back();
				}
				style.identifierUtf8 = styleCombined;
			}
			tileLayer.styles[style.identifierUtf8] = style;
			tileLayer.defaultStyleUtf8 = style.identifierUtf8;

			WmtsTileMatrixSetLink setLink;
			setLink.tileMatrixSetIdentifierUtf8 = matrixSet.identifierUtf8;
			tileLayer.setLinks[setLink.tileMatrixSetIdentifierUtf8] = setLink;
			tileLayersSupported.push_back(tileLayer);

			for (size_t i = 0; i < resolutions.size(); i++)
			{
				const std::string& resolution = resolutions[i];
				const double resolutionValue = GB_ToDouble(GB_Utf8Replace(resolution, GB_STR(","), GB_STR(".")));
				if (resolutionValue <= 0)
				{
					GBLOG_WARNING(GB_Utf8Format("Invalid resolution '%s' in TileSetProfile. Resolutions must be positive numbers.", resolution.c_str()));
					continue;
				}

				tileMatrix.identifierUtf8 = std::to_string(resolutionValue);
				if (tileLayer.boundingBoxes.size() != 1)
				{
					GBLOG_WARNING("TileSetProfile contains multiple bounding boxes, which is not supported by the current implementation. Only the first bounding box will be used.");
				}
				if (tileLayer.boundingBoxes.empty() || tileMatrix.tileWidth <= 0 || tileMatrix.tileHeight <= 0)
				{
					continue;
				}
				tileMatrix.matrixWidth = static_cast<int>(std::ceil(tileLayer.boundingBoxes[0].rect.Width() / tileMatrix.tileWidth / resolutionValue));
				tileMatrix.matrixHeight = static_cast<int>(std::ceil(tileLayer.boundingBoxes[0].rect.Height() / tileMatrix.tileHeight / resolutionValue));
				tileMatrix.topLeft.Set(tileLayer.boundingBoxes[0].rect.minX, tileLayer.boundingBoxes[0].rect.minY + tileMatrix.matrixHeight * tileMatrix.tileHeight * resolutionValue);
				tileMatrix.tres = resolutionValue;
				matrixSet.tileMatrices[resolutionValue] = tileMatrix;
			}
			tileMatrixSets[matrixSet.identifierUtf8] = matrixSet;
		}

		void ParseWMTSContents(const CPLXMLNode* rootNode)
		{
			if (!rootNode)
			{
				return;
			}

			tileMatrixSets.clear();

			for (CPLXMLNode* tileMatrixSetNode = rootNode->psChild; tileMatrixSetNode != nullptr; tileMatrixSetNode = tileMatrixSetNode->psNext)
			{
				if (tileMatrixSetNode->eType != CXT_Element)
				{
					continue;
				}

				const std::string nodeName = GetXmlNodeTagName(tileMatrixSetNode);
				if (!GB_Utf8Equals(nodeName, GB_STR("TileMatrixSet"), false))
				{
					continue;
				}

				WmtsTileMatrixSet set;
				set.identifierUtf8 = GetXmlNodeValue(FindChildElement(tileMatrixSetNode, GB_STR("ows:Identifier")));
				set.titleUtf8 = GetXmlNodeValue(FindChildElement(tileMatrixSetNode, GB_STR("ows:Title")));
				set.abstractUtf8 = GetXmlNodeValue(FindChildElement(tileMatrixSetNode, GB_STR("ows:Abstract")));
				ParseKeywords(tileMatrixSetNode, set.keywordsUtf8);
				set.wkScaleSetUtf8 = GetXmlNodeValue(FindChildElement(tileMatrixSetNode, GB_STR("WellKnownScaleSet")));

				const std::string supportedCrs = GetXmlNodeValue(FindChildElement(tileMatrixSetNode, GB_STR("ows:SupportedCRS")));
				if (supportedCrs.empty())
				{
					GBLOG_WARNING(GB_Utf8Format("No SupportedCRS found in TileMatrixSet '%s'. This TileMatrixSet will be ignored.", set.identifierUtf8.c_str()));
					continue;
				}

				const std::shared_ptr<const GeoCrs> crs = GeoCrsManager::GetFromDefinitionCached(supportedCrs);
				if (!crs)
				{
					GBLOG_WARNING(GB_Utf8Format("Unsupported CRS '%s' found in TileMatrixSet '%s'. This TileMatrixSet will be ignored.", supportedCrs.c_str(), set.identifierUtf8.c_str()));
					continue;
				}

				double metersPerUnit = crs->GetMetersPerUnit();
				if (metersPerUnit > 0)
				{
					set.crsUtf8 = crs->ToEpsgStringUtf8();
					if (arcGISMapServiceInfo && crs->IsGeographic() && IsVersionAtMost10_3(arcGISMapServiceInfo->m_currentVersion) &&
						GB_Utf8StartsWith(std::to_string(metersPerUnit), "111319.49"))
					{
						metersPerUnit = oldArcGISServerMetersPerUnit;
					}
				}
				else
				{
					GBLOG_WARNING(GB_Utf8Format("CRS '%s' in TileMatrixSet '%s' has non-positive meters per unit, which is not supported. This TileMatrixSet will be ignored.", supportedCrs.c_str(), set.identifierUtf8.c_str()));
				}

				if (set.crsUtf8.empty())
				{
					set.crsUtf8 = GB_ToWkt(supportedCrs);
				}

				bool invert = !parserOptions.ignoreAxisOrientation && GeoCrsManager::IsDefinitionAxisOrderReversedCached(set.crsUtf8);
				if (parserOptions.invertAxisOrientation)
				{
					invert = !invert;
				}

				for (CPLXMLNode* tileMatrixNode = tileMatrixSetNode->psChild; tileMatrixNode != nullptr; tileMatrixNode = tileMatrixNode->psNext)
				{
					if (tileMatrixNode->eType != CXT_Element)
					{
						continue;
					}

					const std::string nodeName = GetXmlNodeTagName(tileMatrixNode);
					if (!GB_Utf8Equals(nodeName, GB_STR("TileMatrix"), false))
					{
						continue;
					}

					WmtsTileMatrix tileMatrix;
					tileMatrix.identifierUtf8 = GetXmlNodeValue(FindChildElement(tileMatrixNode, GB_STR("ows:Identifier")));
					tileMatrix.titleUtf8 = GetXmlNodeValue(FindChildElement(tileMatrixNode, GB_STR("ows:Title")));
					tileMatrix.abstractUtf8 = GetXmlNodeValue(FindChildElement(tileMatrixNode, GB_STR("ows:Abstract")));
					ParseKeywords(tileMatrixNode, tileMatrix.keywordsUtf8);
					tileMatrix.scaleDenominator = GB_ToDouble(GetXmlNodeValue(FindChildElement(tileMatrixNode, GB_STR("ScaleDenominator"))));

					const std::string topLeftCornerValue = GetXmlNodeValue(FindChildElement(tileMatrixNode, GB_STR("TopLeftCorner")));
					const std::vector<std::string> topLeftCornerParts = GB_Utf8Split(GB_Utf8Trim(topLeftCornerValue), GB_CHAR(' '));
					if (topLeftCornerParts.size() != 2)
					{
						GBLOG_WARNING(GB_Utf8Format("Invalid TopLeftCorner value '%s' in TileMatrix '%s' of TileMatrixSet '%s'. Expected format is 'x y'.", topLeftCornerValue.c_str(), tileMatrix.identifierUtf8.c_str(), set.identifierUtf8.c_str()));
						continue;
					}
					if (invert)
					{
						tileMatrix.topLeft.Set(GB_ToDouble(GB_Utf8Replace(topLeftCornerParts[1], GB_STR(","), GB_STR("."))), GB_ToDouble(GB_Utf8Replace(topLeftCornerParts[0], GB_STR(","), GB_STR("."))));
					}
					else
					{
						tileMatrix.topLeft.Set(GB_ToDouble(GB_Utf8Replace(topLeftCornerParts[0], GB_STR(","), GB_STR("."))), GB_ToDouble(GB_Utf8Replace(topLeftCornerParts[1], GB_STR(","), GB_STR("."))));
					}

					tileMatrix.tileWidth = static_cast<int>(GB_ToUInt(GetXmlNodeValue(FindChildElement(tileMatrixNode, GB_STR("TileWidth")))));
					tileMatrix.tileHeight = static_cast<int>(GB_ToUInt(GetXmlNodeValue(FindChildElement(tileMatrixNode, GB_STR("TileHeight")))));
					tileMatrix.matrixWidth = static_cast<int>(GB_ToUInt(GetXmlNodeValue(FindChildElement(tileMatrixNode, GB_STR("MatrixWidth")))));
					tileMatrix.matrixHeight = static_cast<int>(GB_ToUInt(GetXmlNodeValue(FindChildElement(tileMatrixNode, GB_STR("MatrixHeight")))));

					const double pixelSize = (IsTianditu() ? tiandituRenderingPixelSize : standardRenderingPixelSize);
					tileMatrix.tres = tileMatrix.scaleDenominator * pixelSize / metersPerUnit;
					set.tileMatrices[tileMatrix.tres] = tileMatrix;
				}

				tileMatrixSets[set.identifierUtf8] = set;
				if (firstTileMatrixSetId.empty())
				{
					firstTileMatrixSetId = set.identifierUtf8;
				}
			}

			tileLayersSupported.clear();
			for (CPLXMLNode* layerNode = rootNode->psChild; layerNode != nullptr; layerNode = layerNode->psNext)
			{
				if (layerNode->eType != CXT_Element)
				{
					continue;
				}

				const std::string nodeName = GetXmlNodeTagName(layerNode);
				if (!GB_Utf8Equals(nodeName, GB_STR("Layer"), false))
				{
					continue;
				}

				WmtsTileLayer tileLayer;
				tileLayer.tileMode = MapTileMode::WMTS;
				tileLayer.identifierUtf8 = GetXmlNodeValue(FindChildElement(layerNode, GB_STR("ows:Identifier")));
				tileLayer.titleUtf8 = GetXmlNodeValue(FindChildElement(layerNode, GB_STR("ows:Title")));
				tileLayer.abstractUtf8 = GetXmlNodeValue(FindChildElement(layerNode, GB_STR("ows:Abstract")));
				ParseKeywords(layerNode, tileLayer.keywordsUtf8);

				GeoBoundingBox boundingBox;
				const CPLXMLNode* wgs84BoundingBoxNode = FindChildElement(layerNode, GB_STR("ows:WGS84BoundingBox"));
				if (wgs84BoundingBoxNode)
				{
					const std::vector<std::string> lowerCornerValue = GB_Utf8Split(GetXmlNodeValue(FindChildElement(wgs84BoundingBoxNode, GB_STR("ows:LowerCorner"))), GB_CHAR(' '));
					const std::vector<std::string> upperCornerValue = GB_Utf8Split(GetXmlNodeValue(FindChildElement(wgs84BoundingBoxNode, GB_STR("ows:UpperCorner"))), GB_CHAR(' '));
					if (lowerCornerValue.size() == 2 && upperCornerValue.size() == 2)
					{
						boundingBox.wktUtf8 = GB_ToWkt("CRS:84");
						boundingBox.rect.Set(GB_ToDouble(GB_Utf8Replace(lowerCornerValue[0], GB_STR(","), GB_STR("."))),
							GB_ToDouble(GB_Utf8Replace(lowerCornerValue[1], GB_STR(","), GB_STR("."))),
							GB_ToDouble(GB_Utf8Replace(upperCornerValue[0], GB_STR(","), GB_STR("."))),
							GB_ToDouble(GB_Utf8Replace(upperCornerValue[1], GB_STR(","), GB_STR("."))));
						tileLayer.boundingBoxes.push_back(boundingBox);
					}
				}

				for (CPLXMLNode* boundingBoxNode = layerNode->psChild; boundingBoxNode != nullptr; boundingBoxNode = boundingBoxNode->psNext)
				{
					if (boundingBoxNode->eType != CXT_Element)
					{
						continue;
					}

					const std::string nodeName = GetXmlNodeTagName(boundingBoxNode);
					if (!GB_Utf8Equals(nodeName, GB_STR("ows:BoundingBox"), false))
					{
						continue;
					}

					boundingBox.Reset();
					const std::vector<std::string> lowerCornerValue = GB_Utf8Split(GetXmlNodeValue(FindChildElement(boundingBoxNode, GB_STR("ows:LowerCorner"))), GB_CHAR(' '));
					const std::vector<std::string> upperCornerValue = GB_Utf8Split(GetXmlNodeValue(FindChildElement(boundingBoxNode, GB_STR("ows:UpperCorner"))), GB_CHAR(' '));
					if (lowerCornerValue.size() != 2 || upperCornerValue.size() != 2)
					{
						GBLOG_WARNING(GB_Utf8Format("Invalid LowerCorner or UpperCorner value in BoundingBox of Layer '%s'. Expected format is 'x y'. This bounding box will be ignored.", tileLayer.identifierUtf8.c_str()));
						continue;
					}

					boundingBox.rect.Set(GB_ToDouble(GB_Utf8Replace(lowerCornerValue[0], GB_STR(","), GB_STR("."))),
						GB_ToDouble(GB_Utf8Replace(lowerCornerValue[1], GB_STR(","), GB_STR("."))),
						GB_ToDouble(GB_Utf8Replace(upperCornerValue[0], GB_STR(","), GB_STR("."))),
						GB_ToDouble(GB_Utf8Replace(upperCornerValue[1], GB_STR(","), GB_STR("."))));

					std::string srsValue = GetXmlNodeAttribute(boundingBoxNode, GB_STR("SRS"));
					if (srsValue.empty())
					{
						srsValue = GetXmlNodeAttribute(boundingBoxNode, GB_STR("srs"));
					}

					std::string crsValue = GetXmlNodeAttribute(boundingBoxNode, GB_STR("CRS"));
					if (crsValue.empty())
					{
						crsValue = GetXmlNodeAttribute(boundingBoxNode, GB_STR("crs"));
					}

					if (!srsValue.empty() && GeoCrsManager::IsDefinitionValidCached(srsValue))
					{
						boundingBox.wktUtf8 = GB_ToWkt(srsValue);
					}
					else if (!crsValue.empty() && GeoCrsManager::IsDefinitionValidCached(crsValue))
					{
						boundingBox.wktUtf8 = GB_ToWkt(crsValue);
					}
					if (boundingBox.wktUtf8.empty())
					{
						GBLOG_WARNING(GB_Utf8Format("BoundingBox element in Layer '%s' is missing both CRS and SRS attributes, so the bounding box will be ignored.", tileLayer.identifierUtf8.c_str()));
						continue;
					}

					bool invert = !parserOptions.ignoreAxisOrientation && GeoCrsManager::IsDefinitionAxisOrderReversedCached(boundingBox.wktUtf8);
					if (parserOptions.invertAxisOrientation)
					{
						invert = !invert;
					}
					if (invert)
					{
						boundingBox.rect.Set(boundingBox.rect.minY, boundingBox.rect.minX, boundingBox.rect.maxY, boundingBox.rect.maxX);
					}
					tileLayer.boundingBoxes.push_back(boundingBox);
				}

				for (CPLXMLNode* styleNode = layerNode->psChild; styleNode != nullptr; styleNode = styleNode->psNext)
				{
					if (styleNode->eType != CXT_Element)
					{
						continue;
					}

					const std::string nodeName = GetXmlNodeTagName(styleNode);
					if (!GB_Utf8Equals(nodeName, GB_STR("Style"), false))
					{
						continue;
					}

					WmtsStyle style;
					style.identifierUtf8 = GetXmlNodeValue(FindChildElement(styleNode, GB_STR("ows:Identifier")));
					style.titleUtf8 = GetXmlNodeValue(FindChildElement(styleNode, GB_STR("ows:Title")));
					style.abstractUtf8 = GetXmlNodeValue(FindChildElement(styleNode, GB_STR("ows:Abstract")));
					ParseKeywords(styleNode, style.keywordsUtf8);

					for (CPLXMLNode* legendUrlNode = styleNode->psChild; legendUrlNode != nullptr; legendUrlNode = legendUrlNode->psNext)
					{
						if (legendUrlNode->eType != CXT_Element)
						{
							continue;
						}
						const std::string nodeName = GetXmlNodeTagName(legendUrlNode);
						if (!GB_Utf8Equals(nodeName, GB_STR("ows:legendURL"), false))
						{
							continue;
						}

						WmtsLegendUrl legendUrl;
						legendUrl.formatUtf8 = GetXmlNodeValue(FindChildElement(legendUrlNode, GB_STR("format")));
						legendUrl.minScale = GB_ToDouble(GetXmlNodeValue(FindChildElement(legendUrlNode, GB_STR("minScale"))));
						legendUrl.maxScale = GB_ToDouble(GetXmlNodeValue(FindChildElement(legendUrlNode, GB_STR("maxScale"))));
						legendUrl.hrefUtf8 = GetXmlNodeValue(FindChildElement(legendUrlNode, GB_STR("href")));
						legendUrl.width = static_cast<int>(GB_ToUInt(GetXmlNodeValue(FindChildElement(legendUrlNode, GB_STR("width")))));
						legendUrl.height = static_cast<int>(GB_ToUInt(GetXmlNodeValue(FindChildElement(legendUrlNode, GB_STR("height")))));
						style.legendUrls.push_back(legendUrl);
					}

					CPLXMLNode* legendUrlNode = FindChildElement(styleNode, GB_STR("LegendURL"));
					if (legendUrlNode)
					{
						WmtsLegendUrl legendUrl;
						legendUrl.formatUtf8 = GetXmlNodeAttribute(legendUrlNode, GB_STR("format"));
						legendUrl.minScale = GB_ToDouble(GetXmlNodeAttribute(legendUrlNode, GB_STR("minScaleDenominator")));
						legendUrl.maxScale = GB_ToDouble(GetXmlNodeAttribute(legendUrlNode, GB_STR("maxScaleDenominator")));
						legendUrl.hrefUtf8 = GetXmlNodeAttribute(legendUrlNode, GB_STR("xlink:href"));
						legendUrl.width = static_cast<int>(GB_ToUInt(GetXmlNodeAttribute(legendUrlNode, GB_STR("width"))));
						legendUrl.height = static_cast<int>(GB_ToUInt(GetXmlNodeAttribute(legendUrlNode, GB_STR("height"))));
						style.legendUrls.push_back(legendUrl);
					}

					style.isDefault = GB_Utf8Equals(GetXmlNodeAttribute(styleNode, GB_STR("isDefault")), GB_STR("true"), false);

					tileLayer.styles[style.identifierUtf8] = style;
					if (style.isDefault)
					{
						tileLayer.defaultStyleUtf8 = style.identifierUtf8;
					}
				}

				if (tileLayer.styles.empty())
				{
					GBLOG_WARNING(GB_Utf8Format("No styles found for Layer '%s' in WMTS capabilities. A default style has been generated, but the layer may not render correctly.", tileLayer.identifierUtf8.c_str()));

					WmtsStyle defaultStyle;
					defaultStyle.identifierUtf8 = GB_STR("default");
					defaultStyle.titleUtf8 = GB_STR("Generated default style");
					defaultStyle.abstractUtf8 = GB_STR("This style was automatically generated because no styles were defined for this layer in the WMTS capabilities.");
					tileLayer.styles[defaultStyle.identifierUtf8] = defaultStyle;
				}

				{
					std::unordered_set<std::string> uniqueFormats;
					for (CPLXMLNode* formatNode = layerNode->psChild; formatNode != nullptr; formatNode = formatNode->psNext)
					{
						if (formatNode->eType != CXT_Element)
						{
							continue;
						}

						const std::string nodeName = GetXmlNodeTagName(formatNode);
						if (!GB_Utf8Equals(nodeName, GB_STR("Format"), false))
						{
							continue;
						}

						const std::string formatValue = GetXmlNodeValue(formatNode);
						if (!formatValue.empty() && uniqueFormats.find(formatValue) == uniqueFormats.end())
						{
							tileLayer.formats.push_back(formatValue);
							uniqueFormats.insert(formatValue);
						}
					}
				}

				for (CPLXMLNode* infoFormatNode = layerNode->psChild; infoFormatNode != nullptr; infoFormatNode = infoFormatNode->psNext)
				{
					if (infoFormatNode->eType != CXT_Element)
					{
						continue;
					}

					const std::string nodeName = GetXmlNodeTagName(infoFormatNode);
					if (!GB_Utf8Equals(nodeName, GB_STR("InfoFormat"), false))
					{
						continue;
					}

					const std::string infoFormatValue = GetXmlNodeValue(infoFormatNode);
					if (!infoFormatValue.empty())
					{
						tileLayer.infoFormats.push_back(infoFormatValue);
					}

					RasterIdentifyFormat identifyFormat = RasterIdentifyFormat::Unknown;
					if (GB_Utf8Equals(infoFormatValue, GB_STR("MIME"), false) || GB_Utf8Equals(infoFormatValue, GB_STR("text/plain"), false))
					{
						identifyFormat = RasterIdentifyFormat::Text;
					}
					else if (GB_Utf8Equals(infoFormatValue, GB_STR("text/html"), false))
					{
						identifyFormat = RasterIdentifyFormat::Html;
					}
					else if (GB_Utf8StartsWith(infoFormatValue, GB_STR("GML."), false) || GB_Utf8Equals(infoFormatValue, GB_STR("application/vnd.ogc.gml"), false) ||
						GB_Utf8Find(infoFormatValue, GB_STR("gml"), false) >= 0 || GB_Utf8Equals(infoFormatValue, GB_STR("application/json"), false) ||
						GB_Utf8Equals(infoFormatValue, GB_STR("application/geojson"), false) || GB_Utf8Equals(infoFormatValue, GB_STR("application/geo+json"), false))
					{
						identifyFormat = RasterIdentifyFormat::Feature;
					}
					else
					{
						continue;
					}
					identifyFormats[identifyFormat] = infoFormatValue;
				}

				for (CPLXMLNode* dimensionNode = layerNode->psChild; dimensionNode != nullptr; dimensionNode = dimensionNode->psNext)
				{
					if (dimensionNode->eType != CXT_Element)
					{
						continue;
					}

					const std::string nodeName = GetXmlNodeTagName(dimensionNode);
					if (!GB_Utf8Equals(nodeName, GB_STR("Dimension"), false))
					{
						continue;
					}

					WmtsDimension dimension;
					dimension.identifierUtf8 = GetXmlNodeValue(FindChildElement(dimensionNode, GB_STR("ows:Identifier")));
					if (dimension.identifierUtf8.empty())
					{
						continue;
					}

					dimension.titleUtf8 = GetXmlNodeValue(FindChildElement(dimensionNode, GB_STR("ows:Title")));
					dimension.abstractUtf8 = GetXmlNodeValue(FindChildElement(dimensionNode, GB_STR("ows:Abstract")));
					ParseKeywords(dimensionNode, dimension.keywordsUtf8);
					dimension.unitOfMeasureUtf8 = GetXmlNodeValue(FindChildElement(dimensionNode, GB_STR("UOM")));
					dimension.unitSymbolUtf8 = GetXmlNodeValue(FindChildElement(dimensionNode, GB_STR("unitSymbol")));
					dimension.defaultValueUtf8 = GetXmlNodeValue(FindChildElement(dimensionNode, GB_STR("Default")));
					dimension.current = GB_Utf8Equals(GetXmlNodeValue(FindChildElement(dimensionNode, GB_STR("current"))), GB_STR("true"), false);
					for (CPLXMLNode* dimensionValueNode = dimensionNode->psChild; dimensionValueNode != nullptr; dimensionValueNode = dimensionValueNode->psNext)
					{
						if (dimensionValueNode->eType != CXT_Element)
						{
							continue;
						}

						const std::string nodeName = GetXmlNodeTagName(dimensionValueNode);
						if (!GB_Utf8Equals(nodeName, GB_STR("Value"), false))
						{
							continue;
						}
						dimension.values.push_back(GetXmlNodeValue(dimensionValueNode));
					}

					tileLayer.dimensions[dimension.identifierUtf8] = dimension;

					// TODO...
				}

				for (CPLXMLNode* setLinkNode = layerNode->psChild; setLinkNode != nullptr; setLinkNode = setLinkNode->psNext)
				{
					if (setLinkNode->eType != CXT_Element)
					{
						continue;
					}

					const std::string nodeName = GetXmlNodeTagName(setLinkNode);
					if (!GB_Utf8Equals(nodeName, GB_STR("TileMatrixSetLink"), false))
					{
						continue;
					}

					WmtsTileMatrixSetLink setLink;
					setLink.tileMatrixSetIdentifierUtf8 = GetXmlNodeValue(FindChildElement(setLinkNode, GB_STR("TileMatrixSet")));
					if (tileMatrixSets.find(setLink.tileMatrixSetIdentifierUtf8) == tileMatrixSets.end())
					{
						GBLOG_WARNING(GB_Utf8Format("TileMatrixSet '%s' referenced in TileMatrixSetLink of Layer '%s' not found. This set link will be ignored.", setLink.tileMatrixSetIdentifierUtf8.c_str(), tileLayer.identifierUtf8.c_str()));
						continue;
					}

					const WmtsTileMatrixSet& tileMatrixSet = tileMatrixSets[setLink.tileMatrixSetIdentifierUtf8];
					for (CPLXMLNode* setLimitsNode = setLinkNode->psChild; setLimitsNode != nullptr; setLimitsNode = setLimitsNode->psNext)
					{
						if (setLimitsNode->eType != CXT_Element)
						{
							continue;
						}

						const std::string nodeName = GetXmlNodeTagName(setLimitsNode);
						if (!GB_Utf8Equals(nodeName, GB_STR("TileMatrixSetLimits"), false))
						{
							continue;
						}

						for (CPLXMLNode* matrixLimitsNode = setLimitsNode->psChild; matrixLimitsNode != nullptr; matrixLimitsNode = matrixLimitsNode->psNext)
						{
							if (matrixLimitsNode->eType != CXT_Element)
							{
								continue;
							}

							const std::string nodeName = GetXmlNodeTagName(matrixLimitsNode);
							if (!GB_Utf8Equals(nodeName, GB_STR("TileMatrixLimits"), false))
							{
								continue;
							}

							const std::string id = GetXmlNodeValue(FindChildElement(matrixLimitsNode, GB_STR("TileMatrix")));
							bool isValid = false;
							int matrixWidth = -1, matrixHeight = -1;
							for (const auto& kvp : tileMatrixSet.tileMatrices)
							{
								if (kvp.second.identifierUtf8 == id)
								{
									isValid = true;
									matrixWidth = kvp.second.matrixWidth;
									matrixHeight = kvp.second.matrixHeight;
									break;
								}
							}
							if (!isValid)
							{
								GBLOG_WARNING(GB_Utf8Format("TileMatrix '%s' referenced in TileMatrixLimits of Layer '%s' and TileMatrixSet '%s' not found. This TileMatrixLimits will be ignored.", id.c_str(), tileLayer.identifierUtf8.c_str(), tileMatrixSet.identifierUtf8.c_str()));
								continue;
							}

							WmtsTileMatrixLimits matrixLimits;
							matrixLimits.rowIndexInterval.lower = static_cast<int>(GB_ToUInt(GetXmlNodeValue(FindChildElement(matrixLimitsNode, GB_STR("MinTileRow")))));
							matrixLimits.rowIndexInterval.upper = static_cast<int>(GB_ToUInt(GetXmlNodeValue(FindChildElement(matrixLimitsNode, GB_STR("MaxTileRow")))));
							matrixLimits.colIndexInterval.lower = static_cast<int>(GB_ToUInt(GetXmlNodeValue(FindChildElement(matrixLimitsNode, GB_STR("MinTileCol")))));
							matrixLimits.colIndexInterval.upper = static_cast<int>(GB_ToUInt(GetXmlNodeValue(FindChildElement(matrixLimitsNode, GB_STR("MaxTileCol")))));
							isValid = (matrixLimits.colIndexInterval.lower >= 0 && matrixLimits.colIndexInterval.lower < matrixWidth && matrixLimits.colIndexInterval.upper >= 0 && matrixLimits.colIndexInterval.upper < matrixWidth &&
								matrixLimits.rowIndexInterval.lower >= 0 && matrixLimits.rowIndexInterval.lower < matrixHeight && matrixLimits.rowIndexInterval.upper >= 0 && matrixLimits.rowIndexInterval.upper < matrixHeight &&
								matrixLimits.colIndexInterval.lower <= matrixLimits.colIndexInterval.upper && matrixLimits.rowIndexInterval.lower <= matrixLimits.rowIndexInterval.upper);
							if (!isValid)
							{
								GBLOG_WARNING(GB_Utf8Format("Invalid TileMatrixLimits for TileMatrix '%s' in Layer '%s' and TileMatrixSet '%s'. Col index intervals must be within [0, %d] and row index intervals must be within [0, %d], and lower bounds must be less than or equal to upper bounds. This TileMatrixLimits will be ignored.", id.c_str(), tileLayer.identifierUtf8.c_str(), tileMatrixSet.identifierUtf8.c_str(), matrixWidth - 1, matrixHeight - 1));
								continue;
							}
							setLink.limits[id] = matrixLimits;
						}
					}
					tileLayer.setLinks[setLink.tileMatrixSetIdentifierUtf8] = setLink;
				}

				for (CPLXMLNode* resourceUrlNode = layerNode->psChild; resourceUrlNode != nullptr; resourceUrlNode = resourceUrlNode->psNext)
				{
					if (resourceUrlNode->eType != CXT_Element)
					{
						continue;
					}

					const std::string nodeName = GetXmlNodeTagName(resourceUrlNode);
					if (!GB_Utf8Equals(nodeName, GB_STR("ResourceURL"), false))
					{
						continue;
					}

					const std::string format = GetXmlNodeAttribute(resourceUrlNode, GB_STR("format"));
					const std::string resourceType = GetXmlNodeAttribute(resourceUrlNode, GB_STR("resourceType"));
					const std::string tmpl = GetXmlNodeAttribute(resourceUrlNode, GB_STR("template"));
					if (format.empty() || resourceType.empty() || tmpl.empty())
					{
						GBLOG_WARNING(GB_Utf8Format("ResourceURL element in Layer '%s' is missing required attributes. All of 'format', 'resourceType', and 'template' attributes are required. This ResourceURL will be ignored.", tileLayer.identifierUtf8.c_str()));
						continue;
					}

					if (GB_Utf8Equals(resourceType, GB_STR("tile"), false))
					{
						tileLayer.getTileUrls[format] = tmpl;
					}
					else if (GB_Utf8Equals(resourceType, GB_STR("FeatureInfo"), false))
					{
						tileLayer.getFeatureInfoUrls[format] = tmpl;

						RasterIdentifyFormat identifyFormat = RasterIdentifyFormat::Unknown;
						if (GB_Utf8Equals(format, GB_STR("MIME"), false) || GB_Utf8Equals(format, GB_STR("text/plain"), false))
						{
							identifyFormat = RasterIdentifyFormat::Text;
						}
						else if (GB_Utf8Equals(format, GB_STR("text/html"), false))
						{
							identifyFormat = RasterIdentifyFormat::Html;
						}
						else if (GB_Utf8StartsWith(format, GB_STR("GML."), false) || GB_Utf8Equals(format, GB_STR("application/vnd.ogc.gml"), false) ||
							GB_Utf8Find(format, GB_STR("gml"), false) >= 0 || GB_Utf8Equals(format, GB_STR("application/json"), false) ||
							GB_Utf8Equals(format, GB_STR("application/geojson"), false) || GB_Utf8Equals(format, GB_STR("application/geo+json"), false))
						{
							identifyFormat = RasterIdentifyFormat::Feature;
						}
						else
						{
							continue;
						}
						identifyFormats[identifyFormat] = format;
					}
					else
					{
						continue;
					}
				}

				tileLayersSupported.push_back(tileLayer);
			}

			tileThemes.clear();
			for (CPLXMLNode* themeNode = rootNode->psChild; themeNode != nullptr; themeNode = themeNode->psNext)
			{
				if (themeNode->eType != CXT_Element)
				{
					continue;
				}

				const std::string nodeName = GetXmlNodeTagName(themeNode);
				if (!GB_Utf8Equals(nodeName, GB_STR("Theme"), false))
				{
					continue;
				}

				WmtsTheme theme;
				ParseTheme(themeNode, theme);
				tileThemes.push_back(theme);
			}

			for (WmtsTileLayer& tileLayer : tileLayersSupported)
			{
				if (tileLayer.boundingBoxes.empty())
				{
					if (!DetectTileLayerBoundingBox(tileLayer))
					{
						GBLOG_WARNING(GB_Utf8Format("No bounding box found for Layer '%s'. This layer may not render correctly.", tileLayer.identifierUtf8.c_str()));
						const static GeoBoundingBox defaultBoundingBox(GB_ToWkt("CRS:84"), GB_Rectangle(-180, -90, 180, 90));
						tileLayer.boundingBoxes.push_back(defaultBoundingBox);
					}
				}
			}
		}

		void ParseTheme(const CPLXMLNode* rootNode, WmtsTheme& theme)
		{
			theme.identifierUtf8 = GetXmlNodeValue(FindChildElement(rootNode, GB_STR("ows:Identifier")));
			theme.titleUtf8 = GetXmlNodeValue(FindChildElement(rootNode, GB_STR("ows:Title")));
			theme.abstractUtf8 = GetXmlNodeValue(FindChildElement(rootNode, GB_STR("ows:Abstract")));
			ParseKeywords(rootNode, theme.keywordsUtf8);

			const CPLXMLNode* themeNode = FindChildElement(rootNode, GB_STR("ows:Theme"));
			if (themeNode)
			{
				theme.subTheme = new WmtsTheme();
				ParseTheme(themeNode, *theme.subTheme);
			}
			else
			{
				theme.subTheme = nullptr;
			}

			theme.layerRefs.clear();
			for (CPLXMLNode* layerRefNode = rootNode->psChild; layerRefNode != nullptr; layerRefNode = layerRefNode->psNext)
			{
				if (layerRefNode->eType != CXT_Element)
				{
					continue;
				}

				const std::string nodeName = GetXmlNodeTagName(layerRefNode);
				if (!GB_Utf8Equals(nodeName, GB_STR("ows:LayerRef"), false))
				{
					continue;
				}

				theme.layerRefs.push_back(GetXmlNodeValue(layerRefNode));
			}
		}

		bool ShouldInvertAxisOrder(const std::string& wkt) const
		{
			bool shouldInvert = false;
			if ((capabilities.versionUtf8 == GB_STR("1.3.0") || capabilities.versionUtf8 == GB_STR("1.3")) && !parserOptions.ignoreAxisOrientation)
			{
				if (GeoCrsManager::IsDefinitionValidCached(wkt) && GeoCrsManager::IsDefinitionAxisOrderReversedCached(wkt))
				{
					shouldInvert = true;
				}
			}

			if (parserOptions.invertAxisOrientation)
			{
				shouldInvert = !shouldInvert;
				GBLOG_INFO(GB_Utf8Format("Axis orientation for CRS '%s' is being inverted due to parser options. Original axis order will be %s.", wkt.c_str(), shouldInvert ? "inverted" : "normal"));
			}
			return shouldInvert;
		}

		bool IsTianditu() const
		{
			if (capabilities.capability.request.getTile.dcpTypes.empty())
			{
				return false;
			}

			const std::string& dcpType = capabilities.capability.request.getTile.dcpTypes[0].http.get.onlineResource.xlinkHrefUtf8;
			return GB_Utf8Find(dcpType, GB_STR("tianditu"), false) > 0;
		}

		bool DetectTileLayerBoundingBox(WmtsTileLayer& tileLayer)
		{
			if (tileLayer.setLinks.empty())
			{
				return false;
			}

			for (const auto& kvp : tileLayer.setLinks)
			{
				const WmtsTileMatrixSetLink& setLink = kvp.second;
				const auto it = tileMatrixSets.find(setLink.tileMatrixSetIdentifierUtf8);
				if (it == tileMatrixSets.end())
				{
					continue;
				}

				const std::shared_ptr<const GeoCrs> crs = GeoCrsManager::GetFromDefinitionCached(it->second.crsUtf8);
				if (!GeoCrsManager::IsDefinitionValidCached(it->second.crsUtf8) || !crs)
				{
					continue;
				}

				auto lastIt = it->second.tileMatrices.end();
				lastIt--;
				if (lastIt == it->second.tileMatrices.end())
				{
					continue;
				}

				const WmtsTileMatrix& tileMatrix = lastIt->second;
				double metersPerUnit = crs->GetMetersPerUnit();
				if (arcGISMapServiceInfo && crs->IsGeographic() && IsVersionAtMost10_3(arcGISMapServiceInfo->m_currentVersion) &&
					GB_Utf8StartsWith(std::to_string(metersPerUnit), "111319.49"))
				{
					metersPerUnit = oldArcGISServerMetersPerUnit;
				}

				const double pixelSize = (IsTianditu() ? tiandituRenderingPixelSize : standardRenderingPixelSize);
				const double resolution = tileMatrix.scaleDenominator * pixelSize / metersPerUnit;
				const GB_Point2d bottomRight(tileMatrix.topLeft.x + resolution * tileMatrix.tileWidth * tileMatrix.matrixWidth, tileMatrix.topLeft.y - resolution * tileMatrix.tileHeight * tileMatrix.matrixHeight);
				GB_Rectangle extent(tileMatrix.topLeft, bottomRight);
				extent.Normalize();

				const GeoBoundingBox boundingBoxProperty(crs->ExportToWktUtf8(), extent);
				tileLayer.boundingBoxes.push_back(boundingBoxProperty);
			}

			return !tileLayer.boundingBoxes.empty();
		}
	};
}

bool ParseWmsCapabilities(const std::string& capabilitiesXmlUtf8, const std::string& baseUrl, WmsCapabilitiesProperty& outCapabilities, const ArcGISMapServiceInfo* arcGISMapServiceInfo, const WmsParserOptions& options)
{
	outCapabilities = WmsCapabilitiesProperty();

	WmsCapabilitiesParser parser;
	if (arcGISMapServiceInfo)
	{
		parser.SetArcGISMapServiceInfo(arcGISMapServiceInfo);
	}
	if (!parser.Parse(capabilitiesXmlUtf8, options, baseUrl))
	{
		return false;
	}

	outCapabilities = parser.GetCapabilities();
	outCapabilities.capability.layers = outCapabilities.capability.layers;
	outCapabilities.capability.tileLayers = parser.GetTileLayers();
	outCapabilities.capability.tileMatrixSets = parser.GetTileMatrixSets();
	parser.GetLayerParents(outCapabilities.capability.layerParents);
	return true;
}

namespace
{
	struct SerializeFrame
	{
		const WmsTreeNode* node = nullptr;
		size_t depth = 0;
	};

	static size_t CalculateSerializedLength(const WmsTreeNode& rootNode, size_t indentUnitLength)
	{
		size_t totalLength = 0;
		bool isFirstLine = true;

		std::vector<SerializeFrame> stack;
		stack.reserve(64);
		stack.push_back({ &rootNode, 0 });

		while (!stack.empty())
		{
			const SerializeFrame frame = stack.back();
			stack.pop_back();

			if (!isFirstLine)
			{
				totalLength += 1; // '\n'
			}
			isFirstLine = false;

			totalLength += frame.depth * indentUnitLength;
			totalLength += frame.node->textUtf8.size();

			const size_t numChildren = frame.node->children.size();
			for (size_t i = numChildren; i > 0; i--)
			{
				stack.push_back({ &frame.node->children[i - 1], frame.depth + 1 });
			}
		}

		return totalLength;
	}

	static void AppendIndent(std::string& output, const std::string& indentString, size_t depth)
	{
		if (indentString.empty() || depth == 0)
		{
			return;
		}

		for (size_t i = 0; i < depth; i++)
		{
			output.append(indentString);
		}
	}
}

std::string WmsTreeNode::ToString(const std::string& indentStringUtf8) const
{
	const size_t totalLength = CalculateSerializedLength(*this, indentStringUtf8.size());

	std::string output;
	output.reserve(totalLength);

	bool isFirstLine = true;

	std::vector<SerializeFrame> stack;
	stack.reserve(64);
	stack.push_back({ this, 0 });

	while (!stack.empty())
	{
		const SerializeFrame frame = stack.back();
		stack.pop_back();

		if (!isFirstLine)
		{
			output.push_back('\n');
		}
		isFirstLine = false;

		AppendIndent(output, indentStringUtf8, frame.depth);
		output.append(frame.node->textUtf8);

		const size_t numChildren = frame.node->children.size();
		for (size_t i = numChildren; i > 0; i--)
		{
			stack.push_back({ &frame.node->children[i - 1], frame.depth + 1 });
		}
	}
	return output;
}

namespace
{
	static std::vector<WmsTreeNode> CreateTileLayerFormatNodes(const WmtsTileLayer& tileLayer, const std::string& path)
	{
		std::vector<WmsTreeNode> formatNodes;
		formatNodes.reserve(tileLayer.formats.size());
		for (const std::string& format : tileLayer.formats)
		{
			WmsTreeNode formatNode;
			formatNode.textUtf8 = format;
			formatNode.nameUtf8 = format;
			formatNode.nodeType = WmsTreeNode::NodeType::Format;
			formatNode.uidUtf8 = GB_Md5Hash(path + "|" + format);
			formatNodes.push_back(std::move(formatNode));
		}
		std::sort(formatNodes.begin(), formatNodes.end(), [](const WmsTreeNode& a, const WmsTreeNode& b) {
			return GB_Utf8CompareLogical(a.textUtf8, b.textUtf8) < 0;
			});
		return formatNodes;
	}

	static std::vector<WmsTreeNode> CreateTileLayerTileMatrixSetNodes(const WmtsTileLayer& tileLayer, const BuildLayerTreeOptions& options, const std::string& path)
	{
		std::vector<WmsTreeNode> tileMatrixSetNodes;
		tileMatrixSetNodes.reserve(tileLayer.setLinks.size());
		for (const auto& kvp : tileLayer.setLinks)
		{
			WmsTreeNode tileMatrixSetNode;
			tileMatrixSetNode.textUtf8 = kvp.first;
			tileMatrixSetNode.nameUtf8 = kvp.first;
			tileMatrixSetNode.nodeType = WmsTreeNode::NodeType::WmtsTileMatrixSet;

			const std::string currentPath = path + "|" + kvp.first;
			tileMatrixSetNode.uidUtf8 = GB_Md5Hash(currentPath);
			if (!options.ignoreUniqueChildNode || tileLayer.formats.size() >= 2)
			{
				tileMatrixSetNode.children = CreateTileLayerFormatNodes(tileLayer, currentPath);
			}
			tileMatrixSetNodes.push_back(std::move(tileMatrixSetNode));
		}
		std::sort(tileMatrixSetNodes.begin(), tileMatrixSetNodes.end(), [](const WmsTreeNode& a, const WmsTreeNode& b) {
			return GB_Utf8CompareLogical(a.textUtf8, b.textUtf8) < 0;
			});
		return tileMatrixSetNodes;
	}

	static std::vector<WmsTreeNode> CreateTileLayerStyleNodes(const WmtsTileLayer& tileLayer, const BuildLayerTreeOptions& options, const std::string& path)
	{
		std::vector<WmsTreeNode> styleNodes;
		styleNodes.reserve(tileLayer.styles.size());
		for (const auto& kvp : tileLayer.styles)
		{
			const WmtsStyle& style = kvp.second;
			WmsTreeNode styleNode;
			styleNode.textUtf8 = style.titleUtf8.empty() ? style.identifierUtf8 : style.titleUtf8;
			styleNode.nameUtf8 = style.identifierUtf8;
			styleNode.nodeType = WmsTreeNode::NodeType::Style;

			const std::string currentPath = path + "|" + kvp.first;
			styleNode.uidUtf8 = GB_Md5Hash(currentPath);
			if (options.ignoreUniqueChildNode)
			{
				if (tileLayer.setLinks.size() >= 2)
				{
					styleNode.children = CreateTileLayerTileMatrixSetNodes(tileLayer, options, currentPath);
				}
				else if (tileLayer.formats.size() >= 2)
				{
					styleNode.children = CreateTileLayerFormatNodes(tileLayer, currentPath);
				}
			}
			else
			{
				styleNode.children = CreateTileLayerTileMatrixSetNodes(tileLayer, options, currentPath);
			}
			styleNodes.push_back(std::move(styleNode));
		}
		std::sort(styleNodes.begin(), styleNodes.end(), [](const WmsTreeNode& a, const WmsTreeNode& b) {
			return GB_Utf8CompareLogical(a.textUtf8, b.textUtf8) < 0;
			});
		return styleNodes;
	}

	static WmsTreeNode CreateTileLayerNode(const WmtsTileLayer& tileLayer, const BuildLayerTreeOptions& options, const std::string& path)
	{
		WmsTreeNode layerNode;
		layerNode.textUtf8 = tileLayer.titleUtf8.empty() ? tileLayer.identifierUtf8 : tileLayer.titleUtf8;
		layerNode.nameUtf8 = tileLayer.identifierUtf8;
		layerNode.nodeType = WmsTreeNode::NodeType::Layer;

		const std::string currentPath = path + "|" + tileLayer.identifierUtf8;
		layerNode.uidUtf8 = GB_Md5Hash(currentPath);

		if (options.ignoreUniqueChildNode)
		{
			if (tileLayer.styles.size() >= 2)
			{
				layerNode.children = CreateTileLayerStyleNodes(tileLayer, options, currentPath);
			}
			else if (tileLayer.setLinks.size() >= 2)
			{
				layerNode.children = CreateTileLayerTileMatrixSetNodes(tileLayer, options, currentPath);
			}
			else if (tileLayer.formats.size() >= 2)
			{
				layerNode.children = CreateTileLayerFormatNodes(tileLayer, currentPath);
			}
		}
		else
		{
			layerNode.children = CreateTileLayerStyleNodes(tileLayer, options, currentPath);
		}
		std::sort(layerNode.children.begin(), layerNode.children.end(), [](const WmsTreeNode& a, const WmsTreeNode& b) {
			return GB_Utf8CompareLogical(a.textUtf8, b.textUtf8) < 0;
			});
		return layerNode;
	}

	static WmsTreeNode CreateWmsLayerNode(const WmsLayerProperty& layer, const std::string& path)
	{
		WmsTreeNode layerNode;
		layerNode.textUtf8 = layer.titleUtf8.empty() ? (layer.nameUtf8.empty() ? std::to_string(layer.orderId) : layer.nameUtf8) : layer.titleUtf8;
		layerNode.nameUtf8 = layer.nameUtf8.empty() ? std::to_string(layer.orderId) : layer.nameUtf8;
		layerNode.nodeType = WmsTreeNode::NodeType::Layer;
		const std::string currentPath = path + "|" + (layer.nameUtf8.empty() ? std::to_string(layer.orderId) : layer.nameUtf8);
		layerNode.uidUtf8 = GB_Md5Hash(currentPath);

		layerNode.children.reserve(layer.subLayers.size());
		for (const WmsLayerProperty& subLayer : layer.subLayers)
		{
			WmsTreeNode subLayerNode = CreateWmsLayerNode(subLayer, currentPath);
			layerNode.children.push_back(std::move(subLayerNode));
		}
		std::sort(layerNode.children.begin(), layerNode.children.end(), [](const WmsTreeNode& a, const WmsTreeNode& b) {
			return GB_Utf8CompareLogical(a.textUtf8, b.textUtf8) < 0;
			});
		return layerNode;
	}
}

bool BuildWmsLayerTree(const WmsCapabilitiesProperty& capabilities, WmsTreeNode& rootNode, const BuildLayerTreeOptions& options)
{
	rootNode = WmsTreeNode();
	rootNode.textUtf8 = capabilities.service.titleUtf8;
	rootNode.nameUtf8 = "root";
	rootNode.nodeType = WmsTreeNode::NodeType::Root;

	const std::string currentPath = (rootNode.textUtf8.empty() ? GB_STR("root") : rootNode.textUtf8);
	rootNode.uidUtf8 = GB_Md5Hash(currentPath);
	rootNode.children.reserve(capabilities.capability.tileLayers.size() + capabilities.capability.layers.size());

	const std::vector<WmtsTileLayer>& tileLayers = capabilities.capability.tileLayers;
	for (const WmtsTileLayer& tileLayer : tileLayers)
	{
		WmsTreeNode layerNode = CreateTileLayerNode(tileLayer, options, currentPath);
		rootNode.children.push_back(std::move(layerNode));
	}

	const std::vector<WmsLayerProperty>& layers = capabilities.capability.layers;
	for (const WmsLayerProperty& layer : layers)
	{
		WmsTreeNode layerNode = CreateWmsLayerNode(layer, currentPath);
		rootNode.children.push_back(std::move(layerNode));
	}

	std::sort(rootNode.children.begin(), rootNode.children.end(), [](const WmsTreeNode& a, const WmsTreeNode& b) {
		return GB_Utf8CompareLogical(a.textUtf8, b.textUtf8) < 0;
		});
	return true;
}

namespace
{
	static const WmsLayerProperty* FindWmsLayer(const std::vector<WmsLayerProperty>& layers, const std::string& layerName)
	{
		for (size_t i = 0; i < layers.size(); i++)
		{
			const WmsLayerProperty& layer = layers[i];
			if (layer.nameUtf8 == layerName)
			{
				return &layer;
			}

			const WmsLayerProperty* foundLayer = FindWmsLayer(layer.subLayers, layerName);
			if (foundLayer)
			{
				return foundLayer;
			}
		}

		return nullptr;
	}

	static inline const WmsLayerProperty* FindWmsLayer(const WmsCapabilitiesProperty* capabilities, const std::string& layerName)
	{
		if (!capabilities || layerName.empty())
		{
			return nullptr;
		}

		return FindWmsLayer(capabilities->capability.layers, layerName);
	}

	static const WmtsTileLayer* FindWmtsTileLayer(const WmsCapabilitiesProperty* capabilities, const std::string& layerName)
	{
		if (!capabilities || layerName.empty())
		{
			return nullptr;
		}

		const std::vector<WmtsTileLayer>& tileLayers = capabilities->capability.tileLayers;
		for (const WmtsTileLayer& tileLayer : tileLayers)
		{
			if (tileLayer.identifierUtf8 == layerName)
			{
				return &tileLayer;
			}
		}

		return nullptr;
	}

	static std::string FindLayerTitle(const WmsCapabilitiesProperty* capabilities, const std::string& layerName, bool* success = nullptr)
	{
		if (success)
		{
			*success = false;
		}

		if (!capabilities || layerName.empty())
		{
			return "";
		}

		for (const WmtsTileLayer& wmtsLayer : capabilities->capability.tileLayers)
		{
			if (wmtsLayer.identifierUtf8 == layerName)
			{
				if (success)
				{
					*success = true;
				}
				return wmtsLayer.titleUtf8;
			}
		}

		const WmsLayerProperty* foundWmsLayer = FindWmsLayer(capabilities->capability.layers, layerName);
		if (foundWmsLayer)
		{
			if (success)
			{
				*success = true;
			}
			return foundWmsLayer->titleUtf8;
		}

		return "";
	}

	static std::vector<std::string> GetTileLayerStyles(const WmtsTileLayer* tileLayer, bool* success = nullptr)
	{
		if (success)
		{
			*success = false;
		}

		if (!tileLayer)
		{
			return {};
		}

		std::vector<std::string> styles;
		styles.reserve(tileLayer->styles.size());
		for (const auto& pair : tileLayer->styles)
		{
			styles.push_back(pair.first);
		}

		if (success)
		{
			*success = true;
		}
		return styles;
	}

	static std::vector<std::string> GetWmsLayerStyles(const WmsLayerProperty* wmsLayer, bool* success = nullptr)
	{
		if (success)
		{
			*success = false;
		}

		if (!wmsLayer)
		{
			return {};
		}

		std::vector<std::string> styles(wmsLayer->styles.size());
		for (size_t i = 0; i < wmsLayer->styles.size(); i++)
		{
			styles[i] = wmsLayer->styles[i].nameUtf8;
		}

		if (success)
		{
			*success = true;
		}
		return styles;
	}

	static std::vector<std::string> FindTileLayerStyles(const WmsCapabilitiesProperty* capabilities, const std::string& tileLayerName, bool* success = nullptr)
	{
		if (success)
		{
			*success = false;
		}

		if (!capabilities || tileLayerName.empty())
		{
			return {};
		}

		const WmtsTileLayer* tileLayer = FindWmtsTileLayer(capabilities, tileLayerName);
		if (!tileLayer)
		{
			return {};
		}

		return GetTileLayerStyles(tileLayer, success);
	}

	static std::vector<std::string> GetTileLayerMatrixSets(const WmtsTileLayer* tileLayer, bool* success = nullptr)
	{
		if (success)
		{
			*success = false;
		}

		if (!tileLayer)
		{
			return {};
		}

		std::vector<std::string> matrixSets;
		matrixSets.reserve(tileLayer->setLinks.size());
		for (const auto& pair : tileLayer->setLinks)
		{
			matrixSets.push_back(pair.first);
		}

		if (success)
		{
			*success = true;
		}
		return matrixSets;
	}

	static std::vector<std::string> FindTileLayerMatrixSets(const WmsCapabilitiesProperty* capabilities, const std::string& tileLayerName, bool* success = nullptr)
	{
		if (success)
		{
			*success = false;
		}

		if (!capabilities || tileLayerName.empty())
		{
			return {};
		}

		const WmtsTileLayer* tileLayer = FindWmtsTileLayer(capabilities, tileLayerName);
		if (!tileLayer)
		{
			return {};
		}

		return GetTileLayerMatrixSets(tileLayer, success);
	}

	static std::string SelectAppropriateWmsFormat(const std::vector<std::string>& supportedWmsFormats, bool* success = nullptr)
	{
		if (success)
		{
			*success = false;
		}

		static const std::vector<std::string> preferredFullFormats = {
			"image/png", "image/tiff", "image/geotiff", "image/bmp", "image/jpg", "image/jpeg", "image/webp", "image/gif"
		};
		static const std::vector<std::string> preferredFormatSections = {
			"png", "tiff", "bmp", "jpg", "jpeg", "webp", "gif"
		};

		for (const std::string& fullFormat : preferredFullFormats)
		{
			for (const std::string& supportedWmsFormat : supportedWmsFormats)
			{
				if (GB_Utf8Equals(fullFormat, supportedWmsFormat, false))
				{
					if (success)
					{
						*success = true;
					}
					return supportedWmsFormat;
				}
			}
		}

		for (const std::string& formatSection : preferredFormatSections)
		{
			for (const std::string& supportedWmsFormat : supportedWmsFormats)
			{
				if (GB_Utf8Find(supportedWmsFormat, formatSection, false) >= 0)
				{
					if (success)
					{
						*success = true;
					}
					return supportedWmsFormat;
				}
			}
		}

		return "";
	}

	static bool TransformGeoPolygon(const std::string& sourceWkt, const GB_Polygon& polygon, const std::string& targetWkt, std::vector<GB_Polygon>& outResult)
	{
		outResult.clear();

		std::shared_ptr<const GeoCrs> sourceCrs = GeoCrsManager::GetFromDefinitionCached(sourceWkt);
		if (!sourceCrs || !polygon.IsValid())
		{
			GBLOG_WARNING(GB_Utf8Format("Failed to get source CRS from WKT '%s' or the input polygon is invalid. Source CRS: %s, Polygon valid: %s", sourceWkt.c_str(), sourceCrs ? "valid" : "invalid", polygon.IsValid() ? "true" : "false"));
			return false;
		}

		const GeoBoundingBox srcCrsMaxBBox = sourceCrs->GetValidArea(); // 原坐标系在它自身坐标系下的最大范围
		if (!srcCrsMaxBBox.IsValid())
		{
			GBLOG_WARNING(GB_Utf8Format("Valid area of source CRS '%s' is invalid. Source CRS: %s, Valid area: %s", sourceWkt.c_str(), sourceCrs->ExportToWktUtf8().c_str(), srcCrsMaxBBox.rect.SerializeToString().c_str()));
			return false;
		}

		std::vector<GB_Polygon> validSrcAreas; // 原多边形在原坐标系下的有效范围
		std::vector<std::vector<GB_Polygon>> holes;
		if (!polygon.ComputeIntersection(GB_Polygon(srcCrsMaxBBox.rect), validSrcAreas, holes))
		{
			GBLOG_WARNING(GB_Utf8Format("Failed to compute intersection of input polygon with valid area of source CRS '%s'. Source CRS: %s, Valid area: %s, Input polygon: %s", sourceWkt.c_str(), sourceCrs->ExportToWktUtf8().c_str(), srcCrsMaxBBox.rect.SerializeToString().c_str(), polygon.SerializeToString().c_str()));
			return false;
		}

		std::vector<GB_Polygon> validSrcAreasIn4326; // 原多边形的有效范围转换到4326坐标系下的范围
		validSrcAreasIn4326.reserve(validSrcAreas.size());
		for (const GB_Polygon& validSrcArea : validSrcAreas)
		{
			GB_Polygon validSrcAreaIn4326;
			if (GeoCrsTransform::TransformPolygon(sourceWkt, GB_ToWkt("EPSG:4326"), validSrcArea, validSrcAreaIn4326) && validSrcAreaIn4326.IsValid())
			{
				validSrcAreasIn4326.push_back(std::move(validSrcAreaIn4326));
				continue;
			}
		}
		if (validSrcAreasIn4326.empty())
		{
			GBLOG_WARNING(GB_Utf8Format("Failed to transform any valid area of the input polygon to EPSG:4326. Source CRS: %s, Valid areas in source CRS: %d, Input polygon: %s", sourceWkt.c_str(), static_cast<int>(validSrcAreas.size()), polygon.SerializeToString().c_str()));
			return false;
		}

		std::shared_ptr<const GeoCrs> targetCrs = GeoCrsManager::GetFromDefinitionCached(targetWkt);
		if (!targetCrs)
		{
			GBLOG_WARNING(GB_Utf8Format("Failed to get target CRS from WKT '%s'. Target CRS: %s", targetWkt.c_str(), targetCrs ? "valid" : "invalid"));
			return false;
		}
		const GeoBoundingBox targetCrsMaxBBoxIn4326 = targetCrs->GetValidAreaLonLat(); // 目标坐标系在4326坐标系下的最大范围
		if (!targetCrsMaxBBoxIn4326.IsValid())
		{
			GBLOG_WARNING(GB_Utf8Format("Valid area of target CRS '%s' is invalid. Target CRS: %s, Valid area in 4326: %s", targetWkt.c_str(), targetCrs->ExportToWktUtf8().c_str(), targetCrsMaxBBoxIn4326.rect.SerializeToString().c_str()));
			return false;
		}

		std::vector<GB_Polygon> intersectionAreasIn4326; // 原多边形的有效范围转换到4326坐标系下与目标坐标系在4326坐标系下的有效范围的交集
		for (const GB_Polygon& validSrcAreaIn4326 : validSrcAreasIn4326)
		{
			std::vector<GB_Polygon> intersectionAreas;
			std::vector<std::vector<GB_Polygon>> intersectioHoles;
			if (validSrcAreaIn4326.ComputeIntersection(GB_Polygon(targetCrsMaxBBoxIn4326.rect), intersectionAreas, intersectioHoles))
			{
				for (const GB_Polygon& intersectionArea : intersectionAreas)
				{
					if (intersectionArea.IsValid())
					{
						intersectionAreasIn4326.push_back(intersectionArea);
					}
				}
			}
		}
		if (intersectionAreasIn4326.empty())
		{
			GBLOG_WARNING(GB_Utf8Format("No intersection area between valid areas of the input polygon transformed to 4326 and the valid area of the target CRS transformed to 4326. Source CRS: %s, Target CRS: %s, Valid areas in 4326: %d, Target CRS valid area in 4326: %s, Input polygon: %s", sourceWkt.c_str(), targetWkt.c_str(), static_cast<int>(validSrcAreasIn4326.size()), targetCrsMaxBBoxIn4326.rect.SerializeToString().c_str(), polygon.SerializeToString().c_str()));
			return false;
		}

		outResult.reserve(intersectionAreasIn4326.size());
		for (const GB_Polygon& intersectionAreaIn4326 : intersectionAreasIn4326)
		{
			GB_Polygon transformedPolygon;
			if (GeoCrsTransform::TransformPolygon(GB_ToWkt("EPSG:4326"), targetWkt, intersectionAreaIn4326, transformedPolygon) && transformedPolygon.IsValid())
			{
				outResult.push_back(std::move(transformedPolygon));
			}
		}
		if (outResult.empty())
		{
			GBLOG_WARNING(GB_Utf8Format("Failed to transform any intersection area to target CRS. Source CRS: %s, Target CRS: %s, Intersection areas in 4326: %d, Input polygon: %s", sourceWkt.c_str(), targetWkt.c_str(), static_cast<int>(intersectionAreasIn4326.size()), polygon.SerializeToString().c_str()));
			return false;
		}
		return true;
	}
}

std::vector<MapRequestItem> BuildVisibleMapRequestItems(const BuildVisibleMapRequestItemsInput& input, bool* success)
{
	if (success)
	{
		*success = false;
	}

	if (!input.capabilities)
	{
		GBLOG_WARNING("Capabilities is null.");
		return {};
	}

	if (input.requestAreaWkt.empty() || !input.requestAreaPolygon.IsValid())
	{
		GBLOG_WARNING(GB_Utf8Format("Request area is invalid. requestAreaWkt empty: %s, polygon valid: %s", input.requestAreaWkt.empty() ? "true" : "false", input.requestAreaPolygon.IsValid() ? "true" : "false"));
		return {};
	}

	std::vector<MapRequestItem> outRequestItems;
	std::set<std::string> usedItemUids;
	std::string styleName = "";
	std::string tileMatrixSetName = "";
	std::string formatName = "";

	if (input.mapType == MapTileMode::WMTS)
	{
		const WmtsTileLayer* tileLayer = FindWmtsTileLayer(input.capabilities, input.layerNameUtf8);
		if (!tileLayer)
		{
			GBLOG_WARNING(GB_Utf8Format("Tile layer '%s' not found in capabilities.", input.layerNameUtf8.c_str()));
			return {};
		}

		const std::vector<std::string> tileLayerStyles = GetTileLayerStyles(tileLayer);
		if (input.styleUtf8.empty())
		{
			if (tileLayerStyles.size() != 1)
			{
				GBLOG_WARNING(GB_Utf8Format("Style is not specified for tile layer '%s', and there are %d styles available. Unable to determine which style to use.", input.layerNameUtf8.c_str(), static_cast<int>(tileLayerStyles.size())));
				return {};
			}
			styleName = tileLayerStyles[0];
		}
		else
		{
			if (std::find(tileLayerStyles.begin(), tileLayerStyles.end(), input.styleUtf8) == tileLayerStyles.end())
			{
				GBLOG_WARNING(GB_Utf8Format("Style '%s' not found for tile layer '%s'.", input.styleUtf8.c_str(), input.layerNameUtf8.c_str()));
				return {};
			}
			styleName = input.styleUtf8;
		}

		const std::vector<std::string> matrixSetNames = GetTileLayerMatrixSets(tileLayer);
		if (input.tileMatrixSetUtf8.empty())
		{
			if (matrixSetNames.size() != 1)
			{
				GBLOG_WARNING(GB_Utf8Format("Tile matrix set is not specified for tile layer '%s', and there are %d tile matrix sets available. Unable to determine which tile matrix set to use.", input.layerNameUtf8.c_str(), static_cast<int>(matrixSetNames.size())));
				return {};
			}
			tileMatrixSetName = matrixSetNames[0];
		}
		else
		{
			if (std::find(matrixSetNames.begin(), matrixSetNames.end(), input.tileMatrixSetUtf8) == matrixSetNames.end())
			{
				GBLOG_WARNING(GB_Utf8Format("Tile matrix set '%s' not found for tile layer '%s'.", input.tileMatrixSetUtf8.c_str(), input.layerNameUtf8.c_str()));
				return {};
			}
			tileMatrixSetName = input.tileMatrixSetUtf8;
		}

		const std::vector<std::string>& formats = tileLayer->formats;
		if (input.formatUtf8.empty())
		{
			if (formats.size() != 1)
			{
				GBLOG_WARNING(GB_Utf8Format("Format is not specified for tile layer '%s', and there are %d formats available. Unable to determine which format to use.", input.layerNameUtf8.c_str(), static_cast<int>(formats.size())));
				return {};
			}
			formatName = formats[0];
		}
		else
		{
			if (std::find(formats.begin(), formats.end(), input.formatUtf8) == formats.end())
			{
				GBLOG_WARNING(GB_Utf8Format("Format '%s' not found for tile layer '%s'.", input.formatUtf8.c_str(), input.layerNameUtf8.c_str()));
				return {};
			}
			formatName = input.formatUtf8;
		}

		const WmtsTileMatrixSet* tileMatrixSet = nullptr;
		{
			const std::unordered_map<std::string, WmtsTileMatrixSet>::const_iterator it = input.capabilities->capability.tileMatrixSets.find(tileMatrixSetName);
			if (it == input.capabilities->capability.tileMatrixSets.end())
			{
				GBLOG_WARNING(GB_Utf8Format("Tile matrix set '%s' not found in capabilities.", tileMatrixSetName.c_str()));
				return {};
			}
			tileMatrixSet = &(it->second);
		}
		if (!tileMatrixSet)
		{
			GBLOG_WARNING(GB_Utf8Format("Tile matrix set '%s' not found in capabilities.", tileMatrixSetName.c_str()));
			return {};
		}

		std::string tileMatrixSetCanonicalWktUtf8 = "";
		if (!TryCanonicalizeCrsDefinition(tileMatrixSet->crsUtf8, tileMatrixSetCanonicalWktUtf8))
		{
			GBLOG_WARNING(GB_Utf8Format("Failed to canonicalize CRS '%s' of tile matrix set '%s'.", tileMatrixSet->crsUtf8.c_str(), tileMatrixSetName.c_str()));
			return {};
		}

		std::vector<GB_Polygon> requestAreaInTileMatrixSetCrs;
		if (!TransformGeoPolygon(input.requestAreaWkt, input.requestAreaPolygon, tileMatrixSet->crsUtf8, requestAreaInTileMatrixSetCrs) || requestAreaInTileMatrixSetCrs.empty())
		{
			GBLOG_WARNING(GB_Utf8Format("Failed to transform request area polygon to CRS of tile matrix set '%s'. Request area WKT: %s, Request area polygon: %s, Tile matrix set CRS: %s", tileMatrixSetName.c_str(), input.requestAreaWkt.c_str(), input.requestAreaPolygon.SerializeToString().c_str(), tileMatrixSet->crsUtf8.c_str()));
			return {};
		}

		const WmtsTileMatrixSetLink* tileMatrixSetLink = nullptr;
		{
			const auto setLinkIterator = tileLayer->setLinks.find(tileMatrixSetName);
			if (setLinkIterator != tileLayer->setLinks.end())
			{
				tileMatrixSetLink = &(setLinkIterator->second);
			}
		}

		for (size_t polygonIndex = 0; polygonIndex < requestAreaInTileMatrixSetCrs.size(); polygonIndex++)
		{
			const GB_Polygon& requestPolygon = requestAreaInTileMatrixSetCrs[polygonIndex];
			GB_Rectangle requestBoundingBox;
			if (!TryGetPolygonBoundingBox(requestPolygon, requestBoundingBox))
			{
				continue;
			}

			double targetResolution = 0;
			int requestedZoomLevel = -1;
			if (!ResolveTargetResolution(input, tileMatrixSet->crsUtf8, requestBoundingBox, tileMatrixSet, targetResolution, requestedZoomLevel))
			{
				GBLOG_WARNING(GB_Utf8Format("Failed to resolve target resolution for WMTS layer '%s'. Request polygon index: %d, bounding box: %s", input.layerNameUtf8.c_str(), static_cast<int>(polygonIndex), requestBoundingBox.SerializeToString().c_str()));
				return {};
			}

			const WmtsTileMatrix* selectedTileMatrix = SelectTileMatrix(*tileMatrixSet, targetResolution, requestedZoomLevel, input.renderTarget.lodSelectMode, input.minZoomLevel, input.maxZoomLevel);
			if (!selectedTileMatrix)
			{
				GBLOG_WARNING(GB_Utf8Format("Failed to select tile matrix for WMTS layer '%s'. Target resolution: %.15g, requested zoom: %d", input.layerNameUtf8.c_str(), targetResolution, requestedZoomLevel));
				return {};
			}

			const int zoomLevel = GetTileMatrixZoomLevel(*tileMatrixSet, *selectedTileMatrix);
			const WmtsTileMatrixLimits* tileMatrixLimits = nullptr;
			if (tileMatrixSetLink)
			{
				const std::unordered_map<std::string, WmtsTileMatrixLimits>::const_iterator limitsIterator = tileMatrixSetLink->limits.find(selectedTileMatrix->identifierUtf8);
				if (limitsIterator != tileMatrixSetLink->limits.end())
				{
					tileMatrixLimits = &(limitsIterator->second);
				}
			}

			GB_IntInterval colIndexInterval;
			GB_IntInterval rowIndexInterval;
			if (!selectedTileMatrix->Intersects(requestBoundingBox, tileMatrixLimits, colIndexInterval, rowIndexInterval) || !colIndexInterval.IsValid() || !rowIndexInterval.IsValid())
			{
				continue;
			}

			for (int rowIndex = rowIndexInterval.lower; rowIndex <= rowIndexInterval.upper; rowIndex++)
			{
				for (int colIndex = colIndexInterval.lower; colIndex <= colIndexInterval.upper; colIndex++)
				{
					const GB_Rectangle tileRectangle = selectedTileMatrix->TileRect(colIndex, rowIndex);
					if (!tileRectangle.IsValid())
					{
						continue;
					}

					const GB_Rectangle intersectionBoundingBox = tileRectangle.Intersected(requestBoundingBox);
					if (!intersectionBoundingBox.IsValid() || !std::isfinite(intersectionBoundingBox.Area()) || intersectionBoundingBox.Area() <= 0)
					{
						continue;
					}

					std::vector<GB_Polygon> intersectionAreas;
					std::vector<std::vector<GB_Polygon>> holeAreas;
					if (!requestPolygon.ComputeIntersection(GB_Polygon(tileRectangle), intersectionAreas, holeAreas) || intersectionAreas.empty())
					{
						continue;
					}

					const double intersectionArea = ComputePolygonRegionsNetArea(intersectionAreas, holeAreas);
					if (!std::isfinite(intersectionArea) || intersectionArea <= 0.0)
					{
						continue;
					}

					std::string urlUtf8 = BuildWmtsRestfulUrl(*tileLayer, formatName, styleName, tileMatrixSetName, *selectedTileMatrix, rowIndex, colIndex);
					if (urlUtf8.empty())
					{
						urlUtf8 = BuildWmtsKvpUrl(input, *tileLayer, styleName, tileMatrixSetName, formatName, *selectedTileMatrix, rowIndex, colIndex);
					}
					if (urlUtf8.empty())
					{
						GBLOG_WARNING(GB_Utf8Format("Failed to build WMTS tile URL. Layer: %s, TileMatrixSet: %s, TileMatrix: %s, Row: %d, Col: %d", input.layerNameUtf8.c_str(), tileMatrixSetName.c_str(), selectedTileMatrix->identifierUtf8.c_str(), rowIndex, colIndex));
						return {};
					}

					MapRequestItem requestItem;
					requestItem.urlUtf8 = urlUtf8;
					requestItem.boundingBox.Set(tileMatrixSetCanonicalWktUtf8, tileRectangle);
					requestItem.zoomLevel = zoomLevel;
					requestItem.rowIndex = static_cast<size_t>(rowIndex);
					requestItem.colIndex = static_cast<size_t>(colIndex);
					requestItem.uidUtf8 = input.layerNameUtf8 + "|" + tileMatrixSetName + "|" + selectedTileMatrix->identifierUtf8 + "|" + std::to_string(rowIndex) + "|" + std::to_string(colIndex);

					if (usedItemUids.insert(requestItem.uidUtf8).second)
					{
						outRequestItems.push_back(std::move(requestItem));
					}
				}
			}
		}
	}
	else if (input.mapType == MapTileMode::WMSC)
	{
		const WmsLayerProperty* wmsLayer = FindWmsLayer(input.capabilities, input.layerNameUtf8);
		if (!wmsLayer)
		{
			GBLOG_WARNING(GB_Utf8Format("Layer '%s' not found in capabilities.", input.layerNameUtf8.c_str()));
			return {};
		}

		const std::vector<std::string> wmsLayerStyles = GetWmsLayerStyles(wmsLayer);
		if (input.styleUtf8.empty())
		{
			if (wmsLayerStyles.empty())
			{
				styleName.clear();
			}
			else if (wmsLayerStyles.size() == 1)
			{
				styleName = wmsLayerStyles[0];
			}
			else
			{
				GBLOG_WARNING(GB_Utf8Format("Style is not specified for layer '%s', and there are %d styles available. Unable to determine which style to use.", input.layerNameUtf8.c_str(), static_cast<int>(wmsLayerStyles.size())));
				return {};
			}
		}
		else
		{
			if (std::find(wmsLayerStyles.begin(), wmsLayerStyles.end(), input.styleUtf8) == wmsLayerStyles.end())
			{
				GBLOG_WARNING(GB_Utf8Format("Style '%s' not found for layer '%s'.", input.styleUtf8.c_str(), input.layerNameUtf8.c_str()));
				return {};
			}
			styleName = input.styleUtf8;
		}

		const std::vector<std::string>& formats = input.capabilities->capability.request.getMap.formatsUtf8;
		if (input.formatUtf8.empty())
		{
			formatName = SelectAppropriateWmsFormat(formats);
			if (formatName.empty())
			{
				GBLOG_WARNING(GB_Utf8Format("Unable to determine an appropriate WMS format for layer '%s'.", input.layerNameUtf8.c_str()));
				return {};
			}
		}
		else
		{
			if (std::find(formats.begin(), formats.end(), input.formatUtf8) == formats.end())
			{
				GBLOG_WARNING(GB_Utf8Format("Format '%s' not found for GetMap request.", input.formatUtf8.c_str()));
				return {};
			}
			formatName = input.formatUtf8;
		}

		const std::string wmsLayerCrsDefinitionUtf8 = wmsLayer->PreferredAvailableCrs();
		if (wmsLayerCrsDefinitionUtf8.empty())
		{
			GBLOG_WARNING(GB_Utf8Format("No available CRS found for WMS layer '%s'.", input.layerNameUtf8.c_str()));
			return {};
		}

		std::string wmsLayerCanonicalWktUtf8 = "";
		if (!TryCanonicalizeCrsDefinition(wmsLayerCrsDefinitionUtf8, wmsLayerCanonicalWktUtf8))
		{
			GBLOG_WARNING(GB_Utf8Format("Failed to canonicalize CRS '%s' for WMS layer '%s'.", wmsLayerCrsDefinitionUtf8.c_str(), input.layerNameUtf8.c_str()));
			return {};
		}

		std::vector<GB_Polygon> requestAreaInWmsCrs;
		if (!TransformGeoPolygon(input.requestAreaWkt, input.requestAreaPolygon, wmsLayerCrsDefinitionUtf8, requestAreaInWmsCrs) || requestAreaInWmsCrs.empty())
		{
			GBLOG_WARNING(GB_Utf8Format("Failed to transform request area polygon to CRS '%s' of WMS layer '%s'.", wmsLayerCrsDefinitionUtf8.c_str(), input.layerNameUtf8.c_str()));
			return {};
		}

		const size_t serviceMaxWidth = input.capabilities->service.maxWidth;
		const size_t serviceMaxHeight = input.capabilities->service.maxHeight;
		for (size_t polygonIndex = 0; polygonIndex < requestAreaInWmsCrs.size(); polygonIndex++)
		{
			const GB_Polygon& requestPolygon = requestAreaInWmsCrs[polygonIndex];
			GB_Rectangle requestBoundingBox;
			if (!TryGetPolygonBoundingBox(requestPolygon, requestBoundingBox))
			{
				continue;
			}

			double targetResolution = 0;
			int requestedZoomLevel = -1;
			if (!ResolveTargetResolution(input, wmsLayerCrsDefinitionUtf8, requestBoundingBox, nullptr, targetResolution, requestedZoomLevel))
			{
				GBLOG_WARNING(GB_Utf8Format("Failed to resolve target resolution for WMS layer '%s'. Request polygon index: %d, bounding box: %s", input.layerNameUtf8.c_str(), static_cast<int>(polygonIndex), requestBoundingBox.SerializeToString().c_str()));
				return {};
			}

			std::vector<GB_Rectangle> requestRectangles;
			BuildAdaptivePolygonRequestRectangles(requestPolygon, requestBoundingBox, targetResolution, requestRectangles);
			if (requestRectangles.empty())
			{
				continue;
			}

			for (size_t requestRectangleIndex = 0; requestRectangleIndex < requestRectangles.size(); requestRectangleIndex++)
			{
				const GB_Rectangle& currentRequestRectangle = requestRectangles[requestRectangleIndex];
				if (!currentRequestRectangle.IsValid())
				{
					continue;
				}

				size_t imageWidth = 0;
				size_t imageHeight = 0;
				if (!ComputeWmsImageSize(input, *wmsLayer, currentRequestRectangle, targetResolution, serviceMaxWidth, serviceMaxHeight, imageWidth, imageHeight))
				{
					GBLOG_WARNING(GB_Utf8Format("Failed to compute WMS image size for layer '%s'. Request polygon index: %d, bounding box: %s", input.layerNameUtf8.c_str(), static_cast<int>(polygonIndex), currentRequestRectangle.SerializeToString().c_str()));
					return {};
				}

				std::string urlUtf8 = BuildWmsGetMapUrl(input, *wmsLayer, styleName, wmsLayerCrsDefinitionUtf8, formatName, currentRequestRectangle, imageWidth, imageHeight);
				if (urlUtf8.empty())
				{
					GBLOG_WARNING(GB_Utf8Format("Failed to build WMS GetMap URL for layer '%s'.", input.layerNameUtf8.c_str()));
					return {};
				}

				MapRequestItem requestItem;
				requestItem.urlUtf8 = urlUtf8;
				requestItem.boundingBox.Set(wmsLayerCanonicalWktUtf8, currentRequestRectangle);
				requestItem.zoomLevel = requestedZoomLevel;
				requestItem.uidUtf8 = input.layerNameUtf8 + "|" + wmsLayerCrsDefinitionUtf8 + "|" + formatName + "|" + std::to_string(imageWidth) + "x" + std::to_string(imageHeight) + "|" + BuildRectangleBboxText(currentRequestRectangle);

				if (usedItemUids.insert(requestItem.uidUtf8).second)
				{
					outRequestItems.push_back(std::move(requestItem));
				}
			}
		}
	}
	else
	{
		GBLOG_WARNING(GB_Utf8Format("MapTileMode %d is not supported by BuildVisibleMapRequestItems yet.", static_cast<int>(input.mapType)));
		return {};
	}

	if (success)
	{
		*success = true;
	}
	return outRequestItems;
}


////#include <cstdlib>
//#include <locale>
//#include <memory>
//#include <string>
////
////namespace internal
////{
////	struct CplFreeDeleter
////	{
////		void operator()(char* text) const
//		{
//			if (text != nullptr)
//			{
//				CPLFree(text);
//			}
//		}
////	};
////
////	static std::string NormalizeToken(const std::string& text)
//	{
//		std::string normalized;
//		normalized.reserve(text.size());
//
//		for (size_t i = 0; i < text.size(); i++)
//		{
//			const unsigned char character = static_cast<unsigned char>(text[i]);
//
//			if ((character >= 'a' && character <= 'z') ||
//				(character >= '0' && character <= '9'))
//			{
//				normalized.push_back(static_cast<char>(character));
//			}
//			else if (character >= 'A' && character <= 'Z')
//			{
//				normalized.push_back(static_cast<char>(character - 'A' + 'a'));
//			}
//		}
//
//		return normalized;
//	}
////
////	static bool IsOnlyTrailingAsciiWhitespace(const char* text)
//	{
//		if (text == nullptr)
//		{
//			return true;
//		}
//
//		while (*text != '\0')
//		{
//			if (*text != ' ' &&
//				*text != '	' &&
//				*text != '\r' &&
//				*text != '\n' &&
//				*text != '\f' &&
//				*text != '\v')
//			{
//				return false;
//			}
//			text++;
//		}
//
//		return true;
//	}
////
////	static bool TryImportSpatialReference(const std::string& wkt, OGRSpatialReference& spatialReference)
//	{
//		if (wkt.empty())
//		{
//			return false;
//		}
//
//		const char* current = wkt.c_str();
//		if (spatialReference.importFromWkt(&current) != OGRERR_NONE)
//		{
//			return false;
//		}
//
//		return IsOnlyTrailingAsciiWhitespace(current);
//	}
////
////	static bool TryImportSrsNode(const std::string& wkt, OGR_SRSNode& rootNode)
//	{
//		if (wkt.empty())
//		{
//			return false;
//		}
//
//		const char* current = wkt.c_str();
//		if (rootNode.importFromWkt(&current) != OGRERR_NONE)
//		{
//			return false;
//		}
//
//		return IsOnlyTrailingAsciiWhitespace(current);
//	}
////
////	static bool TryExportToWktWithFormat(const OGRSpatialReference& spatialReference, const char* formatName, std::string& outputWkt)
//	{
//		const std::string formatOption = std::string("FORMAT=") + formatName;
//		const char* options[] =
//		{
//			formatOption.c_str(),
//			nullptr
//		};
//
//		char* rawWkt = nullptr;
//		if (spatialReference.exportToWkt(&rawWkt, options) != OGRERR_NONE || rawWkt == nullptr)
//		{
//			return false;
//		}
//
//		std::unique_ptr<char, CplFreeDeleter> wktHolder(rawWkt);
//		outputWkt.assign(wktHolder.get());
//		return true;
//	}
////
////	static bool TryExportToWkt2(const OGRSpatialReference& spatialReference, std::string& outputWkt)
//	{
//		static const char* const formatNames[] =
//		{
//			"WKT2_2019",
//			"WKT2_2018",
//			"WKT2"
//		};
//
//		for (size_t i = 0; i < sizeof(formatNames) / sizeof(formatNames[0]); i++)
//		{
//			if (TryExportToWktWithFormat(spatialReference, formatNames[i], outputWkt))
//			{
//				return true;
//			}
//		}
//
//		return false;
//	}
////
////	static bool HasExplicitAreaAndBbox(const OGR_SRSNode* node)
//	{
//		if (node == nullptr)
//		{
//			return false;
//		}
//
//		return node->GetNode("AREA") != nullptr && node->GetNode("BBOX") != nullptr;
//	}
////
////	static bool IsTransverseMercator(const OGRSpatialReference& spatialReference)
//	{
//		const char* projectionName = spatialReference.GetAttrValue("PROJECTION");
//		if (projectionName != nullptr)
//		{
//			if (NormalizeToken(projectionName) == NormalizeToken(SRS_PT_TRANSVERSE_MERCATOR))
//			{
//				return true;
//			}
//		}
//
//		const char* methodName = spatialReference.GetAttrValue("METHOD");
//		if (methodName != nullptr)
//		{
//			if (NormalizeToken(methodName) == "transversemercator")
//			{
//				return true;
//			}
//		}
//
//		methodName = spatialReference.GetAttrValue("CONVERSION|METHOD");
//		if (methodName != nullptr)
//		{
//			if (NormalizeToken(methodName) == "transversemercator")
//			{
//				return true;
//			}
//		}
//
//		return false;
//	}
////
////	static bool TryParseDouble(const char* text, double& value)
//	{
//		if (text == nullptr || *text == '\0')
//		{
//			return false;
//		}
//
//		char* endPtr = nullptr;
//		value = std::strtod(text, &endPtr);
//		if (endPtr == text)
//		{
//			return false;
//		}
//
//		return IsOnlyTrailingAsciiWhitespace(endPtr) && std::isfinite(value);
//	}
////
////	static bool IsCentralMeridianParameterName(const char* parameterName)
//	{
//		if (parameterName == nullptr)
//		{
//			return false;
//		}
//
//		const std::string normalizedName = NormalizeToken(parameterName);
//		return normalizedName == "centralmeridian" ||
//			normalizedName == "longitudeoforigin" ||
//			normalizedName == "longitudeofnaturalorigin";
//	}
////
////	static bool TryGetCentralMeridianFromTree(const OGR_SRSNode* node, double& centralMeridianDeg)
//	{
//		if (node == nullptr)
//		{
//			return false;
//		}
//
//		if (NormalizeToken(node->GetValue()) == "parameter" && node->GetChildCount() >= 2)
//		{
//			const OGR_SRSNode* parameterNameNode = node->GetChild(0);
//			const OGR_SRSNode* parameterValueNode = node->GetChild(1);
//
//			if (parameterNameNode != nullptr &&
//				parameterValueNode != nullptr &&
//				IsCentralMeridianParameterName(parameterNameNode->GetValue()))
//			{
//				if (TryParseDouble(parameterValueNode->GetValue(), centralMeridianDeg))
//				{
//					return true;
//				}
//			}
//		}
//
//		for (int childIndex = 0; childIndex < node->GetChildCount(); childIndex++)
//		{
//			if (TryGetCentralMeridianFromTree(node->GetChild(childIndex), centralMeridianDeg))
//			{
//				return true;
//			}
//		}
//
//		return false;
//	}
////
////	static bool TryGetCentralMeridian(const OGRSpatialReference& spatialReference, double& centralMeridianDeg)
//	{
//		static const char* const parameterNames[] =
//		{
//			SRS_PP_CENTRAL_MERIDIAN,
//			SRS_PP_LONGITUDE_OF_ORIGIN
//		};
//
//		for (size_t i = 0; i < sizeof(parameterNames) / sizeof(parameterNames[0]); i++)
//		{
//			OGRErr errorCode = OGRERR_FAILURE;
//			const double value = spatialReference.GetNormProjParm(parameterNames[i], 0.0, &errorCode);
//			if (errorCode == OGRERR_NONE && std::isfinite(value))
//			{
//				centralMeridianDeg = value;
//				return true;
//			}
//		}
//
//		return TryGetCentralMeridianFromTree(spatialReference.GetRoot(), centralMeridianDeg);
//	}
////
////	static std::string FormatDouble(double value)
//	{
//		std::ostringstream stream;
//		stream.imbue(std::locale::classic());
//		stream << std::setprecision(15) << value;
//		return stream.str();
//	}
////
////	static std::string FormatLongitudeLabel(double longitudeDeg)
//	{
//		if (!std::isfinite(longitudeDeg))
//		{
//			return std::string();
//		}
//
//		const double absoluteLongitude = std::abs(longitudeDeg);
//		if (absoluteLongitude == 0.0)
//		{
//			return "0";
//		}
//
//		return FormatDouble(absoluteLongitude) + (longitudeDeg > 0.0 ? "E" : "W");
//	}
////
////	static OGR_SRSNode* CreateLeafNode(const std::string& value)
//	{
//		return new OGR_SRSNode(value.c_str());
//	}
////
////	static OGR_SRSNode* CreateSingleStringChildNode(const char* nodeName, const std::string& text)
//	{
//		OGR_SRSNode* node = new OGR_SRSNode(nodeName);
//		node->AddChild(CreateLeafNode(text));
//		return node;
//	}
////
////	static OGR_SRSNode* CreateBboxNode(double westLongitudeDeg, double southLatitudeDeg, double eastLongitudeDeg, double northLatitudeDeg)
//	{
//		OGR_SRSNode* bboxNode = new OGR_SRSNode("BBOX");
//		bboxNode->AddChild(CreateLeafNode(FormatDouble(southLatitudeDeg)));
//		bboxNode->AddChild(CreateLeafNode(FormatDouble(westLongitudeDeg)));
//		bboxNode->AddChild(CreateLeafNode(FormatDouble(northLatitudeDeg)));
//		bboxNode->AddChild(CreateLeafNode(FormatDouble(eastLongitudeDeg)));
//		return bboxNode;
//	}
////
////	static OGR_SRSNode* CreateUsageNode(double westLongitudeDeg, double eastLongitudeDeg)
////	{
////		OGR_SRSNode* usageNode = new OGR_SRSNode("USAGE");
////		usageNode->AddChild(CreateSingleStringChildNode("SCOPE", "Custom area derived from central meridian."));
////		usageNode->AddChild(CreateSingleStringChildNode("AREA", std::string("Between ") + FormatLongitudeLabel(westLongitudeDeg) + " and " + FormatLongitudeLabel(eastLongitudeDeg) + "."));
////		usageNode->AddChild(CreateBboxNode(westLongitudeDeg, -85.0, eastLongitudeDeg, 85.0));
////		return usageNode;
////	}
////
////	static int FindUsageInsertChildIndex(const OGR_SRSNode& rootNode)
//	{
//		for (int childIndex = 0; childIndex < rootNode.GetChildCount(); childIndex++)
//		{
//			const OGR_SRSNode* childNode = rootNode.GetChild(childIndex);
//			if (childNode == nullptr)
//			{
//				continue;
//			}
//
//			const std::string normalizedValue = NormalizeToken(childNode->GetValue());
//			if (normalizedValue == "id" || normalizedValue == "remark")
//			{
//				return childIndex;
//			}
//		}
//
//		return rootNode.GetChildCount();
//	}
////
////	static bool InjectUsageAreaBboxIntoWkt2(std::string& wkt2Text, double westLongitudeDeg, double eastLongitudeDeg)
////	{
////		OGR_SRSNode rootNode;
////		if (!TryImportSrsNode(wkt2Text, rootNode))
////		{
////			return false;
////		}
////
////		const std::string normalizedRootValue = NormalizeToken(rootNode.GetValue());
////		if (normalizedRootValue != "projcrs" &&
////			normalizedRootValue != "projectedcrs" &&
////			normalizedRootValue != "derivedprojcrs")
////		{
////			return false;
////		}
////
////		if (HasExplicitAreaAndBbox(&rootNode))
////		{
////			return true;
////		}
////
////		OGR_SRSNode* usageNode = rootNode.GetNode("USAGE");
////		if (usageNode == nullptr)
////		{
////			usageNode = CreateUsageNode(westLongitudeDeg, eastLongitudeDeg);
////			rootNode.InsertChild(usageNode, FindUsageInsertChildIndex(rootNode));
////		}
////		else
////		{
////			if (usageNode->FindChild("SCOPE") < 0)
////			{
////				usageNode->InsertChild(CreateSingleStringChildNode("SCOPE", "Custom area derived from central meridian."), 0);
////			}
////
////			if (usageNode->FindChild("AREA") < 0)
////			{
////				usageNode->AddChild(CreateSingleStringChildNode("AREA", std::string("Between ") + FormatLongitudeLabel(westLongitudeDeg) + " and " + FormatLongitudeLabel(eastLongitudeDeg) + "."));
////			}
////
////			if (usageNode->FindChild("BBOX") < 0)
////			{
////				usageNode->AddChild(CreateBboxNode(westLongitudeDeg, -85.0, eastLongitudeDeg, 85.0));
////			}
////		}
////
////		char* rawWkt = nullptr;
////		if (rootNode.exportToWkt(&rawWkt) != OGRERR_NONE || rawWkt == nullptr)
////		{
////			return false;
////		}
////
////		std::unique_ptr<char, CplFreeDeleter> wktHolder(rawWkt);
////		wkt2Text.assign(wktHolder.get());
////		return true;
////	}
////}
//
////bool TryExportWkt2WithCustomTransverseMercatorAreaBbox(const std::string& inputWkt, std::string& outputWkt2, bool* areaBboxInjected)
//{
//	outputWkt2.clear();
//
//	if (areaBboxInjected != nullptr)
//	{
//		*areaBboxInjected = false;
//	}
//
//	OGRSpatialReference spatialReference;
//	if (!internal::TryImportSpatialReference(inputWkt, spatialReference))
//	{
//		return false;
//	}
//
//	if (!internal::TryExportToWkt2(spatialReference, outputWkt2))
//	{
//		return false;
//	}
//
//	if (!spatialReference.IsProjected())
//	{
//		return true;
//	}
//
//	if (!internal::IsTransverseMercator(spatialReference))
//	{
//		return true;
//	}
//
//	if (internal::HasExplicitAreaAndBbox(spatialReference.GetRoot()))
//	{
//		return true;
//	}
//
//	double centralMeridianDeg = 0.0;
//	if (!internal::TryGetCentralMeridian(spatialReference, centralMeridianDeg))
//	{
//		return true;
//	}
//
//	const double westLongitudeDeg = centralMeridianDeg - 3.0;
//	const double eastLongitudeDeg = centralMeridianDeg + 3.0;
//
//	if (!internal::InjectUsageAreaBboxIntoWkt2(outputWkt2, westLongitudeDeg, eastLongitudeDeg))
//	{
//		return false;
//	}
//
//	if (areaBboxInjected != nullptr)
//	{
//		*areaBboxInjected = true;
//	}
//
//	return true;
//}
////
////#include "proj.h"
//#include <algorithm>
////
////static inline std::string GetProjErrorMessage(PJ_CONTEXT* context)
//{
//	if (!context)
//	{
//		return "PROJ context is null.";
//	}
//
//	const int errorCode = proj_context_errno(context);
//	const char* errorText = proj_context_errno_string(context, errorCode);
//	if (errorText)
//	{
//		return std::string(errorText);
//	}
//
//	return "Unknown PROJ error.";
//}
////
////struct Bounds2D_
////{
////	double minX = 0;
////	double minY = 0;
////	double maxX = 0;
////	double maxY = 0;
////};
////static inline bool TransformBoundsInternal(PJ_CONTEXT* context, PJ* transform, double minX, double minY, double maxX, double maxY, Bounds2D_& outputBounds, std::string* errorMessage)
//{
//	double outMinX = 0;
//	double outMinY = 0;
//	double outMaxX = 0;
//	double outMaxY = 0;
//
//	const int ok = proj_trans_bounds(context, transform, PJ_FWD, minX, minY, maxX, maxY, &outMinX, &outMinY, &outMaxX, &outMaxY, 21);
//	if (!ok)
//	{
//		if (errorMessage)
//		{
//			*errorMessage = GetProjErrorMessage(context);
//		}
//		return false;
//	}
//
//	outputBounds.minX = outMinX;
//	outputBounds.minY = outMinY;
//	outputBounds.maxX = outMaxX;
//	outputBounds.maxY = outMaxY;
//	return true;
//}
////
////static inline bool GetCrsAreaOfUseDegreesFromWkt(const std::string& wkt, Bounds2D_& areaOfUseBounds, std::string* errorMessage = nullptr)
//{
//	using ContextPtr = std::unique_ptr<PJ_CONTEXT, decltype(&proj_context_destroy)>;
//	using PjPtr = std::unique_ptr<PJ, decltype(&proj_destroy)>;
//
//	ContextPtr context(proj_context_create(), &proj_context_destroy);
//	if (!context)
//	{
//		if (errorMessage)
//		{
//			*errorMessage = "Failed to create PROJ context.";
//		}
//		return false;
//	}
//
//	PROJ_STRING_LIST warnings = nullptr;
//	PROJ_STRING_LIST grammarErrors = nullptr;
//
//	PjPtr crs(proj_create_from_wkt(context.get(), wkt.c_str(), nullptr, &warnings, &grammarErrors), &proj_destroy);
//
//	proj_string_list_destroy(warnings);
//	proj_string_list_destroy(grammarErrors);
//
//	if (!crs)
//	{
//		if (errorMessage)
//		{
//			*errorMessage = GetProjErrorMessage(context.get());
//		}
//		return false;
//	}
//
//	if (!proj_is_crs(crs.get()))
//	{
//		if (errorMessage)
//		{
//			*errorMessage = "The WKT does not describe a CRS object.";
//		}
//		return false;
//	}
//
//	const char* areaName = nullptr;
//	const int ok = proj_get_area_of_use(context.get(), crs.get(), &areaOfUseBounds.minX, &areaOfUseBounds.minY, &areaOfUseBounds.maxX, &areaOfUseBounds.maxY, &areaName);
//
//	if (!ok)
//	{
//		if (errorMessage)
//		{
//			*errorMessage = "Area of use is unknown for this WKT/CRS.";
//		}
//		return false;
//	}
//
//	return true;
//}
////
////static inline bool GetCrsEnvelopeInOwnUnitsFromWkt(const std::string& wkt, Bounds2D_& envelopeBounds, std::string* errorMessage = nullptr)
//{
//	using ContextPtr = std::unique_ptr<PJ_CONTEXT, decltype(&proj_context_destroy)>;
//	using PjPtr = std::unique_ptr<PJ, decltype(&proj_destroy)>;
//
//	ContextPtr context(proj_context_create(), &proj_context_destroy);
//	if (!context)
//	{
//		if (errorMessage)
//		{
//			*errorMessage = "Failed to create PROJ context.";
//		}
//		return false;
//	}
//
//	PROJ_STRING_LIST warnings = nullptr;
//	PROJ_STRING_LIST grammarErrors = nullptr;
//
//	PjPtr crs(proj_create_from_wkt(context.get(), wkt.c_str(), nullptr, &warnings, &grammarErrors), &proj_destroy);
//
//	proj_string_list_destroy(warnings);
//	proj_string_list_destroy(grammarErrors);
//
//	if (!crs)
//	{
//		if (errorMessage)
//		{
//			*errorMessage = GetProjErrorMessage(context.get());
//		}
//		return false;
//	}
//
//	if (!proj_is_crs(crs.get()))
//	{
//		if (errorMessage)
//		{
//			*errorMessage = "The WKT does not describe a CRS object.";
//		}
//		return false;
//	}
//
//	double westLon = 0;
//	double southLat = 0;
//	double eastLon = 0;
//	double northLat = 0;
//	const char* areaName = nullptr;
//
//	const int gotArea = proj_get_area_of_use(context.get(), crs.get(), &westLon, &southLat, &eastLon, &northLat, &areaName);
//
//	if (!gotArea)
//	{
//		if (errorMessage)
//		{
//			*errorMessage = "Area of use is unknown for this WKT/CRS, so projected bounds cannot be inferred reliably.";
//		}
//		return false;
//	}
//
//	PjPtr geodeticCrs(proj_crs_get_geodetic_crs(context.get(), crs.get()), &proj_destroy);
//	if (!geodeticCrs)
//	{
//		if (errorMessage)
//		{
//			*errorMessage = GetProjErrorMessage(context.get());
//		}
//		return false;
//	}
//
//	PjPtr transform(proj_create_crs_to_crs_from_pj(context.get(), geodeticCrs.get(), crs.get(), nullptr, nullptr), &proj_destroy);
//
//	if (!transform)
//	{
//		if (errorMessage)
//		{
//			*errorMessage = GetProjErrorMessage(context.get());
//		}
//		return false;
//	}
//
//	PjPtr normalizedTransform(proj_normalize_for_visualization(context.get(), transform.get()), &proj_destroy);
//
//	PJ* transformToUse = normalizedTransform ? normalizedTransform.get() : transform.get();
//
//	// 常见情况：westLon <= eastLon
//	if (westLon <= eastLon)
//	{
//		return TransformBoundsInternal(context.get(), transformToUse, westLon, southLat, eastLon, northLat, envelopeBounds, errorMessage);
//	}
//
//	// 保守处理：若 area of use 跨越反经线，则拆成两个范围分别投影后再合并
//	Bounds2D_ firstPartBounds;
//	Bounds2D_ secondPartBounds;
//
//	if (!TransformBoundsInternal(context.get(), transformToUse, westLon, southLat, 180, northLat, firstPartBounds, errorMessage))
//	{
//		return false;
//	}
//
//	if (!TransformBoundsInternal(context.get(), transformToUse, -180, southLat, eastLon, northLat, secondPartBounds, errorMessage))
//	{
//		return false;
//	}
//
//	envelopeBounds.minX = std::min(firstPartBounds.minX, secondPartBounds.minX);
//	envelopeBounds.minY = std::min(firstPartBounds.minY, secondPartBounds.minY);
//	envelopeBounds.maxX = std::max(firstPartBounds.maxX, secondPartBounds.maxX);
//	envelopeBounds.maxY = std::max(firstPartBounds.maxY, secondPartBounds.maxY);
//	return true;
//}
////
//////bool GetCartesianExtents(const std::string& wkt, double& minX, double& minY, double& maxX, double& maxY)
//////{
//////	Bounds2D_ envelopeBounds;
//////	if (!GetCrsEnvelopeInOwnUnitsFromWkt(wkt, envelopeBounds))
//////	{
//////		return false;
//////	}
//////	minX = envelopeBounds.minX;
//////	minY = envelopeBounds.minY;
//////	maxX = envelopeBounds.maxX;
//////	maxY = envelopeBounds.maxY;
//////	return true;
//////}
////
////namespace internal2
////{
////	static constexpr double kUnknownAreaOfUseMarker = -1000.0;
////	static constexpr double kAreaOfUseTolerance = 1e-12;
////
////	static inline bool IsFiniteNumber(double value)
////	{
////		return std::isfinite(value) != 0;
////	}
////
////	static inline bool IsValidAreaOfUse(double westLongitudeDeg, double southLatitudeDeg, double eastLongitudeDeg, double northLatitudeDeg)
////	{
////		// GDAL may return success while values are -1000 (unknown area marker).
////		if (std::fabs(westLongitudeDeg - kUnknownAreaOfUseMarker) < kAreaOfUseTolerance ||
////			std::fabs(southLatitudeDeg - kUnknownAreaOfUseMarker) < kAreaOfUseTolerance ||
////			std::fabs(eastLongitudeDeg - kUnknownAreaOfUseMarker) < kAreaOfUseTolerance ||
////			std::fabs(northLatitudeDeg - kUnknownAreaOfUseMarker) < kAreaOfUseTolerance)
////		{
////			return false;
////		}
////
////		if (!IsFiniteNumber(westLongitudeDeg) ||
////			!IsFiniteNumber(southLatitudeDeg) ||
////			!IsFiniteNumber(eastLongitudeDeg) ||
////			!IsFiniteNumber(northLatitudeDeg))
////		{
////			return false;
////		}
////
////		if (southLatitudeDeg > northLatitudeDeg)
////		{
////			return false;
////		}
////
////		// Longitude can cross anti-meridian, so only validate each endpoint range.
////		if (westLongitudeDeg < -180.0 || westLongitudeDeg > 180.0 ||
////			eastLongitudeDeg < -180.0 || eastLongitudeDeg > 180.0)
////		{
////			return false;
////		}
////
////		if (southLatitudeDeg < -90.0 || southLatitudeDeg > 90.0 ||
////			northLatitudeDeg < -90.0 || northLatitudeDeg > 90.0)
////		{
////			return false;
////		}
////
////		return true;
////	}
////
////	static inline std::string ToLowerAscii(const std::string& text)
////	{
////		std::string lowerText = text;
////		std::transform(lowerText.begin(), lowerText.end(), lowerText.begin(), [](unsigned char character) -> char {
////			return static_cast<char>(std::tolower(character));
////			});
////		return lowerText;
////	}
////
////	static inline bool EqualsIgnoreCaseAscii(const char* leftText, const char* rightText)
////	{
////		if (!leftText || !rightText)
////		{
////			return false;
////		}
////
////		return ToLowerAscii(leftText) == ToLowerAscii(rightText);
////	}
////
////	static inline bool ContainsIgnoreCaseAscii(const char* text, const char* pattern)
////	{
////		if (!text || !pattern)
////		{
////			return false;
////		}
////
////		const std::string lowerText = ToLowerAscii(text);
////		const std::string lowerPattern = ToLowerAscii(pattern);
////		return lowerText.find(lowerPattern) != std::string::npos;
////	}
////
////	static inline std::string FormatDouble(double value)
////	{
////		std::ostringstream stream;
////		stream.imbue(std::locale::classic());
////		stream << std::setprecision(15) << value;
////		return stream.str();
////	}
////
////	static inline double ClampLatitude(double latitudeDeg)
////	{
////		return std::max(-90.0, std::min(90.0, latitudeDeg));
////	}
////
////	static inline double ClampLongitude(double longitudeDeg)
////	{
////		return std::max(-180.0, std::min(180.0, longitudeDeg));
////	}
////
////	static double NormalizeLongitude(double longitudeDeg)
////	{
////		// 为了处理例如 190、-190 这样的输入：把经度规整到 [-180, 180] 区间
////		while (longitudeDeg < -180)
////		{
////			longitudeDeg += 360;
////		}
////
////		while (longitudeDeg > 180)
////		{
////			longitudeDeg -= 360;
////		}
////
////		return longitudeDeg;
////	}
////
////	static inline bool TryGetNormProjParm(const OGRSpatialReference& spatialReference, const char* parameterName, double& parameterValue)
////	{
////		OGRErr error = OGRERR_FAILURE;
////		const double value = spatialReference.GetNormProjParm(parameterName, 0, &error);
////		if (error != OGRERR_NONE)
////		{
////			return false;
////		}
////
////		parameterValue = value;
////		return true;
////	}
////
////	static bool TryGetFirstExistingNormProjParm(const OGRSpatialReference& spatialReference, const std::vector<const char*>& parameterNames, double& parameterValue)
////	{
////		// 按候选参数名顺序依次尝试读取：兼容不同投影定义中“中央经线”可能使用的不同参数名，
////		// 例如 central_meridian / longitude_of_center / longitude_of_origin。
////		for (size_t i = 0; i < parameterNames.size(); i++)
////		{
////			if (TryGetNormProjParm(spatialReference, parameterNames[i], parameterValue))
////			{
////				return true;
////			}
////		}
////
////		return false;
////	}
////
////	static inline bool IsTransverseMercatorLike(const OGRSpatialReference& spatialReference)
////	{
////		// 第一优先级：直接用 GDAL 的 UTM 识别接口。如果能识别出 UTM 分带，那么它本质上就是 TM 家族。
////		int isNorth = FALSE;
////		if (spatialReference.GetUTMZone(&isNorth) != 0)
////		{
////			return true;
////		}
////
////		// 第二优先级：读取 PROJECTION 节点名称。
////		const char* projectionName = spatialReference.GetAttrValue("PROJECTION");
////		const char* methodName = spatialReference.GetAttrValue("METHOD");
////		const char* candidateName = projectionName ? projectionName : methodName;
////		if (!candidateName)
////		{
////			return false;
////		}
////
////		// 先用 GDAL 预定义宏做“精确匹配”。
////		if (EqualsIgnoreCaseAscii(candidateName, SRS_PT_TRANSVERSE_MERCATOR) ||
////			EqualsIgnoreCaseAscii(candidateName, SRS_PT_TRANSVERSE_MERCATOR_SOUTH_ORIENTED))
////		{
////			return true;
////		}
////
////		// 再做一次更宽松的“包含”判断，兼容部分非标准命名。
////		if (ContainsIgnoreCaseAscii(candidateName, "transverse_mercator") ||
////			ContainsIgnoreCaseAscii(candidateName, "transverse mercator"))
////		{
////			return true;
////		}
////
////		return false;
////	}
////
////	static inline bool IsLambertConformalConicLike(const OGRSpatialReference& spatialReference)
////	{
////		// 读取投影名。
////		const char* projectionName = spatialReference.GetAttrValue("PROJECTION");
////		const char* methodName = spatialReference.GetAttrValue("METHOD");
////		const char* candidateName = projectionName ? projectionName : methodName;
////		if (!candidateName)
////		{
////			return false;
////		}
////
////		// 先用标准宏匹配常见 LCC 变体。
////		if (EqualsIgnoreCaseAscii(candidateName, SRS_PT_LAMBERT_CONFORMAL_CONIC_1SP) ||
////			EqualsIgnoreCaseAscii(candidateName, SRS_PT_LAMBERT_CONFORMAL_CONIC_2SP) ||
////			EqualsIgnoreCaseAscii(candidateName, SRS_PT_LAMBERT_CONFORMAL_CONIC_2SP_BELGIUM))
////		{
////			return true;
////		}
////
////		// 再兼容更宽松的字符串写法。
////		if (ContainsIgnoreCaseAscii(candidateName, "lambert_conformal_conic") ||
////			ContainsIgnoreCaseAscii(candidateName, "lambert conformal conic"))
////		{
////			return true;
////		}
////
////		return false;
////	}
////
////	static inline OGR_SRSNode* CreateLeafNode(const std::string& value)
////	{
////		return new OGR_SRSNode(value.c_str());
////	}
////
////	static inline OGR_SRSNode* CreateUsageNode(const std::string& areaName, double westLongitudeDeg, double southLatitudeDeg, double eastLongitudeDeg, double northLatitudeDeg)
////	{
////		// 构造如下 WKT2 结构：
////		// USAGE[
////		//   SCOPE["Approximate area of use"],
////		//   AREA["..."],
////		//   BBOX[south, west, north, east]
////		// ]
////		//
////		// 这里显式使用 BBOX 的 south / west / north / east 顺序，避免与常见的 xmin / ymin / xmax / ymax 习惯混淆。
////		OGR_SRSNode* const usageNode = new OGR_SRSNode("USAGE");
////
////		OGR_SRSNode* const scopeNode = new OGR_SRSNode("SCOPE");
////		scopeNode->AddChild(CreateLeafNode("Approximate area of use"));
////		usageNode->AddChild(scopeNode);
////
////		OGR_SRSNode* const areaNode = new OGR_SRSNode("AREA");
////		areaNode->AddChild(CreateLeafNode(areaName));
////		usageNode->AddChild(areaNode);
////
////		OGR_SRSNode* const bboxNode = new OGR_SRSNode("BBOX");
////		bboxNode->AddChild(CreateLeafNode(FormatDouble(southLatitudeDeg)));
////		bboxNode->AddChild(CreateLeafNode(FormatDouble(westLongitudeDeg)));
////		bboxNode->AddChild(CreateLeafNode(FormatDouble(northLatitudeDeg)));
////		bboxNode->AddChild(CreateLeafNode(FormatDouble(eastLongitudeDeg)));
////		usageNode->AddChild(bboxNode);
////
////		return usageNode;
////	}
////
////	static inline bool TryComputeAreaOfUseByRule(const OGRSpatialReference& spatialReference, double& westLongitudeDeg, double& southLatitudeDeg, double& eastLongitudeDeg, double& northLatitudeDeg, std::string& areaName)
////	{
////		// 只处理投影坐标系。
////		if (!spatialReference.IsProjected() && !spatialReference.IsDerivedProjected())
////		{
////			return false;
////		}
////
////		// 先尝试识别是否为 UTM。
////		int isNorth = FALSE;
////		const int utmZone = spatialReference.GetUTMZone(&isNorth);
////
////		// 规则（1）：
////		// 横轴墨卡托或 UTM：
////		//   经度范围 = 中央经线 ± 3°
////		//   纬度范围 = [-60°, 60°]
////		if (utmZone != 0 || IsTransverseMercatorLike(spatialReference))
////		{
////			double centralMeridianDeg = 0;
////
////			// 中央经线可能存储在不同参数名下，依次尝试。
////			const static std::vector<const char*> centralMeridianParameterNames =
////			{
////				SRS_PP_CENTRAL_MERIDIAN,
////				SRS_PP_LONGITUDE_OF_CENTER,
////				SRS_PP_LONGITUDE_OF_ORIGIN
////			};
////			const bool hasCentralMeridian = TryGetFirstExistingNormProjParm(spatialReference, centralMeridianParameterNames, centralMeridianDeg);
////
////			// 如果不是直接从参数中取到，但又已经识别为 UTM，
////			// 那么可以按照 UTM 分带规则反算中央经线：
////			// zone 1 -> -177°, zone 2 -> -171° ... zone 60 -> 177°
////			if (!hasCentralMeridian)
////			{
////				const int absUtmZone = std::abs(utmZone);
////				if (absUtmZone < 1 || absUtmZone > 60)
////				{
////					return false;
////				}
////
////				centralMeridianDeg = absUtmZone * 6.0 - 183.0;
////			}
////
////			const double normalizedCentralMeridianDeg = NormalizeLongitude(centralMeridianDeg);
////
////			westLongitudeDeg = ClampLongitude(normalizedCentralMeridianDeg - 3);
////			eastLongitudeDeg = ClampLongitude(normalizedCentralMeridianDeg + 3);
////			southLatitudeDeg = -60;
////			northLatitudeDeg = 60;
////			areaName = "Heuristic area of use for Transverse Mercator / UTM";
////			return true;
////		}
////
////		// 规则（2）：
////		// Lambert Conformal Conic:
////		//   双标准纬线：纬度总跨度 = |stdP2 - stdP1| * 1.1，以两条标准纬线中点为中心展开
////		//   单标准纬线：纬度范围 = 标准纬线 ± 3°
////		//   经度范围统一 = 中央经线 ± 30°
////		if (IsLambertConformalConicLike(spatialReference))
////		{
////			double centralMeridianDeg = 0;
////			const static std::vector<const char*> centralMeridianParameterNames =
////			{
////				SRS_PP_CENTRAL_MERIDIAN,
////				SRS_PP_LONGITUDE_OF_CENTER,
////				SRS_PP_LONGITUDE_OF_ORIGIN
////			};
////			const bool hasCentralMeridian = TryGetFirstExistingNormProjParm(spatialReference, centralMeridianParameterNames, centralMeridianDeg);
////
////			// LCC 没有中央经线则无法计算经度范围。
////			if (!hasCentralMeridian)
////			{
////				return false;
////			}
////
////			double standardParallel1Deg = 0;
////			double standardParallel2Deg = 0;
////			const bool hasStandardParallel1 = TryGetNormProjParm(spatialReference, SRS_PP_STANDARD_PARALLEL_1, standardParallel1Deg);
////			const bool hasStandardParallel2 = TryGetNormProjParm(spatialReference, SRS_PP_STANDARD_PARALLEL_2, standardParallel2Deg);
////
////			// 情况 A：双标准纬线。
////			if (hasStandardParallel1 && hasStandardParallel2)
////			{
////				const double latitudeDiffDeg = std::fabs(standardParallel2Deg - standardParallel1Deg);
////
////				// 如果两条标准纬线数值上几乎相同，
////				// 那么退化为单标准纬线情况，避免出现 0 跨度。
////				if (latitudeDiffDeg < 1e-12)
////				{
////					southLatitudeDeg = ClampLatitude(standardParallel1Deg - 3);
////					northLatitudeDeg = ClampLatitude(standardParallel1Deg + 3);
////				}
////				else
////				{
////					// 1) 先取两条标准纬线的中点作为中心；
////					// 2) 总纬度跨度 = 纬度差 * 1.1；
////					// 3) 向上下各展开一半。
////					const double centerLatitudeDeg = (standardParallel1Deg + standardParallel2Deg) * 0.5;
////					const double latitudeSpanDeg = latitudeDiffDeg * 1.1;
////					const double halfLatitudeSpanDeg = latitudeSpanDeg * 0.5;
////
////					southLatitudeDeg = ClampLatitude(centerLatitudeDeg - halfLatitudeSpanDeg);
////					northLatitudeDeg = ClampLatitude(centerLatitudeDeg + halfLatitudeSpanDeg);
////				}
////			}
////			else
////			{
////				// 情况 B：单标准纬线。
////				// 优先取 standard_parallel_1；若没有，再看 standard_parallel_2。
////				// 某些 1SP 定义里可能没有显式标准纬线，这时退回 latitude_of_origin。
////				double singleStandardParallelDeg = 0;
////				if (hasStandardParallel1)
////				{
////					singleStandardParallelDeg = standardParallel1Deg;
////				}
////				else if (hasStandardParallel2)
////				{
////					singleStandardParallelDeg = standardParallel2Deg;
////				}
////				else
////				{
////					if (!TryGetNormProjParm(spatialReference, SRS_PP_LATITUDE_OF_ORIGIN, singleStandardParallelDeg))
////					{
////						return false;
////					}
////				}
////
////				southLatitudeDeg = ClampLatitude(singleStandardParallelDeg - 3);
////				northLatitudeDeg = ClampLatitude(singleStandardParallelDeg + 3);
////			}
////
////			const double normalizedCentralMeridianDeg = NormalizeLongitude(centralMeridianDeg);
////			westLongitudeDeg = ClampLongitude(normalizedCentralMeridianDeg - 30);
////			eastLongitudeDeg = ClampLongitude(normalizedCentralMeridianDeg + 30);
////			areaName = "Heuristic area of use for Lambert Conformal Conic";
////			return true;
////		}
////
////		// 规则（3）：其它投影不处理。
////		return false;
////	}
////
////	static bool ApplyUsageNodeToSpatialReference(OGRSpatialReference& spatialReference, double westLongitudeDeg, double southLatitudeDeg, double eastLongitudeDeg, double northLatitudeDeg, const std::string& areaName)
////	{
////		//  1) 先导出成 WKT2；
////		//  2) 再导入到一个临时 SRS；
////		//  3) 在临时 SRS 的根节点上插入 USAGE 节点；
////		//  4) 再导出并重新导回原对象。
////		//
////		// 好处：
////		//   - 能确保修改的是 WKT2 结构；
////		//   - 避免对原对象做半完成状态的直接修改；
////		//   - 更便于统一验证和失败回滚。
////
////		const char* exportOptions[] =
////		{
////			"FORMAT=WKT2_2019",
////			nullptr
////		};
////
////		char* wkt2Text = nullptr;
////		if (spatialReference.exportToWkt(&wkt2Text, exportOptions) != OGRERR_NONE || wkt2Text == nullptr)
////		{
////			if (wkt2Text)
////			{
////				CPLFree(wkt2Text);
////			}
////			return false;
////		}
////
////		OGRSpatialReference wkt2SpatialReference;
////		const std::string wkt2String = wkt2Text;
////		CPLFree(wkt2Text);
////		wkt2Text = nullptr;
////
////		// 用导出的 WKT2 构造一个临时 SRS。
////		if (wkt2SpatialReference.importFromWkt(wkt2String.c_str()) != OGRERR_NONE)
////		{
////			return false;
////		}
////
////		OGR_SRSNode* const rootNode = wkt2SpatialReference.GetRoot();
////		if (!rootNode)
////		{
////			return false;
////		}
////
////		// 先把已有的 USAGE 节点全部删掉。避免重复挂载多个 USAGE，或者保留旧的错误范围。
////		for (;;)
////		{
////			const int usageIndex = rootNode->FindChild("USAGE");
////			if (usageIndex < 0)
////			{
////				break;
////			}
////
////			rootNode->DestroyChild(usageIndex);
////		}
////
////		OGR_SRSNode* const usageNode = CreateUsageNode(areaName, westLongitudeDeg, southLatitudeDeg, eastLongitudeDeg, northLatitudeDeg);
////
////		// 为了保持 WKT 结构的整洁，如果根节点已有 ID 节点，就把 USAGE 插到 ID 前面；否则直接追加到末尾。
////		const int idIndex = rootNode->FindChild("ID");
////		if (idIndex >= 0)
////		{
////			rootNode->InsertChild(usageNode, idIndex);
////		}
////		else
////		{
////			rootNode->AddChild(usageNode);
////		}
////
////		char* updatedWkt2Text = nullptr;
////		if (wkt2SpatialReference.exportToWkt(&updatedWkt2Text, exportOptions) != OGRERR_NONE || updatedWkt2Text == nullptr)
////		{
////			if (updatedWkt2Text)
////			{
////				CPLFree(updatedWkt2Text);
////			}
////			return false;
////		}
////
////		// importFromWkt() 可能会重建内部对象，因此先记住当前对象的轴映射策略与数据轴映射关系，后面再恢复，避免破坏调用方已有设置。
////		const OSRAxisMappingStrategy axisMappingStrategy = spatialReference.GetAxisMappingStrategy();
////		const std::vector<int> dataAxisToSrsAxisMapping = spatialReference.GetDataAxisToSRSAxisMapping();
////
////		const std::string updatedWkt2String = updatedWkt2Text;
////		CPLFree(updatedWkt2Text);
////		updatedWkt2Text = nullptr;
////
////		// 把加好 USAGE 的 WKT2 重新导回原对象。
////		if (spatialReference.importFromWkt(updatedWkt2String.c_str()) != OGRERR_NONE)
////		{
////			return false;
////		}
////
////		// 恢复轴映射设置。
////		spatialReference.SetAxisMappingStrategy(axisMappingStrategy);
////		if (!dataAxisToSrsAxisMapping.empty())
////		{
////			spatialReference.SetDataAxisToSRSAxisMapping(dataAxisToSrsAxisMapping);
////		}
////
////		// 最后再调用一次 GetAreaOfUse() 做结果验证：只有当 GDAL 能够正确从修改后的 CRS 中读回 area-of-use，才认为本次写入真正成功。
////		double verifyWestLongitudeDeg = 0;
////		double verifySouthLatitudeDeg = 0;
////		double verifyEastLongitudeDeg = 0;
////		double verifyNorthLatitudeDeg = 0;
////		const char* verifyAreaName = nullptr;
////
////		return spatialReference.GetAreaOfUse(&verifyWestLongitudeDeg, &verifySouthLatitudeDeg, &verifyEastLongitudeDeg, &verifyNorthLatitudeDeg, &verifyAreaName) &&
////			IsValidAreaOfUse(verifyWestLongitudeDeg, verifySouthLatitudeDeg, verifyEastLongitudeDeg, verifyNorthLatitudeDeg);
////	}
////} // namespace
////
////bool AddExtraAreaInfo(OGRSpatialReference& spatialReference)
////{
////	// 如果对象本身已经带有合法的 area-of-use，就直接返回 true，不重复覆盖。
////	double existingWestLongitudeDeg = 0;
////	double existingSouthLatitudeDeg = 0;
////	double existingEastLongitudeDeg = 0;
////	double existingNorthLatitudeDeg = 0;
////	const char* existingAreaName = nullptr;
////
////	if (spatialReference.GetAreaOfUse(&existingWestLongitudeDeg, &existingSouthLatitudeDeg, &existingEastLongitudeDeg, &existingNorthLatitudeDeg, &existingAreaName) &&
////		internal2::IsValidAreaOfUse(existingWestLongitudeDeg, existingSouthLatitudeDeg, existingEastLongitudeDeg, existingNorthLatitudeDeg))
////	{
////		return true;
////	}
////
////	// 尝试推导一个“额外补充”的 area-of-use。
////	double westLongitudeDeg = 0;
////	double southLatitudeDeg = 0;
////	double eastLongitudeDeg = 0;
////	double northLatitudeDeg = 0;
////	std::string areaName = "";
////	if (!internal2::TryComputeAreaOfUseByRule(spatialReference, westLongitudeDeg, southLatitudeDeg, eastLongitudeDeg, northLatitudeDeg, areaName))
////	{
////		// 不属于指定投影类型，或缺少必要参数时，返回 false。
////		return false;
////	}
////
////	// 计算成功后，将 USAGE/AREA/BBOX 写回到 spatialReference。
////	return internal2::ApplyUsageNodeToSpatialReference(spatialReference, westLongitudeDeg, southLatitudeDeg, eastLongitudeDeg, northLatitudeDeg, areaName);
////}
////
////
////bool GetCartesianExtents(const std::string& wkt, double& minX, double& minY, double& maxX, double& maxY)
////{
////	OGRSpatialReference spatialReference;
////	if (spatialReference.importFromWkt(wkt.c_str()) != OGRERR_NONE)
////	{
////		return false;
////	}
////
////	const char* existingAreaName = nullptr;
////	if (spatialReference.GetAreaOfUse(&minX, &minY, &maxX, &maxY, &existingAreaName))
////	{
////		return true;
////	}
////
////	if (!AddExtraAreaInfo(spatialReference))
////	{
////		return false;
////	}
////
////	return spatialReference.GetAreaOfUse(&minX, &minY, &maxX, &maxY, &existingAreaName);
////}