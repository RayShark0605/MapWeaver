#include "GeoCrsTransform.h"

#include "GeoBoundingBox.h"
#include "GeoCrs.h"
#include "GeoCrsManager.h"

#include "GB_Logger.h"
#include "GB_Utf8String.h"

#include "Geometry/GB_Point2d.h"
#include "Geometry/GB_Rectangle.h"

#include "ogr_spatialref.h"
#include "ogr_srs_api.h"

#include "gdal_version.h"

#include <algorithm>
#include <memory>
#include <cmath>
#include <cstddef>
#include <atomic>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _OPENMP
#include <omp.h>
#endif

namespace
{
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
    using OgrSrsPtr = std::unique_ptr<OGRSpatialReference, GeoCrsOgrSrsDeleter>;

    static bool IsFinite(double value)
    {
        return std::isfinite(value);
    }

    static bool IsFinitePoint(const GB_Point2d& point)
    {
        return IsFinite(point.x) && IsFinite(point.y);
    }

    static void EnsureTraditionalGisAxisOrder(OGRSpatialReference& srs)
    {
        srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    }

    static double NormalizeLongitudeDegrees(double longitude)
    {
        if (!IsFinite(longitude))
        {
            return longitude;
        }

        // 归一化到 [-180, 180]
        double normalized = std::fmod(longitude, 360.0);
        if (normalized > 180.0)
        {
            normalized -= 360.0;
        }
        else if (normalized < -180.0)
        {
            normalized += 360.0;
        }

        return normalized;
    }

    struct TransformKey
    {
        std::string sourceUid;
        std::string targetUid;

        bool operator==(const TransformKey& other) const
        {
            return sourceUid == other.sourceUid && targetUid == other.targetUid;
        }
    };

    struct TransformKeyHasher
    {
        size_t operator()(const TransformKey& key) const
        {
            const std::hash<std::string> hasher;
            size_t hashValue = hasher(key.sourceUid);
            hashValue ^= hasher(key.targetUid) + 0x9e3779b97f4a7c15ULL + (hashValue << 6) + (hashValue >> 2);
            return hashValue;
        }
    };

    struct TransformItem
    {
        OgrSrsPtr sourceSrs;
        OgrSrsPtr targetSrs;
        CoordinateTransformationPtr transform;

        bool sourceIsGeographic = false;
        bool targetIsGeographic = false;

        // 源 CRS 的自身有效范围（便于 bbox 求交/点的可选归一化判断）
        GB_Rectangle sourceValidRect;
        bool hasSourceValidRect = false;

        std::string canonicalTargetWkt;
    };

    static thread_local std::unordered_map<TransformKey, TransformItem, TransformKeyHasher> g_threadTransformCache;

