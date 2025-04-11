#include "Common.h"
#include "WMSCapabilities.h"
#include "gdal_priv.h"
#include "gdalwarper.h"
#include <sstream>

using namespace std;
const static double NaN = numeric_limits<double>::quiet_NaN();

Point2d::Point2d(double x, double y) :x(x), y(y)
{
}
bool Point2d::IsValid() const
{
	return !isnan(x) && !isnan(y);
}

Point2d Point2d::operator*(double scalar) const
{
	return Point2d(x * scalar, y * scalar);
}

Point2d& Point2d::operator*=(double scalar)
{
	x *= scalar;
	y *= scalar;
	return *this;
}

Rectangle::Rectangle(double minX, double minY, double maxX, double maxY, bool needNormalize) : minX(minX), maxX(maxX), minY(minY), maxY(maxY)
{
	if (needNormalize)
	{
		if (this->minX > this->maxX)
		{
			swap(this->minX, this->maxX);
		}
		if (this->minY > this->maxY)
		{
			swap(this->minY, this->maxY);
		}
	}
}
Rectangle::Rectangle(const Point2d& minPt, const Point2d& maxPt, bool needNormalize)
{
	*this = Rectangle(minPt.x, minPt.y, maxPt.x, maxPt.y, needNormalize);
}
bool Rectangle::IsValid() const
{
	return !isnan(minX) && !isnan(minY) && !isnan(maxX) && !isnan(maxY);
}
double Rectangle::GetWidth() const
{
	return maxX - minX;
}
double Rectangle::GetHeight() const
{
	return maxY - minY;
}
Point2d Rectangle::GetCenterPoint() const
{
	return Point2d(minX / 2 + maxX / 2, minY / 2 + maxY / 2);
}
Point2d Rectangle::GetMinPoint() const
{
	return Point2d(minX, minY);
}
Point2d Rectangle::GetMaxPoint() const
{
	return Point2d(maxX, maxY);
}
void Rectangle::Invert()
{
	swap(minX, minY);
	swap(maxX, maxY);
}

Rectangle Rectangle::Invert() const
{
	Rectangle result = *this;
	result.Invert();
	return result;
}

string Rectangle::ToString() const
{
	return to_string(minX) + "," + to_string(minY) + "," + to_string(maxX) + "," + to_string(maxY);
}

BoundingBox::BoundingBox()
{
	crs = "";
	bbox = Rectangle(NaN, NaN, NaN, NaN, false);
}

BoundingBox::BoundingBox(const string& crs, const Rectangle& bbox): crs(crs), bbox(bbox)
{
}
BoundingBox::BoundingBox(const string& crs, const Point2d& minPt, const Point2d& maxPt) :crs(crs), bbox(minPt, maxPt)
{
}
BoundingBox::BoundingBox(const string& crs, double minX, double minY, double maxX, double maxY) :crs(crs), bbox(minX, minY, maxX, maxY)
{
}
bool BoundingBox::IsValid() const
{
	return !crs.empty() && bbox.IsValid();
}

void BoundingBox::Invert()
{
	bbox.Invert();
}

KeyValuePair::KeyValuePair(const string& key, const string& value) : key(key), value(value)
{
}

namespace internal
{
	static void ToLower(string& str)
	{
		transform(str.begin(), str.end(), str.begin(), ::tolower);
	}
}

bool URLProcessing::HasQueryParam(const string& url, const string& key, string& value)
{
	value = "";
	const size_t questionMarkPos = url.find('?');
	if (questionMarkPos == string::npos)
	{
		return false;
	}

	string lowerKey = key;
	internal::ToLower(lowerKey);

	// 获取查询参数部分（获取'?'后面的部分）
	const string query = url.substr(questionMarkPos + 1);
	stringstream ss(query);
	string param = "";

	// 遍历查询参数
	while (getline(ss, param, '&'))
	{
		const size_t equalPos = param.find('=');
		if (equalPos == string::npos)
		{
			continue;
		}

		string paramKey = param.substr(0, equalPos);
		const string paramValue = param.substr(equalPos + 1);

		internal::ToLower(paramKey);
		if (paramKey == lowerKey)
		{
			value = paramValue;
			return true;
		}
	}
	return false;
}
vector<KeyValuePair> URLProcessing::ExtractQueryParams(const string& url)
{
	const size_t questionMarkPos = url.find('?');
	if (questionMarkPos == string::npos)
	{
		return {};
	}

	// 获取查询参数部分（获取'?'后面的部分）
	vector<KeyValuePair> params;
	const string query = url.substr(questionMarkPos + 1);
	stringstream ss(query);
	string param = "";

	// 遍历查询参数
	while (getline(ss, param, '&'))
	{
		const size_t equalPos = param.find('=');
		if (equalPos == string::npos)
		{
			continue;
		}

		string key = param.substr(0, equalPos);
		internal::ToLower(key);
		const string value = param.substr(equalPos + 1);
		params.emplace_back(key, value);
	}
	return params;
}
string URLProcessing::AddQueryParam(const string& url, const string& key, const string& value)
{
	// 情况1: url中不带任何参数
	const size_t questionMarkPos = url.find('?');
	if (questionMarkPos == string::npos)
	{
		return url + "?" + key + "=" + value;
	}

	// 情况2: url中带参数, 但是不包括key
	{
		string existsValue = "";
		if (!HasQueryParam(url, key, existsValue))
		{
			string result = url;
			if (result.back() == '&')
			{
				result.pop_back();
			}
			result += "&" + key + "=" + value;
			return result;
		}
	}

	// 情况3: url中带参数, 而且已存在key
	const string baseUrl = url.substr(0, questionMarkPos);
	string result = baseUrl + "?";
	vector<KeyValuePair> params = ExtractQueryParams(url);
	for (KeyValuePair& kvp : params)
	{
		if (kvp.key == key)
		{
			kvp.value = value;
		}
		result += kvp.key + "=" + kvp.value + "&";
	}
	result.pop_back(); // 去掉最后加上的'&'
	return result;
}

