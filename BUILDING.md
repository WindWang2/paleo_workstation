# BUILDING

## 快速开始与 TTHW（plan §44.1）

```bash
./paleo-dev bootstrap   # preflight + 依赖 vendor（binary 路）
./paleo-dev build       # 配置 + 构建
./paleo-dev test        # QT_QPA_PLATFORM=offscreen 全套测试
```

- **TTHW 目标：vendor 引导完成后，首次绿色测试 2–5 分钟**（configure+build+
  ctest，8 核基线机；全套测试数以 `ctest -N` 为准，不在此写死，实测 ~48s，余量给增量编译）。
- 引导本身（一次性）：binary 加速档 ~10min（OSGeo4W / deb 闭包 / onnxruntime
  pin）；superbuild 首选路 ≤2h、磁盘 ≥60GB（启用步骤见
  `vendor/superbuild/README.md`，政策见下「依赖来源策略」）。
- 本机已装 QGIS 4.2.x 开发包（如 Arch `qgis 4.2.2`）时 bootstrap 短路 QGIS
  腿，只剩 onnxruntime 下载——注意这属**兜底档**，见下「依赖来源策略」。

## 依赖来源策略（2026-10-01 起）

**原则：尽量不依赖系统库，尽量自编译 vendored。** 优先级：

1. **自编译 vendor（superbuild，首选）**——GEOS→PROJ→GDAL→QGIS 按源码
   tarball + SHA256 pin（`URL_HASH`）构建进 `vendor/superbuild/prefix`
   （启用步骤见其 README；尚无 CI 覆盖，端到端可用性未在干净环境验证）。动机：系统包状态不受本仓
   控制——发行版升级/卸载即破坏构建与运行（2026-10-01 本机实证：Arch
   系统 qgis/cmake 被卸载后，二进制缺 `libqgis_core.so.4.2.2` 无法启动，
   构建工具链同步失踪）；自编译 prefix 把版本、ABI、裁剪面钉进仓库，
   任何机器可复现。
2. **钉哈希的二进制 vendor 闭包（加速档）**——`vendor/prefix`（deb 闭包）
   / OSGeo4W（Windows）。仍是仓控 prefix 而非系统库；TTHW 快，作为 CI
   与新机的务实选择保留。
3. **系统包（仅兜底）**——发行版 QGIS 4.2.x 开发包只在上述两路都不可用
   时作临时兜底；CI 与发布构建禁止依赖系统包提供 QGIS/GDAL/PROJ/GEOS。

> **现状与策略的差距（#76）**：目前 `.github/workflows/ci.yml` 的 Linux job
> 仍从 qgis.org apt 源安装系统 QGIS 包，并未走上面 1/2 路——即 CI 实际
> 处于第 3 档，与本策略矛盾，待迁移。`fetch-deps.sh --check-urls` 可做锁
> 文件可达性冒烟（只 HEAD 探测，不下载）。

例外（不 vendored，沿用系统/官方二进制）：Qt6（体积与构建时长，
superbuild 明示禁止 qt-everywhere 整块编译；走发行版或 OSGeo4W 同源）、
编译器工具链与构建依赖（flex/bison/nasm/python3）、glibc/libstdc++
（ABI floor，无法 vendored）、ONNX Runtime（官方 release SHA256 pin，
与 QGIS 路线正交）。

## QGIS prefix 解析顺序（CMakeLists.txt:16 起）

主构建不写死 QGIS 位置，按序解析：

1. `QGIS_PREFIX` CMake 变量；未给则读环境 **`QGIS_PREFIX_PATH`**；
2. 命中后把它（及 `<prefix>/apps/qt6`，OSGeo4W 布局）前置进
   `CMAKE_PREFIX_PATH`，再在 `<prefix>/apps/qgis/include`、
   `<prefix>/include/qgis`、`<prefix>/usr/include/qgis` 等布局里找
   `qgsapplication.h` 与 `-lqgis_{core,gui,analysis}`；
3. 都没给 → 系统路径（`/usr/include/qgis`、`/usr/lib`）——仅兜底档。