    static bool TryGetTransformItem(const std::string& sourceWktUtf8, const std::string& targetWktUtf8, TransformItem*& outItem)
    {
        outItem = nullptr;

        const std::string trimmedSourceWkt = GB_Utf8Trim(sourceWktUtf8);
        const std::string trimmedTargetWkt = GB_Utf8Trim(targetWktUtf8);
        if (trimmedSourceWkt.empty() || trimmedTargetWkt.empty())
        {
            return false;
        }

        const std::shared_ptr<const GeoCrs> sourceCrs = GeoCrsManager::GetFromWktCached(trimmedSourceWkt);
        const std::shared_ptr<const GeoCrs> targetCrs = GeoCrsManager::GetFromWktCached(trimmedTargetWkt);
        if (!sourceCrs || !targetCrs || !sourceCrs->IsValid() || !targetCrs->IsValid())
        {
            return false;
        }

        const std::string sourceUid = sourceCrs->GetUidUtf8();
        const std::string targetUid = targetCrs->GetUidUtf8();
        if (sourceUid.empty() || targetUid.empty())
        {
            return false;
        }

        TransformKey key;
        key.sourceUid = sourceUid;
        key.targetUid = targetUid;

        auto it = g_threadTransformCache.find(key);
        if (it != g_threadTransformCache.end())
        {
            outItem = &it->second;
            return it->second.transform != nullptr;
        }

        TransformItem item;
        item.sourceIsGeographic = sourceCrs->IsGeographic();
        item.targetIsGeographic = targetCrs->IsGeographic();

        // 取源 CRS 的自身有效范围（如果可用）
        {
            GeoBoundingBox lonLatArea;
            GeoBoundingBox selfArea;
            GeoCrsManager::TryGetValidAreasCached(trimmedSourceWkt, lonLatArea, selfArea);
            if (selfArea.IsValid())
            {
                item.sourceValidRect = selfArea.rect;
                item.hasSourceValidRect = selfArea.rect.IsValid();
            }
        }

        // 使用目标 CRS 的规范化 WKT（WKT2_2018），确保输出 GeoBoundingBox 的 wktUtf8 稳定。
        item.canonicalTargetWkt = targetCrs->ExportToWktUtf8(GeoCrs::WktFormat::Wkt2_2018, false);
        if (item.canonicalTargetWkt.empty())
        {
            // 兜底：至少保留用户输入。
            item.canonicalTargetWkt = trimmedTargetWkt;
        }

        const OGRSpatialReference& sourceRef = sourceCrs->GetConstRef();
        const OGRSpatialReference& targetRef = targetCrs->GetConstRef();

        item.sourceSrs.reset(sourceRef.Clone());
        item.targetSrs.reset(targetRef.Clone());
        if (!item.sourceSrs || !item.targetSrs)
        {
            return false;
        }

        EnsureTraditionalGisAxisOrder(*item.sourceSrs);
        EnsureTraditionalGisAxisOrder(*item.targetSrs);

        item.transform.reset(OGRCreateCoordinateTransformation(item.sourceSrs.get(), item.targetSrs.get()));
        if (item.transform == nullptr)
        {
            return false;
        }

        auto insertResult = g_threadTransformCache.emplace(std::move(key), std::move(item));
        outItem = &insertResult.first->second;
        return outItem->transform != nullptr;
    }

    static bool TryTransformSingleXYInternal(TransformItem& item, double x, double y, double& outX, double& outY)
    {
        outX = x;
        outY = y;

        if (!IsFinite(x) || !IsFinite(y) || item.transform == nullptr)
        {
            return false;
        }

        // 对 Geographic CRS 的经度做适度归一化，减少“超范围但等价”的失败。
        double inputX = x;
        double inputY = y;
        if (item.sourceIsGeographic)
        {
            inputX = NormalizeLongitudeDegrees(inputX);
        }

        int successFlag = FALSE;
        double transformedX = inputX;
        double transformedY = inputY;
        const int overallOk = item.transform->Transform(1, &transformedX, &transformedY, nullptr, &successFlag);

        if (overallOk == FALSE || successFlag == FALSE || !IsFinite(transformedX) || !IsFinite(transformedY))
        {
            return false;
        }

        // 若目标是 Geographic CRS，也进行经度归一化。跨日期线时，单点归一化是安全的。
        if (item.targetIsGeographic)
        {
            transformedX = NormalizeLongitudeDegrees(transformedX);
        }

        outX = transformedX;
        outY = transformedY;
        return true;
    }

    static bool TryTransformSingleXYZInternal(TransformItem& item, double x, double y, double z, double& outX, double& outY, double& outZ)
    {
        outX = x;
        outY = y;
        outZ = z;

        if (!IsFinite(x) || !IsFinite(y) || !IsFinite(z) || item.transform == nullptr)
        {
            return false;
        }

        double inputX = x;
        double inputY = y;
        double inputZ = z;
        if (item.sourceIsGeographic)
        {
            inputX = NormalizeLongitudeDegrees(inputX);
        }

        int successFlag = FALSE;
        const int overallOk = item.transform->Transform(1, &inputX, &inputY, &inputZ, &successFlag);

        if (overallOk == FALSE || successFlag == FALSE || !IsFinite(inputX) || !IsFinite(inputY) || !IsFinite(inputZ))
        {
            return false;
        }

        if (item.targetIsGeographic)
        {
            inputX = NormalizeLongitudeDegrees(inputX);
        }

        outX = inputX;
        outY = inputY;
        outZ = inputZ;
        return true;
    }