void URLProcessing::AddQueryParam(string& url, const string& key, const string& value)
{
	// 情况1: url中不带任何参数
	const size_t questionMarkPos = url.find('?');
	if (questionMarkPos == string::npos)
	{
		url = url + "?" + key + "=" + value;
		return;
	}

	// 情况2: url中带参数, 但是不包括key
	{
		string existsValue = "";
		if (!HasQueryParam(url, key, existsValue))
		{
			if (url.back() == '&')
			{
				url.pop_back();
			}
			url += "&" + key + "=" + value;
			return;
		}
	}

	// 情况3: url中带参数, 而且已存在key
	const string baseUrl = url.substr(0, questionMarkPos);
	string result = baseUrl + "?";
	vector<KeyValuePair> params = ExtractQueryParams(url);
	for (KeyValuePair& kvp : params)
	{
		if (kvp.key == key)
		{
			kvp.value = value;
		}
		result += kvp.key + "=" + kvp.value + "&";
	}
	result.pop_back(); // 去掉最后加上的'&'
	url = result;
}

string URLProcessing::GetRequestBaseUrl(const string& url)
{
	const size_t questionMarkPos = url.find('?');
	if (questionMarkPos == string::npos)
	{
		return url;
	}
	return url.substr(0, questionMarkPos);
}

void URLProcessing::ReplaceQueryParam(string& url, const string& key, const string& value, bool isCaseSensitive)
{
	if (key.empty())
	{
		return;
	}

	const size_t keyLength = key.size();
	const size_t valueLength = value.size();
	size_t pos = 0;
	while (pos <= url.size())
	{
		size_t found = string::npos;
		if (isCaseSensitive)
		{
			found = url.find(key, pos);
		}
		else
		{
			// 自定义大小写不敏感的搜索
			auto it = search(
				url.begin() + pos, url.end(),
				key.begin(), key.end(),
				[](char c1, char c2) {
					return tolower(static_cast<unsigned char>(c1)) == tolower(static_cast<unsigned char>(c2));
				});
			found = (it == url.end()) ? string::npos : distance(url.begin(), it);
		}

		if (found == string::npos)
		{
			break;
		}

		url.replace(found, keyLength, value);
		pos = found + valueLength; // 跳转到替换后的新位置继续查找
	}
}

vector<string> SplitString(const string& input, char delimiter)
{
	vector<string> tokens;
	istringstream iss(input);
	string token;
	while (getline(iss, token, delimiter))
	{
		if (!token.empty())
		{
			tokens.push_back(token);
		}
	}
	return tokens;
}

BoundingBox GetCSBoundingBox4326(const string& epsgCode)
{
	BoundingBox result;

	OGRSpatialReference crs;
	if (crs.SetFromUserInput(epsgCode.c_str()) != OGRERR_NONE)
	{
		return result;
	}

	const char* areaName = nullptr;
	double west = 0, south = 0, east = 0, north = 0;
	if (!crs.GetAreaOfUse(&west, &south, &east, &north, &areaName))
	{
		return result;
	}

	if (crs.AutoIdentifyEPSG() != OGRERR_NONE)
	{
		return result;
	}

	result.crs = "EPSG:4326";
	result.bbox = Rectangle(west, south, east, north, false);
	return result;
}