命中的 `QGIS_PREFIX` 还会编译进二进制（`PALEO_QGIS_PREFIX_DEFAULT`）：
运行时 `QGIS_PREFIX_PATH` env 未设时，provider/srs.db 默认解析到构建所链的
prefix（而非硬编码 `/usr`），vendored 库自身带 `$ORIGIN` RUNPATH——裸跑
`build/paleo` 也是全 vendor 栈。ctest 沙箱同样注入 `QGIS_PREFIX_PATH`。

vendor 路径对照（按策略优先级）：

| 来源 | QGIS_PREFIX_PATH | 由谁准备 |
|---|---|---|
| superbuild 自编译（首选） | `<repo>/vendor/superbuild/prefix` | `vendor/superbuild/`（见其 README） |
| qgis.org deb 闭包（加速档） | `<repo>/vendor/prefix/usr` | `./vendor/fetch-deps.sh`（lock 锁 SHA256） |
| OSGeo4W（Windows CI） | bootstrap 注入（`apps/qgis` 布局） | `./paleo-dev.ps1 bootstrap` |
| 发行版系统包（仅兜底） | 不需要（系统路径即可） | 发行版包管理器 |

## 平台 × 版本矩阵

| 平台 | 状态 | 依赖来源 |
|------|------|----------|
| Linux x86_64：deb 闭包需 glibc≥2.43（Ubuntu 26.04 / 同代 Arch）；Debian 13（glibc 2.41）不能运行该闭包，须走 superbuild | Arch 本机通过；Ubuntu 26.04 CI（系统包，见上） | superbuild 自编译 prefix（首选）→ deb 闭包 `vendor/prefix/usr`（加速档）；发行版 QGIS 4.2.x 仅兜底 |
| Windows x86_64 | CI leg（本机未实测） | OSGeo4W `qgis` + `qgis-devel` 4.2.x + `qt6-devel`，MSVC /MD |
| 更低 glibc 宿主 | 不支持 | superbuild-on-oldest-target（ExternalProject） |

## 依赖（vendor manifest pin）

QGIS 4.2.x · Qt ≥6.6 · GDAL · PROJ · GEOS · QCA-qt6 · QtKeychain-qt6 · libspatialindex · exiv2 · libzip · OpenSSL · sqlite3/spatialite · **ONNX Runtime 1.30.0**（官方 release，sha256 `a5ed5a3c…3b3fd`，manylinux_2_28）。Ubuntu 26.04 的 deb 完整闭包和 SHA-256 在 `vendor/deb-closure.lock`（库解包在 `vendor/prefix/usr/lib/<multiarch>`，`./paleo-dev` 已加入 `LD_LIBRARY_PATH`）；Ubuntu pool 会删除被安全更新取代的旧版本，因此 `fetch-deps.sh` 在 pool 404 时回退到 `https://snapshot.ubuntu.com/ubuntu/<ts>/`，时间戳记录在 `vendor/deb-closure.snapshot`（`--update-lock` 会刷新；可用 `PALEO_DEB_SNAPSHOT` 覆盖），内容仍由锁内 SHA-256 校验；OSGeo4W 安装器摘要在 `vendor/manifest.json`。

## 测试布线约定（devex）

- **注册**：`add_paleo_test(name [LIBS ...])`（CMakeLists.txt）。单参 = 伞式
  旧式契约，回退链 `paleo_core`（cmake/extra-*.cmake 并行方向零改动可用）；
  `LIBS` 给该测试 include 面的最小闭包——touch 单模块不再牵连全部测试
  relink（对比数据 docs/progress/devex.md）。首模块决定 ctest `LABELS`
  （`ctest -L io` / `-LE core` 分步筛选）。
- **QGIS prefix**：测试 main() 统一读环境 `QGIS_PREFIX_PATH`（缺省 `/usr`）；
  `./paleo-dev test` 在 deb 闭包路（`vendor/prefix` 存在）自动注入 prefix 与
  `LD_LIBRARY_PATH`。
- **并行安全**：每个 ctest 项自动获得独立 `XDG_CONFIG_HOME`/`XDG_DATA_HOME`/
  `HOME` 沙箱（`build/ctest-home/<test>/`），QSettings 不再互踩真实用户配置
  ——`ctest -j$(nproc)` 默认安全。**已知边界：Windows NativeFormat 走注册表
  不受 env 控制**，Windows 侧保持串行 ctest。
