#include "GeoBoundingBox.h"

#include "GeoCrsManager.h"
#include "GeoCrs.h"
#include "GB_Crypto.h"
#include "GB_IO.h"
#include "GB_Utf8String.h"
#include "GB_Logger.h"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <limits>
#include <string>
#include <vector>
#include <iomanip>
#include <locale>
#include <sstream>

namespace
{
	constexpr uint16_t kGeoBoundingBoxBinaryVersion = 1;
	constexpr uint32_t kGeoBoundingBoxBinaryTag = 0x58424F47u; // 'GOBX' (little-endian bytes: 47 4F 42 58)

	static bool StartsWith(const std::string& text, const char* prefix)
	{
		if (prefix == nullptr)
		{
			return false;
		}

		const size_t prefixLen = std::char_traits<char>::length(prefix);
		if (text.size() < prefixLen)
		{
			return false;
		}

		return text.compare(0, prefixLen, prefix) == 0;
	}

	static double ClampDouble(double value, double minValue, double maxValue)
	{
		return std::max(minValue, std::min(maxValue, value));
	}

	static bool IsFinite(double value)
	{
		return std::isfinite(value);
	}

	static bool IsFiniteRectangle(const GB_Rectangle& rect)
	{
		return IsFinite(rect.minX) && IsFinite(rect.minY) && IsFinite(rect.maxX) && IsFinite(rect.maxY);
	}

	static void NormalizeRectangleValues(double& minX, double& minY, double& maxX, double& maxY)
	{
		if (minX > maxX)
		{
			std::swap(minX, maxX);
		}
		if (minY > maxY)
		{
			std::swap(minY, maxY);
		}
	}

	static GB_ByteBuffer ToByteBuffer(const std::string& bytes)
	{
		GB_ByteBuffer buffer;
		buffer.reserve(bytes.size());
		buffer.insert(buffer.end(), bytes.begin(), bytes.end());
		return buffer;
	}

	static inline std::string DoubleToStableString(double value)
	{
		// 约定：使用类似 "%.15g" 的表达以尽量保持精度与可读性。
		// std::stod 能解析 "nan" / "inf" / "-inf"。
		if (std::isnan(value))
		{
			return "nan";
		}
		if (std::isinf(value))
		{
			return value > 0 ? "inf" : "-inf";
		}

		std::ostringstream oss;
		oss.imbue(std::locale::classic());
		oss << std::setprecision(15) << value;
		return oss.str();
	}

	static bool TryParseDouble(const std::string& text, double& outValue)
	{
		const std::string trimmed = GB_Utf8Trim(text);
		if (trimmed.empty())
		{
			return false;
		}

		try
		{
			size_t idx = 0;
			outValue = std::stod(trimmed, &idx);
			// 允许末尾空白，但不允许非空白垃圾。
			for (; idx < trimmed.size(); idx++)
			{
				const char ch = trimmed[idx];
				if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n')
				{
					return false;
				}
			}
			return true;
		}
		catch (...)
		{
			return false;
		}
	}

	static bool TryParseGeoBoundingBoxText(const std::string& text, std::string& outWktField, GB_Rectangle& outRect)
	{
		// 目标格式：
		// {GeoBoundingBox: wkt=EPSG:4326;rect={-110,-30,180,90}}
		// {GeoBoundingBox: wkt=<raw_wkt>;rect={minX,minY,maxX,maxY}}
		const std::string trimmed = GB_Utf8Trim(text);
		if (!GB_Utf8StartsWith(trimmed, "{GeoBoundingBox:", true))
		{
			return false;
		}

		const size_t wktPos = trimmed.find("wkt=");
		if (wktPos == std::string::npos)
		{
			return false;
		}

		const size_t rectPos = trimmed.find(";rect=", wktPos + 4);
		if (rectPos == std::string::npos)
		{
			return false;
		}

		outWktField = GB_Utf8Trim(trimmed.substr(wktPos + 4, rectPos - (wktPos + 4)));

		const size_t braceOpen = trimmed.find('{', rectPos);
		if (braceOpen == std::string::npos)
		{
			return false;
		}

		const size_t braceClose = trimmed.find('}', braceOpen + 1);
		if (braceClose == std::string::npos || braceClose <= braceOpen + 1)
		{
			return false;
		}

		const std::string inside = trimmed.substr(braceOpen + 1, braceClose - braceOpen - 1);
		const std::vector<std::string> parts = GB_Utf8Split(inside, GB_CHAR(','));
		if (parts.size() != 4)
		{
			return false;
		}

		double minX = GB_QuietNan;
		double minY = GB_QuietNan;
		double maxX = GB_QuietNan;
		double maxY = GB_QuietNan;
		if (!TryParseDouble(parts[0], minX) ||
			!TryParseDouble(parts[1], minY) ||
			!TryParseDouble(parts[2], maxX) ||
			!TryParseDouble(parts[3], maxY))
		{
			return false;
		}

		outRect.Set(minX, minY, maxX, maxY);
		return true;
	}

