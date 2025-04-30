# MapWeaver ✨

MapWeaver 是一个基于 C++ 的高性能地理空间工具，集成了 GDAL、CURL 和 OpenSSL 等强大库，旨在从 WMS/WMTS 服务中获取地图瓦片，自动拼接、重投影并裁剪为视口范围内的影像。​该工具适用于需要高效地图数据处理的 GIS 开发者、遥感分析师和地理信息系统集成商。​

## 🚀 功能亮点

* 多协议支持：​兼容 WMS 和 WMTS 服务，灵活应对不同地图服务提供商。

* 自动化处理：​实现地图瓦片的自动下载、拼接、重投影和裁剪，简化数据准备流程。

* 高性能设计：​利用 C++ 的高效性，确保大规模地图数据处理的性能需求。

* 安全通信：​通过 OpenSSL 实现 HTTPS 支持，保障数据传输的安全性。

* 支持后续操作：流程的最后会生成/写入 GeoPackages 数据库，方便进行后续工作流。

## 🛠️ 构建与运行

**依赖项**

* [GDAL](https://gdal.org/en/stable/)

* [libcurl](https://curl.se/libcurl/)

* [OpenSSL](https://www.openssl.org/)

**构建步骤**

1. 克隆仓库。

```
git clone https://github.com/RayShark0605/MapWeaver.git
```

2. 打开 `MapWeaver.sln` 解决方案文件（适用于 `Visual Studio`）。
   
3. 配置并确保上述依赖项已正确安装并链接。

4. 构建解决方案并运行。

## 📂 项目结构

* `MapWeaver/`：核心源代码目录。

* `3rdParty/`：第三方库和依赖项。

* `x64/`：64 位构建配置文件。

* `MapWeaver.sln`：Visual Studio 解决方案文件

## 📄 许可证

本项目采用 MIT 许可证。

## 🤝 贡献指南
欢迎社区开发者提出问题、提交拉取请求或提供改进建议。​
