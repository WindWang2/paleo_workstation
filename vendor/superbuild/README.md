# vendor/superbuild — ExternalProject 回退路线（骨架）

状态：**骨架（Phase 0 §39/E3 交付物）——未启用，不承诺完整构建。**
默认依赖路线是 binary vendoring：`vendor/bootstrap.sh`（OSGeo4W / onnxruntime）
与 `vendor/fetch-deps.sh`（qgis.org deb 闭包 → `vendor/prefix`）。对比与决策
记录见 `docs/phase0/et0-vendor-comparison.md`（D7：批准 binary vendoring）。

## 何时启用本路线（回退条款）

满足**任一**条件才启用（plan §39 回退条款）：

1. `fetch-deps.sh` 的 `apt-get download libqgis-dev=4.2.*` 验证失败（上游
   包不可得 / SHA256 对不上且无旧档）；
2. 必须支持比 binary floor 更老的宿主——binary 路要求 glibc ≥ 2.41（Debian
   13 / Ubuntu 25.04 级）；superbuild 即 "superbuild-on-oldest-target"。

不满足时本目录保持骨架状态。启用动作本身要留决策记录（issue/PR body）。

## 启用步骤

1. 回填 `CMakeLists.txt` 里四个 `*_URL`（GEOS/PROJ/GDAL/QGIS 源码 tarball），
   同时给每个 `ExternalProject_Add` 补 `URL_HASH SHA256=<...>`——URL 与
   SHA256 一起才算钉死（pin 后 TTHW 不依赖上游 URL 存活）。
2. preflight 追加检查（plan §44.1 superbuild 档）：flex、bison、nasm、python3。
3. 配置与构建（ninja 增量 = 断点续跑；**不手写 `.done` 标记**，进度即
   ExternalProject stamp 目录）：

   ```bash
   cmake -S vendor/superbuild -B vendor/build/superbuild -G Ninja
   ninja -C vendor/build/superbuild
   ```

4. 主构建指向产物 prefix：

   ```bash
   QGIS_PREFIX_PATH=<repo>/vendor/superbuild/prefix cmake -S . -B build -G Ninja
   ```

   （`CMakeLists.txt:16` 起的 `QGIS_PREFIX`/`QGIS_PREFIX_PATH` 解析已就位。）

预算：8 核基线机 ≤ 2h 无人值守（plan §44.1）；磁盘 ≥ 60GB。

## 裁剪决定（依据 = 代码取证，2026-09-26）

| 开关 | 值 | 依据 |
|---|---|---|
| `WITH_3D` | OFF | `src/` 无 `qgis_3d`/`Qgs3D` 引用（grep 零命中） |
| `WITH_MESH` | OFF | `src/` 无 `QgsMesh` 引用（grep 零命中） |
| `WITH_PDAL` | OFF | `src/` 无 pdal 引用；产品面无点云（plan §10–12 算法面亦无） |
| `WITH_APP` | OFF | Paleo 只链 `qgis_core`/`qgis_gui`/`qgis_analysis`，不装桌面 App |
| `WITH_GUI` | ON | 嵌入式 GUI（`paleo_qgis_iface` 链 `qgis_gui`） |
| `WITH_ANALYSIS` | ON | 主 `CMakeLists.txt:37` 链接 `qgis_analysis` |

## 依赖清单（plan §39「Vendor 依赖清单」的落地位置）

- **源码构建**（ExternalProject，按依赖序）：GEOS → PROJ（含 proj.db 数据）
  → GDAL → QGIS 4.2.x。版本钉位在启用时随 URL 回填。
- **发行版提供**：Qt6（qtbase/qttools/svg/imageformats；**禁止**
  qt-everywhere 整块编译）、X11/GL dev 头、OpenSSL 头、flex/bison/nasm/python
  （preflight 档）。
- **运行时数据文件**：proj.db、GDAL data、srs.db、SVG symbols、Qt
  platform/imageformat 插件（binary 路已随闭包；superbuild 路由各库的
  install 规则落进同一 prefix）。
- **ONNX Runtime**：不进 superbuild——`vendor/manifest.json` 的 GitHub release
  pin（SHA256）与两条 QGIS 路线正交。

## Qt LGPL 条目（决策 D-QT）

Qt 以动态链接方式使用（发行版共享库 / OSGeo4W 同源），不静态链接、不裁剪
重分发——LGPL §4(a) 动态链接路径满足合规；对应义务（提供可重链目标文件或
指明获取方式）由 binary 路的发行版包机制覆盖。manifest 一条记录即可
（`vendor/manifest.json` 的 OSGeo4W `qt6-devel` 条目即此用途）。