BoundingBox GetCSBoundingBox(const string& epsgCode)
{
	BoundingBox result;

	OGRSpatialReference crs;
	if (crs.SetFromUserInput(epsgCode.c_str()) != OGRERR_NONE)
	{
		return result;
	}
	const char* authName = crs.GetAuthorityName(nullptr);
	const char* authCode = crs.GetAuthorityCode(nullptr);
	if (!authName || !authCode)
	{
		return result;
	}

	const char* areaName = nullptr;
	double minX = 0, minY = 0, maxX = 0, maxY = 0;
	if (!crs.GetAreaOfUse(&minX, &minY, &maxX, &maxY, &areaName))
	{
		return result;
	}

	result.crs = string(authName) + ":" + string(authCode);
	result.bbox = Rectangle(minX, minY, maxX, maxY);
	return result;
}

BoundingBox GetBoundingBoxOverlap(const BoundingBox& bbox1, const BoundingBox& bbox2)
{
	BoundingBox result;
	if (bbox1.crs.empty() || bbox2.crs.empty() || bbox1.crs != bbox2.crs || !bbox1.bbox.IsValid() || !bbox2.bbox.IsValid())
	{
		return result;
	}

	result.crs = bbox1.crs;

	const Point2d minPt1 = bbox1.bbox.GetMinPoint();
	const Point2d maxPt1 = bbox1.bbox.GetMaxPoint();
	const Point2d minPt2 = bbox2.bbox.GetMinPoint();
	const Point2d maxPt2 = bbox2.bbox.GetMaxPoint();

	// 计算交集范围
	const double overlapMinX = max(minPt1.x, minPt2.x);
	const double overlapMaxX = min(maxPt1.x, maxPt2.x);
	const double overlapMinY = max(minPt1.y, minPt2.y);
	const double overlapMaxY = min(maxPt1.y, maxPt2.y);

	if (overlapMinX > overlapMaxX || overlapMinY > overlapMaxY)
	{
		result.bbox = Rectangle();
		return result;
	}

	result.bbox = Rectangle(overlapMinX, overlapMinY, overlapMaxX, overlapMaxY, false);
	return result;
}

// 递归提取几何体中所有顶点的辅助函数
void ExtractPoints(OGRGeometry* geom, vector<OGRPoint>& points)
{
	if (!geom)
	{
		return;
	}

	switch (geom->getGeometryType())
	{
	case wkbPolygon: // 单多边形
	{
		OGRPolygon* polygon = (OGRPolygon*)geom;
		if (polygon)
		{
			for (int i = 0; i < polygon->getExteriorRing()->getNumPoints(); i++)
			{
				OGRPoint p;
				polygon->getExteriorRing()->getPoint(i, &p);
				points.push_back(p);
			}
			// 处理内环（孔洞）
			for (int ir = 0; ir < polygon->getNumInteriorRings(); ir++)
			{
				OGRLinearRing* ring = polygon->getInteriorRing(ir);
				if (ring)
				{
					for (int i = 0; i < ring->getNumPoints(); i++)
					{
						OGRPoint p;
						ring->getPoint(i, &p);
						points.push_back(p);
					}
				}
			}
		}
		break;
	}
	case wkbMultiPolygon: // 多多边形集合
	{
		OGRMultiPolygon* mp = (OGRMultiPolygon*)geom;
		if (mp)
		{
			for (int i = 0; i < mp->getNumGeometries(); i++)
			{
				ExtractPoints(mp->getGeometryRef(i), points);
			}
		}
		break;
	}
	case wkbLineString: // 线状交集（如边界重合）
	{
		OGRLineString* line = (OGRLineString*)geom;
		if (line)
		{
			for (int i = 0; i < line->getNumPoints(); i++)
			{
				OGRPoint p;
				line->getPoint(i, &p);
				points.push_back(p);
			}
		}
		break;
	}
	default: // 其他类型（如点）直接添加
	{
		if (geom->getGeometryType() == wkbPoint)
		{
			points.push_back(*((OGRPoint*)geom));
		}
		break;
	}
	}
}
vector<Point2d> GetIntersectionVertices(const vector<Point2d>& points1, const vector<Point2d>& points2)
{
	if (points1.size() != 4 || points2.size() != 4)
	{
		return {};
	}

	const OGRPoint p1(points1[0].x, points1[0].y);
	const OGRPoint p2(points1[1].x, points1[1].y);
	const OGRPoint p3(points1[2].x, points1[2].y);
	const OGRPoint p4(points1[3].x, points1[3].y);

	const OGRPoint q1(points2[0].x, points2[0].y);
	const OGRPoint q2(points2[1].x, points2[1].y);
	const OGRPoint q3(points2[2].x, points2[2].y);
	const OGRPoint q4(points2[3].x, points2[3].y);

	OGRPolygon poly1, poly2;
	OGRLinearRing ring1, ring2;
	ring1.addPoint(&p1);
	ring1.addPoint(&p2);
	ring1.addPoint(&p3);
	ring1.addPoint(&p4);
	ring1.addPoint(&p1);
	poly1.addRing(&ring1);

	ring2.addPoint(&q1);
	ring2.addPoint(&q2);
	ring2.addPoint(&q3);
	ring2.addPoint(&q4);
	ring2.addPoint(&q1);
	poly2.addRing(&ring2);


	OGRGeometry* intersection = poly1.Intersection(&poly2);
	vector<OGRPoint> ogrResult;
	if (intersection && !intersection->IsEmpty())
	{
		ExtractPoints(intersection, ogrResult);
		auto last = unique(ogrResult.begin(), ogrResult.end(),
			[](const OGRPoint& a, const OGRPoint& b)
			{
				return abs(a.getX() - b.getX()) < 1e-6 && abs(a.getY() - b.getY()) < 1e-6;
			});
		ogrResult.erase(last, ogrResult.end());
	}
	OGRGeometryFactory::destroyGeometry(intersection);

	vector<Point2d> result(ogrResult.size());
	for (size_t i = 0; i < ogrResult.size(); i++)
	{
		result[i].x = ogrResult[i].getX();
		result[i].y = ogrResult[i].getY();
	}
	return result;
}