	static int TryParseEpsgCodeFromString(const std::string& text)
	{
		const std::string trimmed = GB_Utf8Trim(text);
		if (trimmed.size() < 6)
		{
			return 0;
		}

		// 支持 "EPSG:4326"（大小写不敏感）
		std::string prefix = trimmed.substr(0, 5);
		prefix = GB_Utf8ToUpper(prefix);
		if (prefix != "EPSG:")
		{
			return 0;
		}

		try
		{
			const std::string numberText = GB_Utf8Trim(trimmed.substr(5));
			if (numberText.empty())
			{
				return 0;
			}

			size_t idx = 0;
			const int epsgCode = std::stoi(numberText, &idx);
			if (epsgCode <= 0)
			{
				return 0;
			}

			for (; idx < numberText.size(); idx++)
			{
				const char ch = numberText[idx];
				if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n')
				{
					return 0;
				}
			}

			return epsgCode;
		}
		catch (...)
		{
			return 0;
		}
	}
}

const GeoBoundingBox GeoBoundingBox::Invalid = GeoBoundingBox();

GeoBoundingBox::GeoBoundingBox()
{
	Reset();
}

GeoBoundingBox::GeoBoundingBox(const std::string& wktUtf8)
{
	Reset();
	this->wktUtf8 = wktUtf8;
}

GeoBoundingBox::GeoBoundingBox(const std::string& wktUtf8, const GB_Rectangle& rect)
{
	Reset();
	Set(wktUtf8, rect);
}

GeoBoundingBox::~GeoBoundingBox() = default;

bool GeoBoundingBox::operator==(const GeoBoundingBox& other) const
{
	if (wktUtf8 == other.wktUtf8)
	{
		return (rect == other.rect);
	}

	if (rect != other.rect)
	{
		return false;
	}

	std::shared_ptr<const GeoCrs> thisCrs = GeoCrsManager::GetFromWktCached(wktUtf8);
	std::shared_ptr<const GeoCrs> otherCrs = GeoCrsManager::GetFromWktCached(other.wktUtf8);
	if (thisCrs == nullptr || otherCrs == nullptr)
	{
		return false;
	}

	return *thisCrs == *otherCrs;
}

bool GeoBoundingBox::operator!=(const GeoBoundingBox& other) const
{
	return !(*this == other);
}

bool GeoBoundingBox::IsValid() const
{
	if (!rect.IsValid())
	{
		return false;
	}

	return GeoCrsManager::IsWktValidCached(wktUtf8);
}

void GeoBoundingBox::Reset()
{
	wktUtf8.clear();
	rect.Reset();
}

void GeoBoundingBox::Set(const std::string& wktUtf8, const GB_Rectangle& rect)
{
	this->wktUtf8 = wktUtf8;
	this->rect = rect;
	this->rect.Normalize();
}

