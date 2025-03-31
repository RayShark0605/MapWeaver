#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <iostream>
#include <limits>
#include "Base.h"
#include "ogr_spatialref.h"
#include "ogr_geometry.h"

struct Point2d
{
	double x = std::numeric_limits<double>::quiet_NaN();
	double y = std::numeric_limits<double>::quiet_NaN();

	Point2d() = default;
	Point2d(double x, double y);

	bool IsValid() const;

	Point2d operator*(double scalar) const;
	Point2d& operator*=(double scalar);
};


class Rectangle
{
public:
	Rectangle() = default;
	Rectangle(double minX, double minY, double maxX, double maxY, bool needNormalize = true);
	Rectangle(const Point2d& minPt, const Point2d& maxPt, bool needNormalize = true);

	bool IsValid() const;
	double GetWidth() const;
	double GetHeight() const;
	Point2d GetCenterPoint() const;
	Point2d GetMinPoint() const;
	Point2d GetMaxPoint() const;

	// 分别交换左下角点和右下角点的XY坐标
	void Invert();
	Rectangle Invert() const;

	// 转换成例如"-180,-90,180,90"这样的字符串
	std::string ToString() const;

private:
	double minX = std::numeric_limits<double>::quiet_NaN();
	double minY = std::numeric_limits<double>::quiet_NaN();
	double maxX = std::numeric_limits<double>::quiet_NaN();
	double maxY = std::numeric_limits<double>::quiet_NaN();
};

struct BoundingBox
{
	std::string crs = ""; // 坐标系的标识符，例如"CRS:84"、"EPSG:3857"等
	Rectangle bbox;

	BoundingBox();
	BoundingBox(const std::string& crs, const Rectangle& bbox);
	BoundingBox(const std::string& crs, const Point2d& minPt, const Point2d& maxPt);
	BoundingBox(const std::string& crs, double minX, double minY, double maxX, double maxY);

	bool IsValid() const;
	void Invert();
};

struct KeyValuePair
{
	std::string key = "";
	std::string value = "";

	KeyValuePair() = default;
	KeyValuePair(const std::string& key, const std::string& value);
};

// URL字符串处理
class URLProcessing
{
public:
	// 判断URL中是否存在指定的查询参数
	static bool HasQueryParam(const std::string& url, const std::string& key, std::string& value);
	
	// 从URL中提取所有的查询参数
	static std::vector<KeyValuePair> ExtractQueryParams(const std::string& url);

	// 给URL添加查询参数, 如果key已存在则替换value
	static std::string AddQueryParam(const std::string& url, const std::string& key, const std::string& value);
	static void AddQueryParam(std::string& url, const std::string& key, const std::string& value);

	// 获取URL的基础部分，即去掉查询参数部分，以问号结尾
	static std::string GetRequestBaseUrl(const std::string& url);

	// 替换URL中的查询参数，一般用于REST，默认大小写不敏感
	static void ReplaceQueryParam(std::string& url, const std::string& key, const std::string& value, bool isCaseSensitive = false);
};

// 坐标系转换
class CSConverter
{
public:
	// 初始化：设置proj.db所在的路径
	static void Initial(const std::string& GDALSharePath);

	// 转换单个点, 转换失败则返回false。例如从"EPSG:4326"转"EPSG:3857"。
	static bool TransformPoint(const std::string& srcEPSGCode, const Point2d& srcPt, const std::string& destEPSGCode, Point2d& destPt);

	// 转换多个点, 如果存在某个点转换失败则返回false
	static bool TransformPoints(const std::string& srcEPSGCode, const std::vector<Point2d>& srcPts, const std::string& destEPSGCode, std::vector<Point2d>& destPts, std::vector<int>& successFlag);

	// 转换BoundingBox
	static bool TransformBoundingBox(const BoundingBox& srcBoundingBox, BoundingBox& destBoundingBox);

	// 判断是否需要反转坐标轴. WMS 1.3.0标准规定, 某些CRS的轴顺序是纬度优先(Lat, Lon), 而不是经度优先(Lon, Lat)
	static bool ShouldInvertAxisOrientation(const std::string& EPSGCode);

private:
	static std::unordered_map<std::string, bool> crsInvertAxisCache;
};

// 将字符串按照空白字符分割成多个子串
std::vector<std::string> SplitString(const std::string& input);

// 获取指定坐标系范围在EPSG:4326下的包络盒
BoundingBox GetCSBoundingBox4326(const std::string& epsgCode);

// 获取两个包络盒的重叠部分
BoundingBox GetBoundingBoxOverlap(const BoundingBox& bbox1, const BoundingBox& bbox2);

// 计算两个四边形（不一定是矩形）的交集区域
std::vector<Point2d> GetIntersectionVertices(const std::vector<Point2d>& points1, const std::vector<Point2d>& points2);

// 瓦片拼接
struct TileInfo;
bool TileSplice(const std::vector<TileInfo>& tiles, std::string& resultImagePath);

// 图像重投影
bool ReprojectImage(const std::string& imagePath, const std::string& targetCRS, std::string& resultImagePath);
