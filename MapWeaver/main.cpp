#include <QtCore/QCoreApplication>
#include <iostream>
#include <mutex>
#include "WMSCapabilities.h"
#include "Network.h"
#include "ThreadPool.h"
#include "ogr_spatialref.h"
#include "ogr_geometry.h"
#include <gdal_priv.h>

using namespace std;
int main(int argc, char *argv[])
{
    //const string url = "https://tiles.geovisearth.com/base/v1/wmts/GetCapabilities?tmsIds=w&token=1b6ea8d6e66808585e20b92eaed02c5edd0349a090b3b186b39e2b6f5074b51f";
    //const string url = "https://data.geopf.fr/wms-r/wms?SERVICE=WMS&VERSION=1.3.0&REQUEST=GetCapabilities";
    const string url = "http://www.ign.es/wmts/pnoa-ma?SERVICE=WMTS&REQUEST=GETCAPABILITIES";
   
    const string proxyUrl = "http://127.0.0.1:10808";

    auto start = chrono::high_resolution_clock::now();
    string content = "", errorInfo = "";
    const bool downloadSuccess = WMSCapabilitiesDownloader::DownloadCapabilitiesXML(url, content, errorInfo, proxyUrl);
    auto end = chrono::high_resolution_clock::now();
    auto duration = chrono::duration_cast<chrono::milliseconds>(end - start);
    cout << "下载XML耗时：" << duration.count() << " 毫秒" << endl;
    if (!downloadSuccess)
    {
        cout << errorInfo << endl;
        return 0;
    }
    cout << "下载完成！" << endl;

    start = chrono::high_resolution_clock::now();
    CSConverter::Initial(GetProjDirPath());
    WMSCapabilitiesWorker worker;
    if (!worker.ParseCapabilities(content, errorInfo))
    {
        cout << "解析失败！" << endl;
        return 0;
    }
    end = chrono::high_resolution_clock::now();
    duration = chrono::duration_cast<chrono::milliseconds>(end - start);
    cout << "解析XML耗时：" << duration.count() << " 毫秒" << endl;
    cout << "解析完成!" << endl;

    // 确定地图图层
    const vector<string> layerTitles = worker.GetAllLayerTitle();
    if (layerTitles.empty())
    {
		cout << "未找到图层！" << endl;
		return 0;
    }
    cout << "图层列表：" << endl;
    for (const string& layerName : layerTitles)
    {
        cout << layerName << endl;
    }

    string curLayerTitle = "";
    while (true)
    {
        cout << endl << endl << "选择图层名：" << endl;
        getline(cin, curLayerTitle);
        if (find(layerTitles.begin(), layerTitles.end(), curLayerTitle) == layerTitles.end())
        {
            cout << "图层不存在！" << endl;
            continue;
        }
        break;
    }
    cout << endl << endl;

    // 确定瓦片矩阵集（哪一个瓦片矩阵CRS）
    string tileMatrixSetName = "";
    if (worker.IsWMTSLayer(curLayerTitle))
    {
        const vector<string> tileMatrixSets = worker.GetLayerAllTileMatrixSets(curLayerTitle);
        if (tileMatrixSets.empty())
        {
            cout << "该图层不存在瓦片矩阵集！" << endl;
            return 0;
        }

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

    // 确定格式
    const vector<string> formats = worker.GetLayerFormats(curLayerTitle);
	if (formats.empty())
	{
		cout << "未找到任何格式！" << endl;
		return 0;
	}
    cout << endl << "格式列表：" << endl;
    for (const string& format : formats)
    {
        cout << format << endl;
    }
    string format = "";
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

    // 确定风格
    const vector<string> styles = worker.GetLayerStyles(curLayerTitle);
    string style = "";
    if (styles.size() == 1)
    {
        style = styles[0];
        cout << "默认使用唯一风格: " << style << endl;
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

    //const BoundingBox viewExtent4326("EPSG:4326", 124.8250, 32.6901, 159.0153, 49.8245);
    const BoundingBox viewExtent4326("EPSG:4326", -180, -90, 180, 90);
    //const BoundingBox viewExtent4326("EPSG:4326", 108, 29, 116, 33);
    //const string geoCRS = "EPSG:2381";
    const string geoCRS = "EPSG:4326"; // 先纬度后经度

    const BoundingBox geoCRSBBox4326 = GetCSBoundingBox4326(geoCRS);
    if (!geoCRSBBox4326.IsValid())
    {
        cout << "解析geoCRS的boundingbox失败！" << endl;
        return 0;
    }
    const BoundingBox layerBBox4326 = worker.GetLayerBoundingBox4326(curLayerTitle, tileMatrixSetName);
    if (!layerBBox4326.IsValid())
    {
        cout << "查找 " << curLayerTitle << " 的4326 boundingbox失败！" << endl;
        return 0;
    }

	const BoundingBox validMapBBox4326 = GetBoundingBoxOverlap(geoCRSBBox4326, layerBBox4326);
    if (!validMapBBox4326.IsValid())
    {
        cout << "geoCRS范围和地图范围无交集！" << endl;
        return 0;
    }

    BoundingBox validViewExtentBox4326 = GetBoundingBoxOverlap(validMapBBox4326, viewExtent4326);
    if (!validViewExtentBox4326.IsValid())
    {
        cout << "视口范围和有效地图范围无交集！" << endl;
        return 0;
    }

    const string tileCRS = worker.GetLayerCRS(curLayerTitle, tileMatrixSetName);
	if (tileCRS.empty())
	{
		cout << "查找 " << curLayerTitle << " 瓦片所在的CRS失败！" << endl;
		return 0;
	}
    BoundingBox validViewExtentBoxTileCRS(tileCRS, Rectangle());
	if (!CSConverter::TransformBoundingBox(validViewExtentBox4326, validViewExtentBoxTileCRS))
	{
		cout << "转换视口范围失败！" << endl;
		return 0;
	}

    // 消除北向影响
    const double northVectorAngle = M_PI / 2; // 北向角度，默认朝上
    const double bboxScale = (1 - sqrt(2) / 2) / (M_PI / 4) * abs(northVectorAngle - M_PI / 4) + sqrt(2) / 2;
    validViewExtentBoxTileCRS.bbox = Rectangle(validViewExtentBoxTileCRS.bbox.GetMinPoint() * bboxScale, validViewExtentBoxTileCRS.bbox.GetMaxPoint() * bboxScale);

    // 计算瓦片信息
    const vector<TileInfo> tiles = worker.CalculateTilesInfo(curLayerTitle, tileMatrixSetName, format, style, validViewExtentBoxTileCRS, url);

	// 下载瓦片
    start = chrono::high_resolution_clock::now();
    ThreadPool pool(8);
	vector<string> errorInfos(tiles.size());
    for (size_t i = 0; i < tiles.size(); i++)
    {
        pool.Enqueue(DownloadImageMultiThread, tiles[i].url, tiles[i].filePath, ref(errorInfos[i]), proxyUrl, "", "");
    }
    pool.WaitAll();
    end = chrono::high_resolution_clock::now();
    duration = chrono::duration_cast<chrono::milliseconds>(end - start);
    cout << "下载完毕！耗时：" << duration.count() << " 毫秒" << endl;

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
        return 0;
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
        return 0;
    }

    return 0;
}