std::string GeoBoundingBox::SerializeToString() const
{
	const std::string trimmedWkt = GB_Utf8Trim(wktUtf8);
	std::string wktField = trimmedWkt;

	if (!trimmedWkt.empty() && GeoCrsManager::IsWktValidCached(trimmedWkt))
	{
		const std::shared_ptr<const GeoCrs> crs = GeoCrsManager::GetFromWktCached(trimmedWkt);
		if (crs && crs->IsValid())
		{
			const int epsgCode = crs->TryGetEpsgCode(false, false, 0);
			if (epsgCode > 0)
			{
				wktField = "EPSG:" + std::to_string(epsgCode);
			}
		}
	}

	std::ostringstream oss;
	oss.imbue(std::locale::classic());
	oss << "{GeoBoundingBox: wkt=" << wktField
		<< ";rect={"
		<< DoubleToStableString(rect.minX) << ","
		<< DoubleToStableString(rect.minY) << ","
		<< DoubleToStableString(rect.maxX) << ","
		<< DoubleToStableString(rect.maxY)
		<< "}}";
	return oss.str();
}

GB_ByteBuffer GeoBoundingBox::SerializeToBinary() const
{
	GB_ByteBuffer buffer;
	buffer.reserve(32 + wktUtf8.size());

	GB_ByteBufferIO::AppendUInt32LE(buffer, GB_ClassMagicNumber);
	GB_ByteBufferIO::AppendUInt32LE(buffer, kGeoBoundingBoxBinaryTag);
	GB_ByteBufferIO::AppendUInt16LE(buffer, kGeoBoundingBoxBinaryVersion);
	GB_ByteBufferIO::AppendUInt16LE(buffer, 0);

	const std::string trimmedWkt = GB_Utf8Trim(wktUtf8);
	const size_t wktSize = trimmedWkt.size();
	const uint32_t wktSizeU32 = (wktSize <= static_cast<size_t>(std::numeric_limits<uint32_t>::max()))
		? static_cast<uint32_t>(wktSize)
		: 0u;

	GB_ByteBufferIO::AppendUInt32LE(buffer, wktSizeU32);
	if (wktSizeU32 > 0)
	{
		buffer.insert(buffer.end(), trimmedWkt.begin(), trimmedWkt.end());
	}

	GB_ByteBufferIO::AppendDoubleLE(buffer, rect.minX);
	GB_ByteBufferIO::AppendDoubleLE(buffer, rect.minY);
	GB_ByteBufferIO::AppendDoubleLE(buffer, rect.maxX);
	GB_ByteBufferIO::AppendDoubleLE(buffer, rect.maxY);

	return buffer;
}

bool GeoBoundingBox::Deserialize(const GB_ByteBuffer& data)
{
	Reset();

	if (data.empty())
	{
		return false;
	}

	size_t offset = 0;
	uint32_t magic = 0;
	uint32_t tag = 0;
	uint16_t version = 0;
	uint16_t reserved = 0;
	uint32_t wktSize = 0;

	if (!GB_ByteBufferIO::ReadUInt32LE(data, offset, magic) || magic != GB_ClassMagicNumber)
	{
		return false;
	}

	if (!GB_ByteBufferIO::ReadUInt32LE(data, offset, tag) || tag != kGeoBoundingBoxBinaryTag)
	{
		return false;
	}

	if (!GB_ByteBufferIO::ReadUInt16LE(data, offset, version) || version != kGeoBoundingBoxBinaryVersion)
	{
		return false;
	}

	if (!GB_ByteBufferIO::ReadUInt16LE(data, offset, reserved))
	{
		return false;
	}

	if (!GB_ByteBufferIO::ReadUInt32LE(data, offset, wktSize))
	{
		return false;
	}

	if (wktSize > 0)
	{
		if (offset + static_cast<size_t>(wktSize) > data.size())
		{
			return false;
		}

		wktUtf8.assign(reinterpret_cast<const char*>(&data[offset]), static_cast<size_t>(wktSize));
		offset += static_cast<size_t>(wktSize);
	}

	double minX = GB_QuietNan;
	double minY = GB_QuietNan;
	double maxX = GB_QuietNan;
	double maxY = GB_QuietNan;

	if (!GB_ByteBufferIO::ReadDoubleLE(data, offset, minX) ||
		!GB_ByteBufferIO::ReadDoubleLE(data, offset, minY) ||
		!GB_ByteBufferIO::ReadDoubleLE(data, offset, maxX) ||
		!GB_ByteBufferIO::ReadDoubleLE(data, offset, maxY))
	{
		Reset();
		return false;
	}

	rect.minX = minX;
	rect.minY = minY;
	rect.maxX = maxX;
	rect.maxY = maxY;

	if (IsFiniteRectangle(rect))
	{
		NormalizeRectangleValues(rect.minX, rect.minY, rect.maxX, rect.maxY);
	}

	return true;
}