    static void AppendRectangleGridSamples(const GB_Rectangle& rect, int sampleGridCount, std::vector<double>& xs, std::vector<double>& ys)
    {
        const int count = std::max(2, sampleGridCount);
        xs.reserve(xs.size() + static_cast<size_t>(count) * static_cast<size_t>(count));
        ys.reserve(ys.size() + static_cast<size_t>(count) * static_cast<size_t>(count));

        for (int yIndex = 0; yIndex < count; yIndex++)
        {
            const double yT = (count <= 1) ? 0 : static_cast<double>(yIndex) / static_cast<double>(count - 1);
            const double y = rect.minY + (rect.maxY - rect.minY) * yT;

            for (int xIndex = 0; xIndex < count; xIndex++)
            {
                const double xT = (count <= 1) ? 0 : static_cast<double>(xIndex) / static_cast<double>(count - 1);
                const double x = rect.minX + (rect.maxX - rect.minX) * xT;
                xs.push_back(x);
                ys.push_back(y);
            }
        }
    }

    static bool TryTransformRectangleToAabbInternal(TransformItem& item, const GB_Rectangle& sourceRect, int sampleGridCount, GB_Rectangle& outTargetRect)
    {
        outTargetRect = GB_Rectangle::Invalid;

        if (!sourceRect.IsValid() || item.transform == nullptr)
        {
            return false;
        }

        GB_Rectangle workingRect = sourceRect;

        // 与源 CRS 的有效范围求交（更接近“部分交集”的真实语义）。
        if (item.hasSourceValidRect && item.sourceValidRect.IsValid())
        {
            workingRect = workingRect.Intersected(item.sourceValidRect);
        }

        // 若求交后退化/无效，则认为无法给出合理转换结果。
        if (!workingRect.IsValid() || workingRect.Area() <= 0.0)
        {
            return false;
        }

        // 优先使用 TransformBounds（GDAL 3.4+）：内部会沿边界加密采样，通常比手工网格更可靠。
        // 兼容性：若 GDAL < 3.4，则直接走下方“兜底：手工网格采样”。
#if defined(GDAL_VERSION_NUM) && GDAL_VERSION_NUM >= 3040000
        const int densifyPoints = std::max(2, sampleGridCount);
        double boundsMinX = 0.0;
        double boundsMinY = 0.0;
        double boundsMaxX = 0.0;
        double boundsMaxY = 0.0;
        const int boundsOk = item.transform->TransformBounds(
            workingRect.minX,
            workingRect.minY,
            workingRect.maxX,
            workingRect.maxY,
            &boundsMinX,
            &boundsMinY,
            &boundsMaxX,
            &boundsMaxY,
            densifyPoints);

        if (boundsOk != FALSE && IsFinite(boundsMinX) && IsFinite(boundsMinY) && IsFinite(boundsMaxX) && IsFinite(boundsMaxY))
        {
            // 目标为 Geographic CRS 时：TransformBounds 用“xmax < xmin”表示跨越反经线（日期变更线）。
            // 由于 GB_Rectangle 只能表示单段经度，这里保守返回全球经度范围。
            if (item.targetIsGeographic&& boundsMaxX < boundsMinX)
            {
                boundsMinX = -180.0;
                boundsMaxX = 180.0;
            }

            outTargetRect.Set(boundsMinX, boundsMinY, boundsMaxX, boundsMaxY);
            return outTargetRect.IsValid() && outTargetRect.Area() > 0.0;
        }
#endif

        // ---- 兜底：手工网格采样 ----
        std::vector<double> xValues;
        std::vector<double> yValues;
        AppendRectangleGridSamples(workingRect, sampleGridCount, xValues, yValues);

        const size_t numPoints = xValues.size();
        if (numPoints == 0 || numPoints > static_cast<size_t>(std::numeric_limits<int>::max()))
        {
            return false;
        }

        std::vector<int> successFlags(numPoints, FALSE);
        item.transform->Transform(static_cast<int>(numPoints), xValues.data(), yValues.data(), nullptr, successFlags.data());

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

            double x = xValues[i];
            double y = yValues[i];
            if (!IsFinite(x) || !IsFinite(y))
            {
                continue;
            }

            if (item.targetIsGeographic)
            {
                x = NormalizeLongitudeDegrees(x);
            }

            hasAnyPoint = true;
            minX = std::min(minX, x);
            minY = std::min(minY, y);
            maxX = std::max(maxX, x);
            maxY = std::max(maxY, y);
        }

