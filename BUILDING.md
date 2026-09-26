# BUILDING

## 平台 × 版本矩阵

| 平台 | 状态 | 依赖来源 |
|------|------|----------|
| Linux x86_64 (glibc≥2.41: Debian13/Ubuntu26.04/Arch) | Arch 本机通过；Ubuntu 26.04 CI | qgis.org deb 闭包 → `vendor/prefix/usr`；或发行版原生 QGIS 4.2.x 开发包 |
| Windows x86_64 | CI leg（本机未实测） | OSGeo4W `qgis` + `qgis-devel` 4.2.x + `qt6-devel`，MSVC /MD |
| 更低 glibc 宿主 | 不支持 | 回退条款：ExternalProject superbuild-on-oldest-target |

## 依赖（vendor manifest pin）

QGIS 4.2.x · Qt ≥6.6 · GDAL · PROJ · GEOS · QCA-qt6 · QtKeychain-qt6 · libspatialindex · exiv2 · libzip · OpenSSL · sqlite3/spatialite · **ONNX Runtime 1.30.0**（官方 release，sha256 `a5ed5a3c…3b3fd`，manylinux_2_28）。Ubuntu 26.04 的 deb 完整闭包和 SHA-256 在 `vendor/deb-closure.lock`；OSGeo4W 安装器摘要在 `vendor/manifest.json`。

## 常见失败表

| 现象 | 原因 | 修复 |
|------|------|------|
| `QDomDocument: No such file` | 缺 Qt6Xml 等模块 dev 包 | 装 qt6 全组 dev（见 CMakeLists find_package 列表） |
| `defaulted operator==` 报错 | C++ 标准 <20 | CMAKE_CXX_STANDARD=20（已钉） |
| `providerList() empty` | prefix 未指向 vendor/qgis 资源根 | selfcheck 检查 setPrefixPath + QGIS_PREFIX 环境 |
| srs.db missing | vendor prefix 缺 share/qgis/resources | 重跑 `paleo-dev bootstrap` |
| `GDALPolygonize` 无 geotransform 警告 | 测试栅格无仿射 | GDAL 3.11+ 需 SRC_METHOD=NO_GEOTRANSFORM（已知，警告非阻塞） |

## 升级流程

Debian 闭包升级时，在目标发行版且已配置 QGIS 官方 apt 源的干净环境运行 `./vendor/fetch-deps.sh --update-lock --print-only`，审查并提交新的 `vendor/deb-closure.lock`。正常 bootstrap 只读取锁文件，并仅解包锁内经过大小和 SHA-256 校验的文件。ONNX Runtime 升级则更新 `vendor/manifest.json` 的版本和摘要。Windows bootstrap 校验安装器 SHA-256，并拒绝 QGIS 主版本不符；OSGeo4W 包闭包仍由其安装器选择，升级时须复核 CI。

## 实测值（Phase 0，本机 Arch/qgis-4.2.2 已装）

- 首次 configure+build（3 个目标）：<1min（依赖已装）
- `tst_boot`：init+providers+srs.db+渲染 非均匀像素断言 — PASS（102ms）
- `tst_polygonize`：provider 注册+GDALPolygonize C++ 路径 — PASS
- `spikes/onnx/ort_check`：进程内 toy 推理 2.0→42.0 — PASS