bool GeoBoundingBox::Deserialize(const std::string& data)
{
	Reset();

	if (data.empty())
	{
		return false;
	}

	// 0) 先尝试解析可读文本格式："{GeoBoundingBox: wkt=...;rect={...}}"
	{
		std::string wktField;
		GB_Rectangle parsedRect;
		if (TryParseGeoBoundingBoxText(data, wktField, parsedRect))
		{
			std::string finalWkt = GB_Utf8Trim(wktField);
			const int epsgCode = TryParseEpsgCodeFromString(finalWkt);
			if (epsgCode > 0)
			{
				const std::shared_ptr<const GeoCrs> crs = GeoCrsManager::GetFromEpsgCached(epsgCode);
				if (crs && crs->IsValid())
				{
					finalWkt = crs->ExportToWktUtf8(GeoCrs::WktFormat::Wkt2_2018, false);
				}
			}

			wktUtf8 = finalWkt;
			rect = parsedRect;
			rect.Normalize();
			return true;
		}
	}

	// 1) 先尝试按“原始二进制串”解析（防止有人把 SerializeToBinary 的结果直接塞进 std::string 传进来）。
	{
		const GB_ByteBuffer raw = ToByteBuffer(data);
		if (Deserialize(raw))
		{
			return true;
		}
	}

	// 2) 再尝试按 Base64 文本解析。
	std::string trimmed = GB_Utf8Trim(data);
	if (trimmed.empty())
	{
		return false;
	}

	if (StartsWith(trimmed, "GBB64:"))
	{
		trimmed = trimmed.substr(6);
		trimmed = GB_Utf8Trim(trimmed);
	}

	std::string decoded;
	bool ok = GB_Base64Decode(trimmed, decoded, true, true);
	if (!ok)
	{
		ok = GB_Base64Decode(trimmed, decoded, false, false);
	}
	if (!ok)
	{
		return false;
	}

	const GB_ByteBuffer bytes = ToByteBuffer(decoded);
	return Deserialize(bytes);
}

bool GeoBoundingBox::ClampRectToCrsValidArea()
{
	const std::string trimmedWkt = GB_Utf8Trim(wktUtf8);
	if (trimmedWkt.empty())
	{
		return false;
	}

	if (!IsFiniteRectangle(rect))
	{
		return false;
	}

	GeoBoundingBox lonLatArea;
	GeoBoundingBox selfArea;
	GeoCrsManager::TryGetValidAreasCached(trimmedWkt, lonLatArea, selfArea);

	if (!selfArea.rect.IsValid())
	{
		GBLOG_WARNING(GB_STR("【GeoBoundingBox::ClampRectToCrsValidArea】无法获得 CRS 有效范围。"));
		return false;
	}

	const GB_Rectangle& limitRect = selfArea.rect;

	double minX = ClampDouble(rect.minX, limitRect.minX, limitRect.maxX);
	double maxX = ClampDouble(rect.maxX, limitRect.minX, limitRect.maxX);
	double minY = ClampDouble(rect.minY, limitRect.minY, limitRect.maxY);
	double maxY = ClampDouble(rect.maxY, limitRect.minY, limitRect.maxY);

	NormalizeRectangleValues(minX, minY, maxX, maxY);
	if (rect.Area() == 0)
	{
		*this = Invalid;
		return false;
	}

	rect.Set(minX, minY, maxX, maxY);

	return true;
}

GeoBoundingBox GeoBoundingBox::ClampedRectToCrsValidArea() const
{
	GeoBoundingBox result = *this;
	result.ClampRectToCrsValidArea();
	return result;
}