        if (!hasAnyPoint)
        {
            return false;
        }

        if (item.targetIsGeographic)
        {
            const double lonRange = maxX - minX;
            if (lonRange > 180.0)
            {
                minX = -180.0;
                maxX = 180.0;
            }
        }

        outTargetRect.Set(minX, minY, maxX, maxY);
        return outTargetRect.IsValid() && outTargetRect.Area() > 0.0;
    }

    static void TransformPointsChunkInternal(
        TransformItem& item,
        std::vector<GB_Point2d>& points,
        size_t baseIndex,
        size_t count,
        std::atomic_bool& allOk,
        std::vector<double>& xValues,
        std::vector<double>& yValues,
        std::vector<int>& successFlags,
        std::vector<size_t>& indexMap)
    {
        xValues.clear();
        yValues.clear();
        indexMap.clear();

        xValues.reserve(count);
        yValues.reserve(count);
        indexMap.reserve(count);

        for (size_t i = 0; i < count; i++)
        {
            const size_t pointIndex = baseIndex + i;
            GB_Point2d& point = points[pointIndex];

            if (!IsFinitePoint(point))
            {
                allOk.store(false, std::memory_order_relaxed);
                continue;
            }

            double x = point.x;
            const double y = point.y;
            if (item.sourceIsGeographic)
            {
                x = NormalizeLongitudeDegrees(x);
            }

            indexMap.push_back(pointIndex);
            xValues.push_back(x);
            yValues.push_back(y);
        }

        const size_t validCount = indexMap.size();
        if (validCount == 0)
        {
            return;
        }

        if (validCount > static_cast<size_t>(std::numeric_limits<int>::max()))
        {
            allOk.store(false, std::memory_order_relaxed);
            return;
        }

        successFlags.assign(validCount, FALSE);

        const int overallOk = item.transform->Transform(
            static_cast<int>(validCount),
            xValues.data(),
            yValues.data(),
            nullptr,
            successFlags.data());

        if (overallOk == FALSE)
        {
            allOk.store(false, std::memory_order_relaxed);
        }

        for (size_t i = 0; i < validCount; i++)
        {
            if (successFlags[i] == FALSE || !IsFinite(xValues[i]) || !IsFinite(yValues[i]))
            {
                allOk.store(false, std::memory_order_relaxed);
                continue;
            }

            double x = xValues[i];
            const double y = yValues[i];
            if (item.targetIsGeographic)
            {
                x = NormalizeLongitudeDegrees(x);
            }

            GB_Point2d& point = points[indexMap[i]];
            point.Set(x, y);
            if (!point.IsValid())
            {
                allOk.store(false, std::memory_order_relaxed);
            }
        }
    }
}

bool GeoCrsTransform::TransformPoint(const std::string& sourceWktUtf8, const std::string& targetWktUtf8, const GB_Point2d& sourcePoint, GB_Point2d& outPoint)
{
    outPoint = sourcePoint;

    if (!IsFinitePoint(sourcePoint))
    {
        return false;
    }

    TransformItem* item = nullptr;
    if (!TryGetTransformItem(sourceWktUtf8, targetWktUtf8, item) || item == nullptr)
    {
        return false;
    }

    double outX = sourcePoint.x;
    double outY = sourcePoint.y;
    if (!TryTransformSingleXYInternal(*item, sourcePoint.x, sourcePoint.y, outX, outY))
    {
        return false;
    }

    outPoint.Set(outX, outY);
    return outPoint.IsValid();
}

bool GeoCrsTransform::TransformPoint(const std::string& sourceWktUtf8, const std::string& targetWktUtf8, GB_Point2d& inOutPoint)
{
    GB_Point2d transformed;
    if (!TransformPoint(sourceWktUtf8, targetWktUtf8, inOutPoint, transformed))
    {
        return false;
    }

    inOutPoint = transformed;
    return true;
}