static string GetDir(const string& filePath)
{
	string dir = filePath;
	replace(dir.begin(), dir.end(), '\\', '/');
	const size_t lastSlashPos = dir.find_last_of('/');
	if (lastSlashPos != string::npos)
	{
		dir = dir.substr(0, lastSlashPos);
	}

	return dir;
}
static string GetFileName(const string& filePath)
{
	string filePathCopy = filePath;
	replace(filePathCopy.begin(), filePathCopy.end(), '\\', '/');
	const size_t pos1 = filePathCopy.find_last_of('/');
	const size_t pos2 = filePath.find_last_of('.');
	if (pos1 != string::npos && pos2 != string::npos && pos2 > pos1)
	{
		return filePath.substr(pos1 + 1, pos2 - pos1 - 1);
	}
	return "";
}

static GDALDataset* CreateEmptyImage(int width, int height, const string& imagePath = "temp.tiff")
{
	GDALDriver* tiffDriver = GetGDALDriverManager()->GetDriverByName("GTiff");
	if (!tiffDriver)
	{
		return nullptr;
	}

	GDALDataset* image = tiffDriver->Create(imagePath.c_str(), width, height, 4, GDT_Byte, nullptr);
	return image;
}

static bool ReadImagePixels(const string& imagePath, vector<uint8_t>& rBuffer, vector<uint8_t>& gBuffer, vector<uint8_t>& bBuffer, vector<uint8_t>& aBuffer)
{
	rBuffer.clear();
	gBuffer.clear();
	bBuffer.clear();
	aBuffer.clear();

	GDALDataset* image = (GDALDataset*)GDALOpen(imagePath.c_str(), GA_ReadOnly);
	if (!image)
	{
		return false;
	}

	const int bandCount = image->GetRasterCount();
	if (bandCount != 1 && bandCount != 3 && bandCount != 4)
	{
		return false;
	}

	const int width = image->GetRasterXSize();
	const int height = image->GetRasterYSize();
	if (width <= 0 || height <= 0)
	{
		return false;
	}

	const size_t numPixels = (size_t)width * (size_t)height;
	rBuffer.resize(numPixels, 0);
	gBuffer.resize(numPixels, 0);
	bBuffer.resize(numPixels, 0);
	aBuffer.resize(numPixels, 0);

	if (bandCount == 1)
	{
		GDALColorTable* colorTable = image->GetRasterBand(1)->GetColorTable();
		if (colorTable)
		{
			vector<uint8_t> indexBuffer(numPixels, 0);
			image->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, width, height, indexBuffer.data(), width, height, GDT_Byte, 0, 0);
			for (int i = 0; i < numPixels; i++)
			{
				GDALColorEntry color;
				colorTable->GetColorEntryAsRGB(indexBuffer[i], &color);
				rBuffer[i] = static_cast<uint8_t>(color.c1);
				gBuffer[i] = static_cast<uint8_t>(color.c2);
				bBuffer[i] = static_cast<uint8_t>(color.c3);
				aBuffer[i] = static_cast<uint8_t>(color.c4);
			}
		}
		else
		{
			image->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, width, height, rBuffer.data(), width, height, GDT_Byte, 0, 0);
			gBuffer = bBuffer = aBuffer = rBuffer;
		}
	}
	else if (bandCount == 3)
	{
		image->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, width, height, rBuffer.data(), width, height, GDT_Byte, 0, 0);
		image->GetRasterBand(2)->RasterIO(GF_Read, 0, 0, width, height, gBuffer.data(), width, height, GDT_Byte, 0, 0);
		image->GetRasterBand(3)->RasterIO(GF_Read, 0, 0, width, height, bBuffer.data(), width, height, GDT_Byte, 0, 0);
		fill(aBuffer.begin(), aBuffer.end(), (uint8_t)255);
	}
	else if (bandCount == 4)
	{
		image->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, width, height, rBuffer.data(), width, height, GDT_Byte, 0, 0);
		image->GetRasterBand(2)->RasterIO(GF_Read, 0, 0, width, height, gBuffer.data(), width, height, GDT_Byte, 0, 0);
		image->GetRasterBand(3)->RasterIO(GF_Read, 0, 0, width, height, bBuffer.data(), width, height, GDT_Byte, 0, 0);
		image->GetRasterBand(4)->RasterIO(GF_Read, 0, 0, width, height, aBuffer.data(), width, height, GDT_Byte, 0, 0);
	}

	GDALClose(image);
	return true;
}

