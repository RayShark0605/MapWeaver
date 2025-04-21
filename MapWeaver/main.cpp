#include <iostream>
#include <chrono>
#include "WMSCapabilities.h"
#include "Network.h"
#include "ThreadPool.h"
#include "ogr_spatialref.h"
#include "ogr_geometry.h"
#include <gdal_priv.h>

using namespace std;

bool GetUserInput(const WMSCapabilitiesWorker& worker, string& layerTitle, string& tileMatrixSetName, string& format, string& style)
{
    layerTitle = tileMatrixSetName = format = style = "";
    vector<string> layerTitles = worker.GetRootLayerTitles();
    if (layerTitles.empty())
    {
        cout << "未找到图层！" << endl;
        return false;
    }

    // 确定地图图层
    while (!layerTitles.empty())
    {
        if (layerTitles.size() == 1)
        {
            layerTitle = layerTitles[0];
            cout << "默认使用唯一图层：" << layerTitle << endl;
        }
        else
        {
            cout << "图层列表：" << endl;
            for (const string& layerName : layerTitles)
            {
                cout << layerName << endl;
            }
            while (true)
            {
                cout << endl << endl << "选择图层名：" << endl;
                getline(cin, layerTitle);
                if (find(layerTitles.begin(), layerTitles.end(), layerTitle) == layerTitles.end())
                {
                    cout << "图层不存在！" << endl;
                    continue;
                }
                break;
            }
            cout << endl << endl;
        }

		layerTitles = worker.GetChildrenLayerTitles(layerTitle);
    }

    // 确定瓦片矩阵集
    if (worker.IsWMTSLayer(layerTitle))
    {
        const vector<string> tileMatrixSets = worker.GetLayerAllTileMatrixSets(layerTitle);
        if (tileMatrixSets.empty())
        {
            cout << "该图层不存在瓦片矩阵集！" << endl;
            return false;
        }

        if (tileMatrixSets.size() == 1)
        {
            tileMatrixSetName = tileMatrixSets[0];
            cout << "默认使用唯一瓦片矩阵集：" << tileMatrixSetName << endl;
        }
        else
        {
            cout << endl << "瓦片矩阵集列表：" << endl;
            for (const string& matrixSetName : tileMatrixSets)
            {
                cout << matrixSetName << endl;
            }

            while (true)
            {
                cout << endl << endl << "选择瓦片矩阵集：" << endl;
                getline(cin, tileMatrixSetName);
                if (find(tileMatrixSets.begin(), tileMatrixSets.end(), tileMatrixSetName) == tileMatrixSets.end())
                {
                    cout << "瓦片矩阵集不存在！" << endl;
                    continue;
                }
                break;
            }
            cout << endl << endl;
        }
    }

    // 确定格式
    const vector<string> formats = worker.GetLayerFormats(layerTitle);
    if (formats.empty())
    {
        cout << "警告：未找到任何格式！默认设置格式为image/png" << endl;
        format = "image/png";
    }
    else if (formats.size() == 1)
    {
        format = formats[0];
        cout << "默认使用唯一格式：" << format << endl;
    }
    else
    {
        cout << endl << "格式列表：" << endl;
        for (const string& curFormat : formats)
        {
            cout << curFormat << endl;
        }
        while (true)
        {
            cout << endl << endl << "选择格式：" << endl;
            getline(cin, format);
            if (find(formats.begin(), formats.end(), format) == formats.end())
            {
                cout << "格式不存在！" << endl;
                continue;
            }
            break;
        }
        cout << endl << endl;
    }

    // 确定风格
    const vector<string> styles = worker.GetLayerStyles(layerTitle);
    if (styles.empty())
    {
        cout << "警告：未找到任何风格！默认设置风格为空" << endl;
        style = "";
    }
    else if (styles.size() == 1)
    {
        style = styles[0];
        cout << "默认使用唯一风格：" << style << endl;
    }
    else
    {
        cout << endl << "风格列表：" << endl;
        for (const string& style : styles)
        {
            cout << style << endl;
        }
        while (true)
        {
            cout << endl << endl << "选择风格：" << endl;
            getline(cin, style);
            if (find(styles.begin(), styles.end(), style) == styles.end())
            {
                cout << "风格不存在！" << endl;
                continue;
            }
            break;
        }
        cout << endl << endl;
    }
    return true;
}