bool GeoCrsTransform::TransformPoints(const std::string& sourceWktUtf8, const std::string& targetWktUtf8, const std::vector<GB_Point2d>& sourcePoints, std::vector<GB_Point2d>& outPoints, bool enableOpenMP)
{
    outPoints = sourcePoints;
    return TransformPoints(sourceWktUtf8, targetWktUtf8, outPoints, enableOpenMP);
}

bool GeoCrsTransform::TransformPoints(const std::string& sourceWktUtf8, const std::string& targetWktUtf8, std::vector<GB_Point2d>& inOutPoints, bool enableOpenMP)
{
    std::atomic_bool allOk(true);

    const size_t count = inOutPoints.size();
    if (count == 0)
    {
        return true;
    }

    constexpr size_t chunkSize = 4096;
    const size_t chunkCount = (count + chunkSize - 1) / chunkSize;
    const bool useParallel = enableOpenMP && chunkCount <= static_cast<size_t>(std::numeric_limits<int>::max());

    if (useParallel)
    {
#pragma omp parallel
        {
            TransformItem* threadItem = nullptr;
            if (!TryGetTransformItem(sourceWktUtf8, targetWktUtf8, threadItem) || threadItem == nullptr || threadItem->transform == nullptr)
            {
                allOk.store(false, std::memory_order_relaxed);
            }

            std::vector<double> xValues;
            std::vector<double> yValues;
            std::vector<int> successFlags;
            std::vector<size_t> indexMap;

#pragma omp for schedule(static)
            for (int chunkIndex = 0; chunkIndex < static_cast<int>(chunkCount); chunkIndex++)
            {
                if (threadItem == nullptr || threadItem->transform == nullptr)
                {
                    allOk.store(false, std::memory_order_relaxed);
                    continue;
                }

                const size_t baseIndex = static_cast<size_t>(chunkIndex) * chunkSize;
                const size_t remaining = count - baseIndex;
                const size_t thisChunkCount = std::min(chunkSize, remaining);
                TransformPointsChunkInternal(*threadItem, inOutPoints, baseIndex, thisChunkCount, allOk, xValues, yValues, successFlags, indexMap);
            }
        }
    }
    else
    {
        TransformItem* item = nullptr;
        if (!TryGetTransformItem(sourceWktUtf8, targetWktUtf8, item) || item == nullptr || item->transform == nullptr)
        {
            return false;
        }

        std::vector<double> xValues;
        std::vector<double> yValues;
        std::vector<int> successFlags;
        std::vector<size_t> indexMap;

        for (size_t baseIndex = 0; baseIndex < count; baseIndex += chunkSize)
        {
            const size_t remaining = count - baseIndex;
            const size_t thisChunkCount = std::min(chunkSize, remaining);
            TransformPointsChunkInternal(*item, inOutPoints, baseIndex, thisChunkCount, allOk, xValues, yValues, successFlags, indexMap);
        }
    }

    return allOk.load(std::memory_order_relaxed);
}

bool GeoCrsTransform::TransformXY(const std::string& sourceWktUtf8, const std::string& targetWktUtf8, double x, double y, double& outX, double& outY)
{
    outX = x;
    outY = y;

    TransformItem* item = nullptr;
    if (!TryGetTransformItem(sourceWktUtf8, targetWktUtf8, item) || item == nullptr)
    {
        return false;
    }

    return TryTransformSingleXYInternal(*item, x, y, outX, outY);
}

bool GeoCrsTransform::TransformXYZ(const std::string& sourceWktUtf8, const std::string& targetWktUtf8, double x, double y, double z, double& outX, double& outY, double& outZ)
{
    outX = x;
    outY = y;
    outZ = z;

    TransformItem* item = nullptr;
    if (!TryGetTransformItem(sourceWktUtf8, targetWktUtf8, item) || item == nullptr)
    {
        return false;
    }

    return TryTransformSingleXYZInternal(*item, x, y, z, outX, outY, outZ);
}