bool TileSplice(const vector<TileInfo>& tiles, string& resultImagePath)
{
	if (tiles.empty())
	{
		return false;
	}

	OGRSpatialReference crs;
	if (crs.SetFromUserInput(tiles[0].bbox.crs.c_str()) != OGRERR_NONE)
	{
		return false;
	}
	char* wkt = nullptr; // 记得使用CPLFree(wkt);释放
	if (crs.exportToWkt(&wkt) != OGRERR_NONE || !wkt)
	{
		return false;
	}

	int minTileX = tiles[0].col, maxTileX = tiles[0].col;
	int minTileY = tiles[0].row, maxTileY = tiles[0].row;
	for (const TileInfo& tile : tiles)
	{
		minTileX = min(minTileX, tile.col);
		maxTileX = max(maxTileX, tile.col);
		minTileY = min(minTileY, tile.row);
		maxTileY = max(maxTileY, tile.row);
	}

	const int tileWidth = tiles[0].numWidthPixels, tileHeight = tiles[0].numHeightPixels;
	const size_t tilePixelsNum = (size_t)tileWidth * (size_t)tileHeight;
	const int tileMatrixWidth = tileWidth * (maxTileX - minTileX + 1);
	const int tileMatrixHeight = tileHeight * (maxTileY - minTileY + 1);
	if (tileMatrixWidth <= 0 || tileMatrixHeight <= 0)
	{
		CPLFree(wkt);
		return false;
	}

	const size_t numPixels = (size_t)tileMatrixWidth * (size_t)tileMatrixHeight;
	const string spliceTileImagePath = GetDir(tiles[0].filePath) + "/splice_tile.tiff";
	GDALDataset* image = CreateEmptyImage(tileMatrixWidth, tileMatrixHeight, spliceTileImagePath);
	if (!image)
	{
		CPLFree(wkt);
		return false;
	}

	vector<uint8_t> rBuffer(numPixels, 0);
	vector<uint8_t> gBuffer(numPixels, 0);
	vector<uint8_t> bBuffer(numPixels, 0);
	vector<uint8_t> aBuffer(numPixels, 0);

	for (const TileInfo& tile : tiles)
	{
		vector<uint8_t> tileRBuffer, tileGBuffer, tileBBuffer, tileABuffer;
		if (!ReadImagePixels(tile.filePath, tileRBuffer, tileGBuffer, tileBBuffer, tileABuffer) || tileRBuffer.size() != tilePixelsNum ||
			tileGBuffer.size() != tilePixelsNum || tileBBuffer.size() != tilePixelsNum || tileABuffer.size() != tilePixelsNum)
		{
			continue;
		}

		for (int row = 0; row < tileHeight; row++)
		{
			for (int col = 0; col < tileWidth; col++)
			{
				const size_t offsetX = (size_t)(tile.col - minTileX) * (size_t)tileWidth + col;
				const size_t offsetY = (size_t)(tile.row - minTileY) * (size_t)tileHeight + row;
				const size_t index = offsetY * (size_t)tileMatrixWidth + offsetX;
				const size_t tileIndex = (size_t)row * (size_t)tileWidth + col;
				if (index >= rBuffer.size() || tileIndex >= tileRBuffer.size())
				{
					continue; // 不应该发生
				}

				rBuffer[index] = tileRBuffer[tileIndex];
				gBuffer[index] = tileGBuffer[tileIndex];
				bBuffer[index] = tileBBuffer[tileIndex];
				aBuffer[index] = tileABuffer[tileIndex];
			}
		}
	}

	image->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, tileMatrixWidth, tileMatrixHeight, rBuffer.data(), tileMatrixWidth, tileMatrixHeight, GDT_Byte, 0, 0);
	image->GetRasterBand(2)->RasterIO(GF_Write, 0, 0, tileMatrixWidth, tileMatrixHeight, gBuffer.data(), tileMatrixWidth, tileMatrixHeight, GDT_Byte, 0, 0);
	image->GetRasterBand(3)->RasterIO(GF_Write, 0, 0, tileMatrixWidth, tileMatrixHeight, bBuffer.data(), tileMatrixWidth, tileMatrixHeight, GDT_Byte, 0, 0);
	image->GetRasterBand(4)->RasterIO(GF_Write, 0, 0, tileMatrixWidth, tileMatrixHeight, aBuffer.data(), tileMatrixWidth, tileMatrixHeight, GDT_Byte, 0, 0);

	const double resolutionX = tiles[0].bbox.bbox.GetWidth() / tiles[0].numWidthPixels;
	const double resolutionY = tiles[0].bbox.bbox.GetHeight() / tiles[0].numHeightPixels;
	const Point2d imageLeftTop(tiles[0].bbox.bbox.GetMinPoint().x, tiles[0].bbox.bbox.GetMaxPoint().y);
	vector<double> transform = {
		imageLeftTop.x, resolutionX, 0,
		imageLeftTop.y, 0, -resolutionY
	};

	if (image->SetProjection(wkt) != CPLErr::CE_None)
	{
		GDALClose(image);
		CPLFree(wkt);
		return false;
	}
	if (image->SetGeoTransform(transform.data()) != CPLErr::CE_None)
	{
		GDALClose(image);
		CPLFree(wkt);
		return false;
	}

	CPLFree(wkt);
	GDALClose(image);
	resultImagePath = spliceTileImagePath;
	return true;
}