//const string url = "https://tiles.geovisearth.com/base/v1/wmts/GetCapabilities?tmsIds=w&token=1b6ea8d6e66808585e20b92eaed02c5edd0349a090b3b186b39e2b6f5074b51f";
//const string url = "https://data.geopf.fr/wms-r/wms?SERVICE=WMS&VERSION=1.3.0&REQUEST=GetCapabilities";
//const string url = "http://www.ign.es/wmts/pnoa-ma?SERVICE=WMTS&REQUEST=GETCAPABILITIES";

bool DownloadAndReprojectTile(const TileInfo& tile, const string& targetCRS, string& receiveInfo, string& targetImagePath, const string& proxyUrl = "", const string& proxyUserName = "", const string& proxyPassword = "")
{
    if (!DownloadImageMultiThread(tile.url, tile.filePath, receiveInfo, proxyUrl, proxyUserName, proxyPassword))
    {
        return false;
    }
    if (!ReprojectTile(tile, targetCRS, targetImagePath))
    {
        return false;
    }
    return true;
}

int main(int argc, char *argv[])
{
    GDALAllRegister();

    while (true)
    {
        // 获取URL
        string url = "";
        while (true)
        {
            cout << endl << endl << "输入XML链接：" << endl;
            getline(cin, url);
            if (url.find("http") == string::npos)
            {
                cout << "url不正确！" << endl;
                continue;
            }
            break;
        }
        const string proxyUrl = "http://127.0.0.1:10808", proxyUserName = "", proxyPassword = "";

        // 下载XML
        auto start = chrono::high_resolution_clock::now();
        string content = "", errorInfo = "";
        const bool downloadSuccess = WMSCapabilitiesDownloader::DownloadCapabilitiesXML(url, content, errorInfo, proxyUrl, proxyUserName, proxyPassword);
        auto end = chrono::high_resolution_clock::now();
        auto duration = chrono::duration_cast<chrono::milliseconds>(end - start);
        cout << "下载XML耗时：" << duration.count() << " 毫秒" << endl;
        if (!downloadSuccess)
        {
            cout << "XML下载失败！" << endl;
            cout << errorInfo << endl;
            continue;
        }
        cout << "下载完成！" << endl;

        // 解析XML
        start = chrono::high_resolution_clock::now();
        CSConverter::Initial(GetProjDirPath());
        WMSCapabilitiesWorker worker;
        if (!worker.ParseCapabilities(content, errorInfo))
        {
            cout << "XML解析失败！" << endl;
            continue;
        }
        end = chrono::high_resolution_clock::now();
        duration = chrono::duration_cast<chrono::milliseconds>(end - start);
        cout << "解析XML完成！耗时：" << duration.count() << " 毫秒" << endl;

        // 获取用户输入：图层名、瓦片矩阵集、格式、风格
        string layerTitle = "", tileMatrixSetName = "", format = "", style = "";
        if (!GetUserInput(worker, layerTitle, tileMatrixSetName, format, style))
        {
            continue;
        }

        // 获取有效范围
        //const BoundingBox viewExtent4326("EPSG:4326", 124.8250, 32.6901, 159.0153, 49.8245);
        const BoundingBox viewExtent4326("EPSG:4326", -180, -90, 180, 90);
        //const BoundingBox viewExtent4326("EPSG:4326", 108, 29, 116, 33);
        //const string geoCRS = "EPSG:2381";
        const string geoCRS = "EPSG:4326"; // 先纬度后经度

        const BoundingBox geoCRSBBox4326 = GetCSBoundingBox4326(geoCRS);
        if (!geoCRSBBox4326.IsValid())
        {
            cout << "解析geoCRS的boundingbox失败！" << endl;
            continue;
        }
        const BoundingBox layerBBox4326 = worker.GetLayerBoundingBox4326(layerTitle, tileMatrixSetName);
        if (!layerBBox4326.IsValid())
        {
            cout << "查找 " << layerTitle << " 的4326 boundingbox失败！" << endl;
            continue;
        }

        const BoundingBox validMapBBox4326 = GetBoundingBoxOverlap(geoCRSBBox4326, layerBBox4326);
        if (!validMapBBox4326.IsValid())
        {
            cout << "geoCRS范围和地图范围无交集！" << endl;
            continue;
        }

        BoundingBox validViewExtentBox4326 = GetBoundingBoxOverlap(validMapBBox4326, viewExtent4326);
        if (!validViewExtentBox4326.IsValid())
        {
            cout << "视口范围和有效地图范围无交集！" << endl;
            continue;
        }

        const string tileCRS = worker.GetLayerCRS(layerTitle, tileMatrixSetName);
        if (tileCRS.empty())
        {
            cout << "查找 " << layerTitle << " 瓦片所在的CRS失败！" << endl;
            continue;
        }
        BoundingBox validViewExtentBoxTileCRS(tileCRS, Rectangle());
        if (!CSConverter::TransformBoundingBox(validViewExtentBox4326, validViewExtentBoxTileCRS))
        {
            cout << "转换视口范围失败！" << endl;
            continue;
        }

        // 消除北向影响
        const double northVectorAngle = M_PI / 2; // 北向角度，默认朝上
        const double bboxScale = (1 - sqrt(2) / 2) / (M_PI / 4) * abs(northVectorAngle - M_PI / 4) + sqrt(2) / 2;
        validViewExtentBoxTileCRS.bbox = Rectangle(validViewExtentBoxTileCRS.bbox.GetMinPoint() * bboxScale, validViewExtentBoxTileCRS.bbox.GetMaxPoint() * bboxScale);

        // 计算瓦片信息
        vector<TileInfo> tiles = worker.CalculateTilesInfo(layerTitle, tileMatrixSetName, format, style, validViewExtentBoxTileCRS, url, false);

        // 下载和重投影瓦片
        start = chrono::high_resolution_clock::now();
        ThreadPool pool(6);
        vector<string> errorInfos(tiles.size());
        vector<string> resultTilesPath(tiles.size());
        for (size_t i = 0; i < tiles.size(); i++)
        {
            pool.Enqueue(DownloadAndReprojectTile, tiles[i], geoCRS, ref(errorInfos[i]), ref(resultTilesPath[i]), proxyUrl, "", "");
        }
        pool.WaitAll();

		// 检查下载结果，如果有错误，则使用xLinkHref中的请求链接再次尝试
        if (all_of(errorInfos.begin(), errorInfos.end(), [](const std::string& s) {
            return s == "Received XML instead of image data";
            }))
        {
            tiles = worker.CalculateTilesInfo(layerTitle, tileMatrixSetName, format, style, validViewExtentBoxTileCRS, url, true);
            for (size_t i = 0; i < tiles.size(); i++)
            {
                pool.Enqueue(DownloadAndReprojectTile, tiles[i], geoCRS, ref(errorInfos[i]), ref(resultTilesPath[i]), proxyUrl, "", "");
            }
            pool.WaitAll();
        }

        end = chrono::high_resolution_clock::now();
        duration = chrono::duration_cast<chrono::milliseconds>(end - start);
        cout << "下载和重投影完毕！耗时：" << duration.count() << " 毫秒" << endl;


        // 瓦片拼接
        string spliceImagePath = "";
        start = chrono::high_resolution_clock::now();
        GDALAllRegister();
        bool result = TileSplice(tiles, spliceImagePath);
        end = chrono::high_resolution_clock::now();
        duration = chrono::duration_cast<chrono::milliseconds>(end - start);
        if (result)
        {
            cout << "瓦片拼接成功！耗时：" << duration.count() << " 毫秒" << endl;
        }
        else
        {
            cout << "瓦片拼接失败！耗时：" << duration.count() << " 毫秒" << endl;
            continue;
        }

        // 拼接影像重投影到geoCRS
        string reprojectImagePath = "";
        start = chrono::high_resolution_clock::now();
        result = ReprojectImage(spliceImagePath, geoCRS, reprojectImagePath);
        end = chrono::high_resolution_clock::now();
        duration = chrono::duration_cast<chrono::milliseconds>(end - start);
        if (result)
        {
            cout << "重投影成功！耗时：" << duration.count() << " 毫秒" << endl;
        }
        else
        {
            cout << "重投影失败！耗时：" << duration.count() << " 毫秒" << endl;
            continue;
        }
    }
    return 0;
}
