#ifndef MAP_WEAVER_ARCGIS_MAPSERVICE_INFO_H
#define MAP_WEAVER_ARCGIS_MAPSERVICE_INFO_H

#include <string>
#include <vector>
#include "Geometry/GB_Point2d.h"

struct ArcGISSpatialReference
{
    int m_wkid = -1;
    int m_latestWkid = -1;
    std::string m_wkt = "";
};

struct ArcGISLodInfo
{
    int m_level = -1;
    double m_resolution = 0;
    double m_scale = 0;
};

struct ArcGISExtent
{
    bool m_isValid = false;
    double m_xmin = 0;
    double m_ymin = 0;
    double m_xmax = 0;
    double m_ymax = 0;
    ArcGISSpatialReference m_spatialReference;
};

struct ArcGISLayerInfo
{
    int m_id = -1;
    std::string m_name = "";
    int m_parentLayerId = -1;
    bool m_defaultVisibility = false;
    std::vector<int> m_subLayerIds;
    double m_minScale = 0;
    double m_maxScale = 0;
};

struct ArcGISTileInfo
{
    int m_rows = 0;
    int m_cols = 0;
    int m_dpi = 0;
    std::string m_format = "";
    int m_compressionQuality = 0;
    GB_Point2d m_origin;
    ArcGISSpatialReference m_spatialReference;
    std::vector<ArcGISLodInfo> m_lods;
};

struct ArcGISDocumentInfo
{
    std::string m_title = "";
    std::string m_author = "";
    std::string m_comments = "";
    std::string m_subject = "";
    std::string m_category = "";
    std::string m_antialiasingMode = "";
    std::string m_textAntialiasingMode = "";
    std::string m_keywords = "";
};

struct ArcGISMapServiceInfo
{
    std::string m_currentVersion = "";
    std::string m_serviceDescription = "";
    std::string m_mapName = "";
    std::string m_description = "";
    std::string m_copyrightText = "";
    bool m_supportsDynamicLayers = false;

    std::vector<ArcGISLayerInfo> m_layers;
    std::vector<ArcGISLayerInfo> m_tables;

    bool m_hasSpatialReference = false;
    ArcGISSpatialReference m_spatialReference;

    bool m_singleFusedMapCache = false;

    bool m_hasTileInfo = false;
    ArcGISTileInfo m_tileInfo;

    bool m_hasInitialExtent = false;
    ArcGISExtent m_initialExtent;

    bool m_hasFullExtent = false;
    ArcGISExtent m_fullExtent;

    double m_minScale = 0;
    double m_maxScale = 0;
    std::string m_units;

    std::vector<std::string> m_supportedImageFormatTypes;
    ArcGISDocumentInfo m_documentInfo;
    std::vector<std::string> m_capabilities;
    std::vector<std::string> m_supportedQueryFormats;
    bool m_exportTilesAllowed = false;
    int m_maxRecordCount = 0;
    int m_maxImageHeight = 0;
    int m_maxImageWidth = 0;
    std::vector<std::string> m_supportedExtensions;
};

#endif