bool ReprojectImage(const string& imagePath, const string& targetCRS, string& resultImagePath)
{
	OGRSpatialReference crs;
	if (crs.SetFromUserInput(targetCRS.c_str()) != OGRERR_NONE)
	{
		return false;
	}
	char* targetWKT = nullptr; // 记得使用CPLFree(wkt);释放
	if (crs.exportToWkt(&targetWKT) != OGRERR_NONE || !targetWKT)
	{
		return false;
	}

	GDALAllRegister();
	GDALDataset* image = (GDALDataset*)GDALOpen(imagePath.c_str(), GA_ReadOnly);
	if (!image)
	{
		CPLFree(targetWKT);
		return false;
	}

	GDALDriver* tiffDriver = GetGDALDriverManager()->GetDriverByName("GTiff");
	if (!tiffDriver)
	{
		CPLFree(targetWKT);
		GDALClose(image);
		return false;
	}

	const string targetImagePath = GetDir(imagePath) + "/" + GetFileName(imagePath) + "_reproj.tiff";
	if (GDALCreateAndReprojectImage(image, nullptr, targetImagePath.c_str(), targetWKT, tiffDriver, 
		nullptr, GRA_NearestNeighbour, 0, 0.5, nullptr, nullptr, nullptr) != CPLErr::CE_None)
	{
		CPLFree(targetWKT);
		GDALClose(image);
		return false;
	}

	CPLFree(targetWKT);
	GDALClose(image);
	resultImagePath = targetImagePath;
	return true;
}

bool ReprojectImage(const string& imagePath, const string& sourceCRS, const string& targetCRS, string& resultImagePath)
{
	OGRSpatialReference crs1, crs2;
	if (crs1.SetFromUserInput(sourceCRS.c_str()) != OGRERR_NONE)
	{
		return false;
	}
	char* sourceWKT = nullptr; // 记得使用CPLFree(wkt);释放
	if (crs1.exportToWkt(&sourceWKT) != OGRERR_NONE || !sourceWKT)
	{
		return false;
	}

	if (crs2.SetFromUserInput(targetCRS.c_str()) != OGRERR_NONE)
	{
		CPLFree(sourceWKT);
		return false;
	}
	char* targetWKT = nullptr; // 记得使用CPLFree(wkt);释放
	if (crs2.exportToWkt(&targetWKT) != OGRERR_NONE || !targetWKT)
	{
		CPLFree(sourceWKT);
		return false;
	}

	GDALAllRegister();
	GDALDataset* image = (GDALDataset*)GDALOpen(imagePath.c_str(), GA_ReadOnly);
	if (!image)
	{
		CPLFree(sourceWKT);
		CPLFree(targetWKT);
		return false;
	}

	GDALDriver* tiffDriver = GetGDALDriverManager()->GetDriverByName("GTiff");
	if (!tiffDriver)
	{
		CPLFree(sourceWKT);
		CPLFree(targetWKT);
		GDALClose(image);
		return false;
	}

	const string targetImagePath = GetDir(imagePath) + "/" + GetFileName(imagePath) + "_reproj.tiff";
	if (GDALCreateAndReprojectImage(image, sourceWKT, targetImagePath.c_str(), targetWKT, tiffDriver,
		nullptr, GRA_NearestNeighbour, 0, 0.5, nullptr, nullptr, nullptr) != CPLErr::CE_None)
	{
		CPLFree(sourceWKT);
		CPLFree(targetWKT);
		GDALClose(image);
		return false;
	}

	CPLFree(sourceWKT);
	CPLFree(targetWKT);
	GDALClose(image);
	resultImagePath = targetImagePath;
	return true;
}