bool GeoCrsTransform::TransformBoundingBox(const GeoBoundingBox& sourceBox, const std::string& targetWktUtf8, GeoBoundingBox& outBox, int sampleGridCount)
{
    outBox = GeoBoundingBox::Invalid;

    const std::string trimmedSourceWkt = GB_Utf8Trim(sourceBox.wktUtf8);
    const std::string trimmedTargetWkt = GB_Utf8Trim(targetWktUtf8);
    if (trimmedSourceWkt.empty() || trimmedTargetWkt.empty())
    {
        return false;
    }

    if (!sourceBox.IsValid() || !sourceBox.rect.IsValid())
    {
        return false;
    }

    TransformItem* item = nullptr;
    if (!TryGetTransformItem(trimmedSourceWkt, trimmedTargetWkt, item) || item == nullptr)
    {
        return false;
    }

    GB_Rectangle targetRect;
    if (!TryTransformRectangleToAabbInternal(*item, sourceBox.rect, sampleGridCount, targetRect))
    {
        return false;
    }

    GeoBoundingBox result;
    result.wktUtf8 = item->canonicalTargetWkt;
    result.rect = targetRect;
    outBox = result;
    return outBox.IsValid();
}

bool GeoCrsTransform::TransformBoundingBox(GeoBoundingBox& inOutBox, const std::string& targetWktUtf8, int sampleGridCount)
{
    GeoBoundingBox transformed;
    if (!TransformBoundingBox(inOutBox, targetWktUtf8, transformed, sampleGridCount))
    {
        return false;
    }

    inOutBox = transformed;
    return true;
}

bool GeoCrsTransform::TransformBoundingBoxes(const std::vector<GeoBoundingBox>& sourceBoxes, const std::string& targetWktUtf8, std::vector<GeoBoundingBox>& outBoxes, bool enableOpenMP, int sampleGridCount)
{
    outBoxes = sourceBoxes;
    return TryTransformBoundingBoxes(outBoxes, targetWktUtf8, enableOpenMP, sampleGridCount);
}

bool GeoCrsTransform::TryTransformBoundingBoxes(std::vector<GeoBoundingBox>& inOutBoxes, const std::string& targetWktUtf8, bool enableOpenMP, int sampleGridCount)
{
    const std::string trimmedTargetWkt = GB_Utf8Trim(targetWktUtf8);
    if (trimmedTargetWkt.empty())
    {
        return false;
    }

    std::atomic_bool allOk(true);
    const size_t count = inOutBoxes.size();
    if (count == 0)
    {
        return true;
    }

    auto transformOne = [&](size_t index) {
        GeoBoundingBox& bbox = inOutBoxes[index];

        if (!bbox.IsValid() || !bbox.rect.IsValid())
        {
            bbox = GeoBoundingBox::Invalid;
            allOk.store(false, std::memory_order_relaxed);
            return;
        }

        const std::string trimmedSourceWkt = GB_Utf8Trim(bbox.wktUtf8);
        if (trimmedSourceWkt.empty())
        {
            bbox = GeoBoundingBox::Invalid;
            allOk.store(false, std::memory_order_relaxed);
            return;
        }

        TransformItem* item = nullptr;
        if (!TryGetTransformItem(trimmedSourceWkt, trimmedTargetWkt, item) || item == nullptr)
        {
            bbox = GeoBoundingBox::Invalid;
            allOk.store(false, std::memory_order_relaxed);
            return;
        }

        GB_Rectangle targetRect;
        if (!TryTransformRectangleToAabbInternal(*item, bbox.rect, sampleGridCount, targetRect))
        {
            bbox = GeoBoundingBox::Invalid;
            allOk.store(false, std::memory_order_relaxed);
            return;
        }

        bbox.wktUtf8 = item->canonicalTargetWkt;
        bbox.rect = targetRect;
    };

    if (enableOpenMP)
    {
#pragma omp parallel for
        for (long long i = 0; i < static_cast<long long>(count); i++)
        {
            transformOne(static_cast<size_t>(i));
        }
    }
    else
    {
        for (size_t i = 0; i < count; i++)
        {
            transformOne(i);
        }
    }

    return allOk.load(std::memory_order_relaxed);
}
