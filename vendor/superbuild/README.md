# vendor/superbuild — ExternalProject 自编译路线（策略首选）

状态：**首选依赖路线（2026-10-01 政策升级）——四钉位（GEOS/PROJ/GDAL/QGIS）
URL+SHA256 已回填（QGIS 4.2.3，方向 71 依赖口径统一：与 deb 闭包
4.2.3+44resolute 同上游版本；三路一致性由 tools/check_qgis_versions.py
看守）；端到端可用性尚未在干净环境验证（无 CI 覆盖，见 BUILDING.md）。**
2026-10-01 起「尽量不依赖系统库、尽量自编译 vendored」是仓库级依赖策略
（BUILDING.md「依赖来源策略」），本路线取代 binary vendoring 成为首选；
`vendor/fetch-deps.sh`（deb 闭包）/ OSGeo4W 降为加速档，发行版系统包仅兜底。
历史对比与旧决策（D7：批准 binary vendoring）见
`docs/phase0/et0-vendor-comparison.md`（保留作底账，优先级以新政策为准）。

## 启用依据（原回退条款，2026-10-01 重写）

原回退条款要求满足下列任一条件才启用，现已作废为历史记录：

1. `fetch-deps.sh` 的 `apt-get download libqgis-dev=4.2.*` 验证失败（上游
   包不可得 / SHA256 对不上且无旧档）；
2. 必须支持比 binary floor 更老的宿主——binary 路（已提交的 deb 闭包锁）
   要求 glibc ≥ 2.43（Ubuntu 26.04 级；Debian 13 的 glibc 2.41 不够）；
   superbuild 即 "superbuild-on-oldest-target"。

新政策下本路线即默认路线，无需触发条件；启用动作仍要留决策记录
（issue/PR body）。

## 启用步骤

1. `CMakeLists.txt` 里四个 `*_URL`（GEOS 3.15.0 / PROJ 9.8.1 / GDAL 3.13.3 /
   QGIS 4.2.3）与对应 `URL_HASH SHA256=<...>` 已成对钉死；升级版本时两者
   必须一起改（pin 后 TTHW 不依赖上游 URL 存活），且 `vendor/cache/tarballs/`
   的缓存 tarball 一并更新。
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
| `WITH_MESH` | —（无此开关） | QGIS 无 `WITH_MESH` option（4.2.2 首次实证、4.2.3 升级时重新实证：tarball CMakeLists `WITH_MESH` 零命中，不存在该 cache 变量；mesh 已并入 core，无裁剪面）——原「`src/` 无 `QgsMesh` 引用」的裁剪意图无从落地 |
| `WITH_PDAL` | OFF | `src/` 无 pdal 引用；产品面无点云（plan §10–12 算法面亦无） |
| `WITH_APP` | OFF | Paleo 只链 `qgis_core`/`qgis_gui`/`qgis_analysis`，不装桌面 App；QGIS 4.2.3 无 `WITH_APP` option（4.2.3 tarball 实证：`WITH_DESKTOP` 为真名），本仓开关实际映射其真名 `WITH_DESKTOP=FALSE` |
| `WITH_GUI` | ON | 嵌入式 GUI（`paleo_qgis_iface` 链 `qgis_gui`） |
| `WITH_ANALYSIS` | ON | 主 `CMakeLists.txt:37` 链接 `qgis_analysis` |

## 依赖清单（plan §39「Vendor 依赖清单」的落地位置）

- **源码构建**（ExternalProject，按依赖序）：GEOS → PROJ（含 proj.db 数据）
  → GDAL → QGIS（当前 4.2.3，与 deb 闭包同上游版本——方向 71 统一，
  升级须三路口径一起动，护栏 `tools/check_qgis_versions.py` 拦漏改）。
- **发行版提供**：Qt6（qtbase/qttools/svg/imageformats；**禁止**
  qt-everywhere 整块编译）、X11/GL dev 头、OpenSSL 头、flex/bison/nasm/python
  （preflight 档）。
- **运行时数据文件**：proj.db、GDAL data、srs.db、SVG symbols、Qt
  platform/imageformat 插件（binary 路已随闭包；superbuild 路由各库的
  install 规则落进同一 prefix）。
- **ONNX Runtime**：不进 superbuild——`vendor/manifest.json` 的 GitHub release
  pin（SHA256）与两条 QGIS 路线正交。

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

## Qt LGPL 条目（决策 D-QT）

Qt 以动态链接方式使用（发行版共享库 / OSGeo4W 同源），不静态链接、不裁剪
重分发——LGPL §4(a) 动态链接路径满足合规；对应义务（提供可重链目标文件或
指明获取方式）由 binary 路的发行版包机制覆盖。manifest 一条记录即可
（`vendor/manifest.json` 的 OSGeo4W `qt6-devel` 条目即此用途）。
