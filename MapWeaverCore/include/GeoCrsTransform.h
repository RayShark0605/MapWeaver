#ifndef MAP_WEAVER_GEO_CRS_TRANSFORM_H
#define MAP_WEAVER_GEO_CRS_TRANSFORM_H

#include "MapWeaverPort.h"

#include <string>
#include <vector>

class GB_Point2d;
class GeoBoundingBox;

// GeoCrsTransform
// - 静态坐标系转换工具类（线程安全设计）：
//   1) 内部为每个线程维护一份 OGRCoordinateTransformation 缓存（避免跨线程共享 CT 对象）；
//   2) 全程采用传统 GIS 轴顺序（X=经度/Easting, Y=纬度/Northing），以与 GB_Point2d/GB_Rectangle 的语义一致；
//   3) 对 GeoBoundingBox 会先与源 CRS 的“自身有效范围”求交，再计算目标空间的 AABB：
//      - GDAL >= 3.4 优先使用 TransformBounds(densify_pts=sampleGridCount) 以更好地覆盖非线性投影边界；
//      - 其它版本/失败情况则退化为网格采样；
//      - 若目标为经纬度坐标系且跨越反经线（日期变更线），由于单个矩形无法表达两段经度，这里会保守返回 [-180,180]。
//   4) 变换失败时返回 false，并尽量保持输出/原地数据不被破坏。
class MAPWEAVERCORE_PORT GeoCrsTransform
{
public:
    // （1）把单个 GB_Point2d 从一个 WKT 转到另一个 WKT（输出到 outPoint）。
    static bool TransformPoint(const std::string& sourceWktUtf8, const std::string& targetWktUtf8, const GB_Point2d& sourcePoint, GB_Point2d& outPoint);

    // （1）把单个 GB_Point2d 从一个 WKT 转到另一个 WKT（原地修改）。
    static bool TransformPoint(const std::string& sourceWktUtf8, const std::string& targetWktUtf8, GB_Point2d& inOutPoint);

    // （2）将多个 GB_Point2d 从一个 WKT 转到另一个 WKT（输出到 outPoints）。
    // - enableOpenMp=true 时，若编译器支持 OpenMP，则并行处理。
    // - 返回值：所有点均成功变换返回 true；任一失败返回 false（但成功点仍会写入 outPoints）。
    static bool TransformPoints(const std::string& sourceWktUtf8, const std::string& targetWktUtf8, const std::vector<GB_Point2d>& sourcePoints, std::vector<GB_Point2d>& outPoints, bool enableOpenMP = false);

    // （2）将多个 GB_Point2d 从一个 WKT 转到另一个 WKT（原地修改）。
    // - 返回值语义同上。
    static bool TransformPoints(const std::string& sourceWktUtf8, const std::string& targetWktUtf8, std::vector<GB_Point2d>& inOutPoints, bool enableOpenMP = false);

    // （3）传入 x、y 坐标从一个 WKT 转到另一个 WKT。
    static bool TransformXY(const std::string& sourceWktUtf8, const std::string& targetWktUtf8, double x, double y, double& outX, double& outY);

    // （4）传入 x、y、z 坐标从一个 WKT 转到另一个 WKT。
    static bool TransformXYZ(const std::string& sourceWktUtf8, const std::string& targetWktUtf8, double x, double y, double z, double& outX, double& outY, double& outZ);

    // （5）把单个 GeoBoundingBox 从当前 wkt 转到另一个 wkt（输出到 outBox）。
    static bool TransformBoundingBox(const GeoBoundingBox& sourceBox, const std::string& targetWktUtf8, GeoBoundingBox& outBox, int sampleGridCount = 11);

    // （5）把单个 GeoBoundingBox 从当前 wkt 转到另一个 wkt（原地修改）。
    static bool TransformBoundingBox(GeoBoundingBox& inOutBox, const std::string& targetWktUtf8, int sampleGridCount = 11);

    // （6）把多个 GeoBoundingBox 从各自的 wkt 转到另一个 wkt（输出到 outBoxes）。
    // - 返回值：所有 bbox 均成功变换返回 true；任一失败返回 false（失败项会被写成 Invalid）。
    static bool TransformBoundingBoxes(const std::vector<GeoBoundingBox>& sourceBoxes, const std::string& targetWktUtf8, std::vector<GeoBoundingBox>& outBoxes, bool enableOpenMP = false, int sampleGridCount = 11);

    // （6）把多个 GeoBoundingBox 从各自的 wkt 转到另一个 wkt（原地修改）。
    static bool TryTransformBoundingBoxes(std::vector<GeoBoundingBox>& inOutBoxes, const std::string& targetWktUtf8, bool enableOpenMP = false, int sampleGridCount = 11);

private:
    GeoCrsTransform() = delete;
    ~GeoCrsTransform() = delete;
    GeoCrsTransform(const GeoCrsTransform&) = delete;
    GeoCrsTransform& operator=(const GeoCrsTransform&) = delete;
};

#endif
