#ifndef MAP_WEAVER_CORE_H
#define MAP_WEAVER_CORE_H

#include "MapWeaverPort.h"
#include "GB_Network.h"
#include "Geometry/GB_Polygon.h"
#include "MapLayer.h"
#include "ArcGISMapServiceInfo.h"
#include <cstddef>
#include <string>
#include <vector>

/**
 * @brief 粗略判断一个 URL 是否更像 WMTS 服务地址。
 *
 * 当前会识别的典型形态包括：
 * - KVP 风格中的 `SERVICE=WMTS`；
 * - RESTful 风格中的 `/WMTSCapabilities.xml`；
 * - 常见路径片段 `/wmts?`、`/wmts/`、以及以 `/wmts` 结尾的 URL。
 *
 * @param urlUtf8 输入 URL（UTF-8）。
 * @return true  看起来是 WMTS；
 * @return false 看起来更像普通 WMS 或无法明确判断。
 *
 * @note 这是启发式判断，不保证 100% 准确；真正是否可访问、是否符合规范，仍取决于服务端返回内容。
 */
MAPWEAVERCORE_PORT bool IsUrlForWMTS(const std::string& urlUtf8);

/**
 * @brief 下载 WMS/WMTS 的 Capabilities 文本，并统一输出为 UTF-8 XML 字符串。
 *
 * @details
 * - 对普通 WMS URL，会自动补充/覆盖 `SERVICE=WMS` 与 `REQUEST=GetCapabilities`；
 * - 对 WMTS URL，会自动补充/覆盖 `SERVICE=WMTS` 与 `REQUEST=GetCapabilities`；
 * - 若自动补参后的 URL 请求失败，会退回到原始 URL 再尝试一次；
 * - 返回内容会自动尝试按 XML 头声明的编码转为 UTF-8，并去除 UTF-8 BOM。
 *
 * @param urlUtf8                 输入服务地址（UTF-8）。
 * @param outCapabilitiesXmlUtf8  输出：UTF-8 编码的 Capabilities XML。
 * @param options                 网络请求选项，例如代理、超时、TLS 校验等。
 * @return true  下载并转换成功；
 * @return false 下载失败或返回内容不可用。
 */
MAPWEAVERCORE_PORT bool DownloadWmsCapabilities(const std::string& urlUtf8, std::string& outCapabilitiesXmlUtf8, const GB_NetworkRequestOptions& options = GB_NetworkRequestOptions());

/**
 * @brief 请求 ArcGIS Server 的 `f=pjson` 服务描述，并解析为结构化结果。
 *
 * @details
 * 该接口面向 ArcGIS MapServer / ImageServer / FeatureServer 等服务根地址：
 * - 会尽量把输入 URL 规范化为服务根，再补 `f=pjson`；
 * - 会解析常见的服务元信息、图层列表、空间参考、瓦片信息、范围、支持格式等字段；
 * - 若服务返回 ArcGIS 风格的 `error` 对象，则视为失败。
 *
 * @param urlUtf8         输入 URL（UTF-8）。
 * @param mapServiceInfo  输出：解析后的 ArcGIS 服务信息。
 * @param options         网络请求选项。
 * @return true  请求并解析成功；
 * @return false 失败。
 */
MAPWEAVERCORE_PORT bool RequestArcGISServerJson(const std::string& urlUtf8, ArcGISMapServiceInfo& mapServiceInfo, const GB_NetworkRequestOptions& options = GB_NetworkRequestOptions());

/**
 * @brief WMS/WMTS Capabilities 解析选项。
 */
struct WmsParserOptions
{
	/**
	 * @brief 是否忽略 CRS 轴顺序。
	 *
	 * @details
	 * 设为 true 时，不再根据 CRS 的权威轴顺序信息去交换经纬/XY 次序，
	 * 统一按传统 GIS 习惯解释为 X 在前、Y 在后。
	 */
	bool ignoreAxisOrientation = false;

	/**
	 * @brief 是否在默认判断结果基础上强制反转轴顺序。
	 *
	 * @details
	 * 该选项常用于兼容少数服务端“声明与实际输出不一致”的情况。
	 * 它是在默认轴顺序判断之后再做一次逻辑取反。
	 */
	bool invertAxisOrientation = false;
};

/**
 * @brief 解析 WMS/WMTS 的 Capabilities XML。
 *
 * @details
 * 本函数会把 XML 中的服务信息、WMS 图层、WMTS 图层、TileMatrixSet、请求入口、样式、范围、
 * 尺寸限制、维度等内容解析到 @ref WmsCapabilitiesProperty 中。
 *
 * `baseUrl` 用于补全部分相对路径资源地址；
 * `arcGISMapServiceInfo` 则用于在 ArcGIS Server 场景下补充部分 Capabilities 本身缺失但可从 pjson 推断的信息。
 *
 * @param capabilitiesXmlUtf8   输入：UTF-8 编码的 Capabilities XML 文本。
 * @param baseUrl               用于补全相对 URL 的基地址。
 * @param outCapabilities       输出：解析结果。
 * @param arcGISMapServiceInfo  可选的 ArcGIS 服务描述信息；传空表示不参与补充。
 * @param options               解析选项。
 * @return true  解析成功；
 * @return false XML 无效、根节点不符合预期或关键内容解析失败。
 */