- **层护栏**：`ctest -R layering`（提示）/`layering_strict`（防回升闸门：
  baseline 非空或可收缩即红）/`layering_selftest`（扫描器自检）。

## 加速与实验档（devex）

| 旋钮 | 说明 |
|------|------|
| ccache/sccache | PATH 上有即自动挂 launcher（`-DCMAKE_CXX_COMPILER_LAUNCHER=` 显式指定优先；`PALEO_NO_CCACHE=1` 关）。CI 缓存 `~/.cache/ccache` |
| `PALEO_ENABLE_ASAN=ON` | ASAN 实验档；测试注入 `ASAN_OPTIONS=detect_leaks=0`（QGIS/Qt 设计内"泄漏"面，suppressions 见 `tools/lsan-suppressions.txt`） |
| `PALEO_ENABLE_UBSAN=ON` | UBSAN 巡检档（可恢复，打印栈） |
| `PALEO_UNITY_BUILD=ON` | 实验 unity build：逐产品 target、vendor 三家（sbm/segyio/saribbon）显式排除；有跨 TU 静态符号合批风险，仅供本地加速实验 |
| `PALEO_ENABLE_LTO=ON` | 链接期优化档（`-flto=8 -ffat-lto-objects`，默认 OFF；配置期探测工具链支持）。2026-10-01 交错三轮实测：LAS 冷解析 -40.7%、SEG-Y 索引命中 -45.2%、catalog 灌库/查询 -18~25%，无一致回归；代价=全量链接 ~2.3×。证据与 -O3 否决记录见 `docs/perf/BUILD_OPT.md` |
| clang-tidy 门禁 | `python3 tools/check_tidy.py`——只扫相对 merge-base 改动的 src/ TU；配置 `tools/.clang-tidy`；CI 钉 `clang-tidy-20` |

PCH 评估结论（T6，定性）：模块静态库已把 Qt/QGIS 头的重压摊到 10 个
产品 target，重头是 ui/workflow 两个。PCH 预热
（`target_precompile_headers` 塞 `<QtGui>`/`qgsapplication.h`）的理论收益
集中在这两个 target 的全量编译；但代价是任何 PCH 头变更强制全 target
重编（正好打在本仓最重的 ui 上），与 AUTOMOC/unity 组合易碎，且会改变
编译命令形状让 ccache 全量 miss 一次。当前全量构建在 16 核已是分钟级、
增量由 ccache 兜底——**不落地 PCH**，ccache 是本仓收益/风险比更高的路；
编译时间成为痛点时再按当时数据重评。分析全文见 docs/progress/devex.md。

## 新模块脚手架（devex）

```bash
scripts/new_module.sh <name> <层> [target-lib]   # 生成层标记模板+测试骨架，
                                                 # 同步 tools/layering_vocab.json
```

词表外置后，新顶层模块不登记 `tools/layering_vocab.json` 会被
layer-marker 全量判违规（by design，防漏网）；脚本同时打印
`cmake/extra-<wave>.cmake` 接线片段。

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

- superbuild 首轮全量 ≈ 56min（含一次 libspatialindex 门禁失败重试）；主构建
  切 `vendor/superbuild/prefix` 重编 789 targets ≈ 11min；ctest 129/129 ≈ 37s；
  `paleo_selfcheck` 8/8，357ms（底账 docs/progress/vendor-superbuild.md）
- 首次 configure+build（3 个目标）：<1min（依赖已装）
- `tst_boot`：init+providers+srs.db+渲染 非均匀像素断言 — PASS（102ms）
- `tst_polygonize`：provider 注册+GDALPolygonize C++ 路径 — PASS
- `spikes/onnx/ort_check`：进程内 toy 推理 2.0→42.0 — PASS
- 全套 ctest（wave4 起 62 测试，offscreen）：~48s（TTHW 预算内）；设
  `PALEO_REAL_PROJECT_AREA` 跑真数据档时全套 ~110–130s（wave3 实测
  109.3s/133.1s）
