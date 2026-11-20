# Linux 源码依赖闭包

源码路线面向 Linux x86_64，使用 Debian 13（glibc 2.41）的固定 Qt 和开发库归档
构建 GIS 栈。本机的编译器、glibc、构建工具和代码生成器提供宿主接口。
CMake 查找项目库、头文件和包配置时使用项目安装前缀。

## 启用步骤

使用 CMake 3.28+、Ninja、GCC/C++20、Make、dpkg-dev、pkgconf、patchelf、
Python、Flex、Bison、Tcl 和 curl。在需支持的最旧 glibc 上完成源码编译与验收。

```bash
bash vendor/fetch-deps.sh --superbuild-base
cmake -S vendor/superbuild -B vendor/build/superbuild -G Ninja -DSUPERBUILD_JOBS=4
source vendor/superbuild/activate.sh --build
cmake --build vendor/build/superbuild --parallel 1
```

Debian 归档缓存位于 `vendor/cache/superbuild-debs`，源码归档缓存位于
`vendor/cache/tarballs`。首次联网准备后，在新工作树共享归档缓存并设置
`PALEO_VENDOR_OFFLINE=1`，配置时传 `-DSUPERBUILD_OFFLINE=ON`，即可重放归档校验、
解包和源码构建。ExternalProject 记录每个成功步骤，可从上次成功步骤继续。
底座、源码锁、配方、补丁或构建选项变更时，请同时选择新的构建目录和 `SUPERBUILD_PREFIX`。安装完成后默认目标写入源码凭据。开发、配置及打包入口均核对该凭据。`activate.sh --build` 仅用于首次编译依赖。

安装前缀包含 Qt 工具、WebEngine 子进程、插件、字体、GIS 库及资源。主仓由该
前缀编译，ONNX Runtime 与 Office 组件按各自供货入口准备。

```bash
python3 vendor/fetch-onnxruntime.py
bash vendor/fetch-ranuts-document.sh --ensure
source vendor/superbuild/activate.sh
./paleo-dev configure -DCMAKE_BUILD_TYPE=Release -DPALEO_DEPENDENCY_PREFIX="$PALEO_SOURCE_PREFIX"
./paleo-dev build
./paleo-dev test -j 4 -LE perf
./paleo-dev selfcheck
```

## 裁剪决定（依据 = 代码取证，2026-09-26）

| 开关 | 值 | 依据 |
|---|---|---|
| `WITH_3D` | OFF | `src/` 无 `qgis_3d`/`Qgs3D` 引用（grep 零命中） |
| `WITH_MESH` | —（无此开关） | QGIS 无 `WITH_MESH` option（4.2.2 首次实证、4.2.3 升级时重新实证：tarball CMakeLists `WITH_MESH` 零命中，不存在该 cache 变量；mesh 已并入 core，无裁剪面）——原「`src/` 无 `QgsMesh` 引用」的裁剪意图无从落地 |
| `WITH_PDAL` | OFF | `src/` 无 pdal 引用；产品面无点云（plan §10–12 算法面亦无） |
| `WITH_APP` | OFF | Paleo 只链 `qgis_core`/`qgis_gui`/`qgis_analysis`，不装桌面 App；QGIS 4.2.3 无 `WITH_APP` option（4.2.3 tarball 实证：`WITH_DESKTOP` 为真名），本仓开关实际映射其真名 `WITH_DESKTOP=FALSE` |
| `WITH_GUI` | ON | 嵌入式 GUI（`paleo_qgis_iface` 链 `qgis_gui`） |
| `WITH_ANALYSIS` | ON | 主 `CMakeLists.txt:37` 链接 `qgis_analysis` |
| `WITH_PYTHON`、`WITH_BINDINGS`、`WITH_QGIS_PROCESS` | OFF | 部署 QGIS C++ 库和 provider。Paleo Agent Python 使用独立供货。 |
| `ENABLE_TESTS` | OFF | 本地应用回归和 provider、自检仍通过主项目入口运行。 |

## 归档与依赖

- `debian13-base.lock` 固定 Qt、插件、字体和所需开发库的 Debian 归档。根据
  `debian13-base.seeds` 在干净 Debian 13 环境用签名包索引解析依赖，对照 SHA-256
  记录，排除宿主基础 ABI 与由源码替换的 GIS、zlib、Zstd 和 SQLite 归档。
- `debian-sources.lock` 固定 zlib 与 SQLite 的源码、Debian 补丁和安全更新；
  Zstd、GEOS 3.15.0、PROJ 9.8.1、GDAL 3.13.3、QGIS 4.2.3 的源码在
  `CMakeLists.txt` 中钉版本和 SHA-256。
- 依赖顺序为 zlib → Zstd → SQLite → GEOS → PROJ → GDAL → QGIS。QGIS
  版本与锁定的二进制供货路线同步，由 `tools/check_qgis_versions.py` 检查。
- Qt 的来源和许可说明随归档保存在前缀的 `share/doc`，以共享库链接。
  `qt6-svg-plugins` 单独锁定，提供 QGIS 主题图标所需的 SVG 图标引擎和图像插件。

## 补丁（patches/）

- **`qgis-4.2.3-labels-with-layer.patch`** — Paleo「标注随图层 z 序」。
  QGIS 原生把所有标注放在渲染末尾统一绘制（永远置顶），下层的井位/顶点
  标注会「穿透」压在它们之上的多边形与栅格。补丁给地图渲染任务加了按层
  开关：图层自定义属性 `rendering/labelsWithLayer=true` 时，该层注册到
  一个独占的 `QgsDefaultLabelingEngine`（`LayerRenderJob::layerLabelingEngine`），
  层渲染完成后立即把标注画进该层自己的渲染目标（image/picture/目标
  painter），合成顺序因此严格按图层树。选择性掩膜的标注**源**层（文本
  mask / label mask source set）与 staged render（GeoPDF 导出）引擎保持
  共享引擎不变。补丁在 `qgsmaprendererjob.h` 里留下宏
  `QGIS_PALEO_LABELS_WITH_LAYER` 作编译期探测。
  由 `ExternalProject_Add(qgis PATCH_COMMAND ...)` 经
  `patches/apply-patch.cmake` 幂等应用（marker 已在则跳过；打不上即
  FATAL_ERROR）。
  **版本履历**：4.2.2 → 4.2.3（方向 71，2026-10-07）——内容零改动仅随
  版本重命名；对 4.2.3 官方 tarball 逐文件验证：`patch -p1 --dry-run`
  干净通过（无 fuzz、四个触及文件全中），实弹应用后
  `QGIS_PALEO_LABELS_WITH_LAYER` 宏与 `layerLabelingEngine` 接线落位。
  **降级契约**：binary 兜底路线（`vendor/fetch-deps.sh` deb 闭包 /
  OSGeo4W / 发行版包）的 QGIS **没有**此补丁——Paleo 侧必须
  `#ifdef QGIS_PALEO_LABELS_WITH_LAYER` 探测并对未打补丁的构建优雅降级
  （此时 `rendering/labelsWithLayer` 是无害的空属性，标注回到原生置顶）。