MAPWEAVERCORE_PORT bool ParseWmsCapabilities(const std::string& capabilitiesXmlUtf8, const std::string& baseUrl, WmsCapabilitiesProperty& outCapabilities, const ArcGISMapServiceInfo* arcGISMapServiceInfo = nullptr, const WmsParserOptions& options = WmsParserOptions());

/**
 * @brief 构建图层树时的附加选项。
 */
struct BuildLayerTreeOptions
{
	/**
	 * @brief 是否折叠“只有唯一子节点”的中间层。
	 *
	 * @details
	 * 主要用于 WMTS 图层树展示：
	 * - 当某个图层只有一个 style / 一个 tileMatrixSet / 一个 format 时，
	 *   可直接省略该中间层，使树结构更紧凑；
	 * - 设为 false 时，则总是显式保留完整层级。
	 */
	bool ignoreUniqueChildNode = true;
};

/**
 * @brief WMS/WMTS 图层树节点。
 */
struct WmsTreeNode
{
	/**
	 * @brief 节点类型。
	 */
	enum class NodeType
	{
		Unknown = 0,
		Root,              ///< 根节点。
		Layer,             ///< WMS 图层或 WMTS 图层。
		WmtsTileMatrixSet, ///< WMTS TileMatrixSet 节点。
		Style,             ///< 样式节点。
		Format             ///< 输出格式节点。
	};

	NodeType nodeType = NodeType::Unknown; ///< 节点类型。
	std::string textUtf8 = "";             ///< 用于界面显示的文本，通常是标题或格式名。
	std::string nameUtf8 = "";             ///< 用于程序识别的名称，通常是 identifier / name。
	std::vector<WmsTreeNode> children;     ///< 子节点列表。
	std::string uidUtf8 = "";              ///< 稳定节点标识，当前实现基于路径字符串做哈希。

	/**
	 * @brief 把整棵子树格式化为可读字符串。
	 * @param indentStringUtf8 单层缩进字符串，默认 "----"。
	 * @return 文本化后的树结构。
	 */
	MAPWEAVERCORE_PORT std::string ToString(const std::string& indentStringUtf8 = "----") const;
};

/**
 * @brief 基于已解析的 Capabilities 构建用于展示的 WMS/WMTS 图层树。
 *
 * @details
 * 根节点使用服务标题作为显示文本；子节点中既包含普通 WMS 图层，也包含 WMTS 图层及其 style /
 * tileMatrixSet / format 等层级信息。
 *
 * @param capabilities 输入：已解析的服务能力信息。
 * @param rootNode     输出：构建后的根节点。
 * @param options      构建选项。
 * @return true  构建成功；
 * @return false 当前实现通常不会因普通数据问题失败，但仍保留返回值以便后续扩展。
 */
MAPWEAVERCORE_PORT bool BuildWmsLayerTree(const WmsCapabilitiesProperty& capabilities, WmsTreeNode& rootNode, const BuildLayerTreeOptions& options = BuildLayerTreeOptions());

/**
 * @brief 单个地图请求项。
 *
 * @details
 * 对 WMTS 来说通常对应一个瓦片请求；
 * 对 WMS 来说通常对应一个 GetMap 请求矩形。
 */
struct MapRequestItem
{
	std::string urlUtf8 = "";   ///< 最终请求 URL。
	GeoBoundingBox boundingBox; ///< 该请求覆盖的空间范围及其 CRS。
	int zoomLevel = -1;         ///< 选中的缩放级别；WMS 在非按 zoom 指定时可能为 -1。
	size_t rowIndex = 0, colIndex = 0; ///< WMTS 的行列号；对 WMS 通常无实际意义。
	std::string uidUtf8 = "";   ///< 用于去重和跟踪的稳定请求标识。
};

/**
 * @brief 目标输出分辨率/尺寸相关配置。
 */
struct MapRenderTargetOptions
{
	/**
	 * @brief 目标分辨率的确定方式。
	 */
	enum class ResolutionMode
	{
		Auto = 0,              ///< 自动：优先按输出像素尺寸推导目标分辨率。
		ByZoomLevel,           ///< 直接指定 zoom / lod。
		ByResolution,          ///< 直接指定目标分辨率（目标 CRS 单位 / 像素）。
		ByScaleDenominator,    ///< 通过比例尺分母推导目标分辨率。
		ByOutputImageSize      ///< 由输出图像尺寸 + BBOX 反推目标分辨率。
	};

	/**
	 * @brief 当需要从多个 LOD 中选一个时的选择策略。
	 */
	enum class LodSelectMode
	{
		Nearest = 0,           ///< 选分辨率最接近目标值的 LOD。
		NotCoarserThanTarget,  ///< 不允许比目标更粗；若没有完全满足者，则退到最细级别。
		NotFinerThanTarget     ///< 不允许比目标更细；若没有完全满足者，则退到最粗级别。
	};

