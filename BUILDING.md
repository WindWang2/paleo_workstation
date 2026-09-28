# BUILDING

## 快速开始与 TTHW（plan §44.1）

```bash
./paleo-dev bootstrap   # preflight + 依赖 vendor（binary 路）
./paleo-dev build       # 配置 + 构建
./paleo-dev test        # QT_QPA_PLATFORM=offscreen 全套测试
```

- **TTHW 目标：vendor 引导完成后，首次绿色测试 2–5 分钟**（configure+build+
  ctest，8 核基线机；当前全套 84 测试（以 ctest -N 为准）实测 ~48s，余量给增量编译）。
- 引导本身（一次性）：binary 路 ~10min（OSGeo4W / deb 闭包 / onnxruntime
  pin）；superbuild 回退路 ≤2h、磁盘 ≥60GB——仅在 binary 路不可用时启用
  （`vendor/superbuild/README.md` 回退条款）。
- 本机已装 QGIS 4.2.x 开发包（如 Arch `qgis 4.2.2`）时 bootstrap 短路 QGIS
  腿，只剩 onnxruntime 下载。

## QGIS prefix 解析顺序（CMakeLists.txt:16 起）

主构建不写死 QGIS 位置，按序解析：

1. `QGIS_PREFIX` CMake 变量；未给则读环境 **`QGIS_PREFIX_PATH`**；
2. 命中后把它（及 `<prefix>/apps/qt6`，OSGeo4W 布局）前置进
   `CMAKE_PREFIX_PATH`，再在 `<prefix>/apps/qgis/include`、
   `<prefix>/include/qgis`、`<prefix>/usr/include/qgis` 等布局里找
   `qgsapplication.h` 与 `-lqgis_{core,gui,analysis}`；
3. 都没给 → 系统路径（`/usr/include/qgis`、`/usr/lib`）。

vendor 路径对照：

| 来源 | QGIS_PREFIX_PATH | 由谁准备 |
|---|---|---|
| 发行版系统包（Arch 等） | 不需要（系统路径即可） | 发行版包管理器 |
| qgis.org deb 闭包 | `<repo>/vendor/prefix/usr` | `./vendor/fetch-deps.sh`（lock 锁 SHA256） |
| OSGeo4W（Windows CI） | bootstrap 注入（`apps/qgis` 布局） | `./paleo-dev.ps1 bootstrap` |
| superbuild（回退，未启用） | `<repo>/vendor/superbuild/prefix` | `vendor/superbuild/`（见其 README） |

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
- 全套 ctest（wave4 起 62 测试，offscreen）：~48s（TTHW 预算内）；设
  `PALEO_REAL_PROJECT_AREA` 跑真数据档时全套 ~110–130s（wave3 实测
  109.3s/133.1s）