bool ReprojectTile(const TileInfo& tile, const string& targetCRS, string& resultImagePath)
{
	OGRSpatialReference crs1, crs2;
	if (crs1.SetFromUserInput(tile.bbox.crs.c_str()) != OGRERR_NONE)
	{
		return false;
	}
	char* sourceWKT = nullptr; // 记得使用CPLFree(wkt);释放
	if (crs1.exportToWkt(&sourceWKT) != OGRERR_NONE || !sourceWKT)
	{
		return false;
	}

	if (crs2.SetFromUserInput(targetCRS.c_str()) != OGRERR_NONE)
	{
		CPLFree(sourceWKT);
		return false;
	}
	char* targetWKT = nullptr; // 记得使用CPLFree(wkt);释放
	if (crs2.exportToWkt(&targetWKT) != OGRERR_NONE || !targetWKT)
	{
		CPLFree(sourceWKT);
		return false;
	}

	GDALDataset* image = nullptr;
	try
	{
		image = (GDALDataset*)GDALOpen(tile.filePath.c_str(), GA_ReadOnly);
	}
	catch (const exception& e)
	{
		CPLFree(sourceWKT);
		CPLFree(targetWKT);
		return false;
	}
	
	if (!image)
	{
		CPLFree(sourceWKT);
		CPLFree(targetWKT);
		return false;
	}
	if (image->SetProjection(sourceWKT) != CPLErr::CE_None)
	{
		CPLFree(sourceWKT);
		CPLFree(targetWKT);
		GDALClose(image);
		return false;
	}

	const double resolutionX = tile.bbox.bbox.GetWidth() / tile.numWidthPixels;
	const double resolutionY = tile.bbox.bbox.GetHeight() / tile.numHeightPixels;
	const Point2d imageLeftTop(tile.bbox.bbox.GetMinPoint().x, tile.bbox.bbox.GetMaxPoint().y);
	vector<double> transform = {
		imageLeftTop.x, resolutionX, 0,
		imageLeftTop.y, 0, -resolutionY
	};
	if (image->SetGeoTransform(transform.data()) != CPLErr::CE_None)
	{
		CPLFree(sourceWKT);
		CPLFree(targetWKT);
		GDALClose(image);
		return false;
	}

	GDALDriver* tiffDriver = GetGDALDriverManager()->GetDriverByName("GTiff");
	if (!tiffDriver)
	{
		CPLFree(sourceWKT);
		CPLFree(targetWKT);
		GDALClose(image);
		return false;
	}

	const string targetImagePath = GetDir(tile.filePath) + "/" + GetFileName(tile.filePath) + "_reproj.tiff";
	if (GDALCreateAndReprojectImage(image, sourceWKT, targetImagePath.c_str(), targetWKT, tiffDriver,
		nullptr, GRA_NearestNeighbour, 0, 0, nullptr, nullptr, nullptr) != CPLErr::CE_None)
	{
		CPLFree(sourceWKT);
		CPLFree(targetWKT);
		GDALClose(image);
		return false;
	}

	CPLFree(sourceWKT);
	CPLFree(targetWKT);
	GDALClose(image);
	resultImagePath = targetImagePath;
	return true;
}

unordered_map<string, bool> CSConverter::crsInvertAxisCache;
void CSConverter::Initial(const string& GDALSharePath)
{
	const char* projPath[] = { GDALSharePath.c_str(), nullptr };
	OSRSetPROJSearchPaths(projPath);

	crsInvertAxisCache.clear();
}

bool CSConverter::TransformPoint(const string& srcEPSGCode, const Point2d& srcPt, const string& destEPSGCode, Point2d& destPt)
{
	OGRSpatialReference srcRef, dstRef;
	if (srcRef.SetFromUserInput(srcEPSGCode.c_str()) != OGRERR_NONE)
	{
		return false;
	}
	if (dstRef.SetFromUserInput(destEPSGCode.c_str()) != OGRERR_NONE)
	{
		return false;
	}

	// 强制将所有坐标按 (经度, 纬度) 顺序解释
	srcRef.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
	dstRef.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

	OGRCoordinateTransformation* coordTransform = OGRCreateCoordinateTransformation(&srcRef, &dstRef);
	if (!coordTransform)
	{
		return false;
	}

	double x = srcPt.x, y = srcPt.y;
	const int transformResult = coordTransform->Transform(1, &x, &y);
	OGRCoordinateTransformation::DestroyCT(coordTransform);
	if (transformResult != 1)
	{
		return false;
	}

	destPt.x = x;
	destPt.y = y;
	return true;
}