	/**
	 * @brief WMS 输出图像宽高的确定方式。
	 */
	enum class WmsImageSizeMode
	{
		ByResolution = 0,      ///< 先确定目标分辨率，再反算 width / height。
		ExactWidthHeight,      ///< 直接使用 outputImageWidth / outputImageHeight。
		FixedLongEdge,         ///< 固定长边像素，短边按比例推导。
		FitWithinMaxSize       ///< 约束在 maxOutputImageWidth / maxOutputImageHeight 内。
	};

	ResolutionMode resolutionMode = ResolutionMode::Auto;   ///< 分辨率确定模式。
	LodSelectMode lodSelectMode = LodSelectMode::Nearest;   ///< LOD 选择策略。
	WmsImageSizeMode wmsImageSizeMode = WmsImageSizeMode::ByResolution; ///< WMS 输出尺寸模式。

	// ByZoomLevel
	int zoomLevel = -1; ///< 指定的 zoom / lod。

	// ByResolution
	double targetResolution = 0; ///< 目标分辨率，单位为“目标 CRS 坐标单位 / 像素”。

	// ByScaleDenominator
	double scaleDenominator = 0;      ///< 目标比例尺分母。
	double renderingPixelSizeMeters = 0.00028; ///< 渲染像素物理尺寸（米/像素），默认采用 OGC 常用 0.28 mm。

	// ByOutputImageSize / ExactWidthHeight
	size_t outputImageWidth = 0;  ///< 目标输出宽度（像素）。
	size_t outputImageHeight = 0; ///< 目标输出高度（像素）。

	// FixedLongEdge
	size_t longEdgePixels = 0; ///< 固定长边像素数。

	// FitWithinMaxSize
	size_t maxOutputImageWidth = 4096;  ///< 允许的最大输出宽度。
	size_t maxOutputImageHeight = 4096; ///< 允许的最大输出高度。

	bool keepAspectRatio = true; ///< 在可推导尺寸的模式下是否保持原始空间长宽比。
};

/**
 * @brief 构建当前可见区域请求项时的输入参数。
 */
struct BuildVisibleMapRequestItemsInput
{
	MapTileMode mapType; ///< 地图类型。当前实现主要支持 WMTS 与 WMSC（WMS-C 风格）。
	const WmsCapabilitiesProperty* capabilities = nullptr; ///< 已解析的能力信息。
	std::string mapServiceUrlUtf8 = ""; ///< 服务基础地址，用于构造最终请求 URL。
	std::string layerNameUtf8 = "";     ///< 目标图层名 / identifier。
	std::string styleUtf8 = "";         ///< 指定样式；为空时会按默认规则自动选择。
	std::string tileMatrixSetUtf8 = ""; ///< WMTS 指定的 TileMatrixSet；为空时尝试自动推断。
	std::string formatUtf8 = "";        ///< 指定输出格式；为空时会尽量选择更适合的栅格格式。
	int minZoomLevel = -1;               ///< 允许的最小缩放级别；<0 表示不限制。
	int maxZoomLevel = -1;               ///< 允许的最大缩放级别；<0 表示不限制。

	std::string requestAreaWkt = ""; ///< 请求区域的 CRS 定义（UTF-8，可为 EPSG/WKT 等）。
	GB_Polygon requestAreaPolygon;    ///< 请求区域多边形；其坐标必须与 requestAreaWkt 对应。

	MapRenderTargetOptions renderTarget; ///< 输出分辨率与尺寸要求。
};

/**
 * @brief 基于请求区域与服务能力，生成实际需要访问的地图请求项列表。
 *
 * @details
 * - 对 WMTS：会根据请求区域、TileMatrixSet、目标分辨率/LOD、样式、格式等，筛出与当前区域相交的瓦片；
 * - 对 WMS：会根据请求区域和目标分辨率，把多边形区域自适应拆分成若干矩形，再为每个矩形生成 GetMap 请求；
 * - 返回结果会按 `uidUtf8` 去重。
 *
 * @param input   输入参数。
 * @param success 可选输出：是否成功构建出至少一个有效请求项。
 * @return 请求项列表；失败时返回空数组。
 *
 * @note 当 `requestAreaPolygon` 很复杂时，WMS 路径下可能会生成多个矩形请求，以减少明显无效的空白区域下载。
 */
MAPWEAVERCORE_PORT std::vector<MapRequestItem> BuildVisibleMapRequestItems(const BuildVisibleMapRequestItemsInput& input, bool* success = nullptr);

//MAPWEAVERCORE_PORT bool TryExportWkt2WithCustomTransverseMercatorAreaBbox(const std::string& inputWkt, std::string& outputWkt2, bool* areaBboxInjected = nullptr);

//MAPWEAVERCORE_PORT bool GetCartesianExtents(const std::string& wkt, double& minX, double& minY, double& maxX, double& maxY);

#endif