bool CSConverter::TransformPoints(const string& srcEPSGCode, const vector<Point2d>& srcPts, const string& destEPSGCode, vector<Point2d>& destPts, vector<int>& successFlag)
{
	OGRSpatialReference srcRef, dstRef;
	if (srcRef.SetFromUserInput(srcEPSGCode.c_str()) != OGRERR_NONE)
	{
		return false;
	}
	if (dstRef.SetFromUserInput(destEPSGCode.c_str()) != OGRERR_NONE)
	{
		return false;
	}

	// 强制将所有坐标按 (经度, 纬度) 顺序解释
	srcRef.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
	dstRef.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

	OGRCoordinateTransformation* coordTransform = OGRCreateCoordinateTransformation(&srcRef, &dstRef);
	if (!coordTransform)
	{
		return false;
	}

	vector<double> x(srcPts.size());
	vector<double> y(srcPts.size());
	vector<int> success(srcPts.size(), 0);
	for (size_t i = 0; i < srcPts.size(); i++)
	{
		x[i] = srcPts[i].x;
		y[i] = srcPts[i].y;
	}
	const int transformResult = coordTransform->Transform(srcPts.size(), x.data(), y.data(), nullptr, success.data());
	if (transformResult != 1)
	{
		OGRCoordinateTransformation::DestroyCT(coordTransform);
		return false;
	}

	successFlag = vector<int>(srcPts.size(), 0);
	destPts = vector<Point2d>(srcPts.size());
	for (size_t i = 0; i < srcPts.size(); i++)
	{
		destPts[i].x = x[i];
		destPts[i].y = y[i];
		successFlag[i] = success[i];
	}

	OGRCoordinateTransformation::DestroyCT(coordTransform);
	return all_of(successFlag.begin(), successFlag.end(), [](int value) { return value == 1; });
}

bool CSConverter::TransformBoundingBox(const BoundingBox& srcBoundingBox, BoundingBox& destBoundingBox, bool isRestrictedArea)
{
	const Point2d srcP1 = srcBoundingBox.bbox.GetMinPoint();
	const Point2d srcP2 = srcBoundingBox.bbox.GetMaxPoint();
	const Point2d srcP3(srcP1.x, srcP2.y);
	const Point2d srcP4(srcP2.x, srcP1.y);

	Point2d destP1, destP2, destP3, destP4;
	if (!TransformPoint(srcBoundingBox.crs, srcP1, destBoundingBox.crs, destP1))
	{
		return false;
	}
	if (!TransformPoint(srcBoundingBox.crs, srcP2, destBoundingBox.crs, destP2))
	{
		return false;
	}
	if (!TransformPoint(srcBoundingBox.crs, srcP3, destBoundingBox.crs, destP3))
	{
		return false;
	}
	if (!TransformPoint(srcBoundingBox.crs, srcP4, destBoundingBox.crs, destP4))
	{
		return false;
	}

	const double minX = min({ destP1.x, destP2.x, destP3.x, destP4.x });
	const double minY = min({ destP1.y, destP2.y, destP3.y, destP4.y });
	const double maxX = max({ destP1.x, destP2.x, destP3.x, destP4.x });
	const double maxY = max({ destP1.y, destP2.y, destP3.y, destP4.y });

	destBoundingBox.bbox = Rectangle(minX, minY, maxX, maxY);
	if (destBoundingBox.crs == "EPSG:4326" || !isRestrictedArea)
	{
		return true;
	}

	// 确保转换后的bbox不能超过其坐标系的最大范围
	const BoundingBox destCRSMaxBBox4326 = GetCSBoundingBox4326(destBoundingBox.crs);
	BoundingBox destCRSMaxBBox(destBoundingBox.crs, Rectangle());
	if (!TransformBoundingBox(destCRSMaxBBox4326, destCRSMaxBBox, false))
	{
		return false;
	}
	destBoundingBox = GetBoundingBoxOverlap(destBoundingBox, destCRSMaxBBox);
	return true;
}

bool CSConverter::ShouldInvertAxisOrientation(const string& EPSGCode)
{
	const auto findCache = crsInvertAxisCache.find(EPSGCode);
	if (findCache != crsInvertAxisCache.end())
	{
		return findCache->second;
	}

	OGRSpatialReference crs;
	if (crs.SetFromUserInput(EPSGCode.c_str()) != OGRERR_NONE)
	{
		return false;
	}

	// 纬度优先或北向优先
	const bool result = crs.EPSGTreatsAsLatLong() || crs.EPSGTreatsAsNorthingEasting();
	crsInvertAxisCache[EPSGCode] = result;

	return result;
}