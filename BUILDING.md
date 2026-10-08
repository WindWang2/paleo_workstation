# BUILDING

## 快速开始与 TTHW（plan §44.1）

```bash
./paleo-dev bootstrap   # preflight + 依赖 vendor（binary 路）
./paleo-dev build       # 配置 + 构建
./paleo-dev test        # QT_QPA_PLATFORM=offscreen 全套测试
```

- **TTHW 目标：vendor 引导完成后，首次绿色测试 2–5 分钟**（configure+build+
  ctest，8 核基线机）。早期 wave-4 的 ~48s 是当时 62 项测试的历史数据，
  不代表当前全量；测试数以 `ctest -N` 为准。2026-10-03 的 243 项基线含既有红项，
  不能声称 TTHW 全绿目标已达成，当前对照见 `docs/progress/job-framework.md` 文末。
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

> **#76 状态（方向 71，2026-10-07）**：`.github/workflows/ci.yml` 的 Linux
> 侧（lint/linux/linux-perf）已从 qgis.org apt 系统 QGIS 迁到 deb 闭包
> vendored 路（bootstrap 拉 `vendor/prefix`，apt 只装政策例外面 Qt6/
> qtkeychain/工具链）；`tools/ci_apt_qgis.sh` 已删除。**状态如实：已迁移、
> 待 CI 实跑验证**（yaml 编写与结构核对在本机完成，GitHub Actions 语义
> 未在本方向内实跑）。`fetch-deps.sh --check-urls` 可做锁文件可达性冒烟
> （只 HEAD 探测，不下载）。
>
> **QGIS 版本口径（方向 71 统一）**：三路同一上游版本 **4.2.3**——
> superbuild 源码 pin（URL+SHA256）、deb 闭包（4.2.3+44resolute）、
> OSGeo4W 家族（4.2.x，installer 只有包名粒度，无法精确 pin——粒度边界
> 见 `vendor/manifest.json` notes；家族闸正则从 manifest 派生，单一来源）。
> 一致性由 `tools/check_qgis_versions.py` 门禁看守（ctest 项
> `qgis_versions`/`qgis_versions_selftest` + CI lint Source gates）：三处
> 口径不一致即红，防再分裂。升级 QGIS 时三处一起动，护栏会拦漏改。

例外（不 vendored，沿用系统/官方二进制）：Qt6（体积与构建时长，
superbuild 明示禁止 qt-everywhere 整块编译；走发行版或 OSGeo4W 同源）、
编译器工具链与构建依赖（flex/bison/nasm/python3）、glibc/libstdc++
（ABI floor，无法 vendored）、ONNX Runtime（官方 release SHA256 pin，
与 QGIS 路线正交）、LibreOffice（官方自含 tarball SHA256 pin——
`vendor/manifest.json` `deps.libreoffice`，`vendor/fetch-libreoffice.sh`
解到 `vendor/libreoffice/`；只取 headless `--convert-to pdf` 子集
core/ure/writer/impress/draw/calc/images/en-us/ooofonts/graphicfilter，
供 document 资产预览用，探测序 `PALEO_SOFFICE` > vendored > PATH；
Windows 侧 MSI 解包递延，走 PATH soffice）。

### glibc 三档口径（显式分层，非混乱）

| 档 | 下限 | 出处 | 语义 |
|---|---|---|---|
| binary vendoring 总地板 | 2.41 | `vendor/bootstrap.sh` preflight | 走 binary 路（deb 闭包/ORT）的宿主最低要求 |
| deb 闭包锁 | 2.43 | `vendor/bootstrap.sh`（闭包腿）/ `fetch-deps.sh` 头注 | 已提交锁是 Ubuntu 26.04（resolute）闭包，链接 GLIBC_2.43 符号；低 glibc 宿主能解包不能运行，提前拒绝 |
| ONNX Runtime abi_floor | 2.28 | `vendor/manifest.json` `abi_floor` | 官方 manylinux_2_28 构建；与 QGIS 路线正交 |
| LibreOffice | ≪2.41（官方自含基线构建） | `vendor/fetch-libreoffice.sh` | 仅作外部转换进程调用，不进链接面 |

更低 glibc 宿主走 superbuild（"superbuild-on-oldest-target"）。

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

## 独立 worktree 开发

从明确的基线建分支；`master` 与 `origin/master` 不一致时先确认要用哪一个。
主 checkout 的未提交改动不会复制到 worktree。通常使用 `codex/<主题>`，
任务指定的 `refactor/`、`goal/` 等分支名优先。每个 worktree 使用自己的 `build/`。

gitignored 依赖不会随 worktree 出现。本机共享已准备的依赖时，以主仓绝对路径
建立 symlink（以下主仓路径按实际位置替换；不要在共享 prefix 中清理或重建）：

```bash
git worktree add ../paleo_workstation-refactor -b refactor/dedup-docs-tests master
cd ../paleo_workstation-refactor
ln -s /home/kevin/projects/paleo_workstation/vendor/superbuild/prefix vendor/superbuild/prefix
ln -s /home/kevin/projects/paleo_workstation/vendor/onnxruntime vendor/onnxruntime
if [ -d /home/kevin/projects/paleo_workstation/vendor/prefix ]; then
  ln -s /home/kevin/projects/paleo_workstation/vendor/prefix vendor/prefix
fi
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DQGIS_PREFIX=/home/kevin/projects/paleo_workstation/vendor/superbuild/prefix
cmake --build build -j8
ctest --test-dir build -j8 --output-on-failure
```

新 worktree 首次 configure 若漏 glm/saribbon/sbm/segyio 的 include，再跑同一条
configure（已知接线问题，见 TODOS.md）。QScintilla 缺头 warning 在不包含
QGIS 代码编辑器头的 Linux 构建中允许降级；Windows 使用已有 `CMAKE_PREFIX_PATH`
前缀或显式 `-DQSCINTILLA_INCLUDE_DIR=<安装目录>/include` 提供头，详见任务框架历史账本。

## Windows 本机开发（localdeps 统一链，方向 72）

**背景（历史注记，勿再踩）**：本机曾以 `C:/deps/Qt/6.8.0/msvc2022_64` 编译、
而 QGIS 前缀与依赖闭包按 Qt 6.11 构建——两版 Qt 同进程混载，全量回归长期
185 红/293（含 5 个伞式测试 0xc0000139 ENTRYPOINT_NOT_FOUND），只能靠
「改前/改后红集合 diff」承载验收。「PATH 前置 `paleo-qgis-deps/Library/bin`
让进程能启动」的旧技巧只解决启动、不解决混链，已废弃。

**统一口径（2026-10-07 起）**：编译与运行同链 Qt 6.11.2，三方来源如下——

| 角色 | 位置 | 内容 |
|------|------|------|
| Qt 全家 + GDAL/GEOS/sqlite/keychain 闭包 | `~/paleo-qgis-deps`（conda 环境，qt6-main 6.11.2） | 头 + cmake 配置 + 运行 DLL（`Library/bin`，PATH 前置） |
| QGIS 4.2.0 前缀 | `~/paleo-qgis-prefix` | qgis_core/gui/analysis，按 Qt 6.11.2 构建 |
| qtpdf 覆盖层 | `C:/deps/Qt/6.11.2/msvc2022_64` | 官方 Qt 6.11.2 qtpdf 扩展（conda 闭包不含 Qt6Pdf；Qt6Config 只在自身前缀内找组件，故走 `QT_ADDITIONAL_PACKAGES_PREFIX_PATH` 补位） |

日常入口 `./paleo-dev.ps1`（build/test/selfcheck 自动进 localdeps 路线；
`vendor/osgeo4w` 存在时 vendored 路线优先，CI 口径不变）。等价手工接线：

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo `
  "-DCMAKE_PREFIX_PATH=$HOME/paleo-qgis-deps/Library" `
  "-DQT_ADDITIONAL_PACKAGES_PREFIX_PATH=C:/deps/Qt/6.11.2/msvc2022_64" `
  "-DQGIS_PREFIX=$HOME/paleo-qgis-prefix" `
  "-DQSCINTILLA_PREFIX=$HOME/paleo-qgis-deps/Library/include/qt6"
# 构建/测试时 PATH 前置（qt6 工具 lrelease/moc 也依赖它解析 DLL）：
#   $HOME/paleo-qgis-deps;$HOME/paleo-qgis-deps/Library/bin;$HOME/paleo-qgis-prefix/bin;C:/deps/Qt/6.11.2/msvc2022_64/bin
# 并设 GDAL_DATA=$HOME/paleo-qgis-deps/Library/share/gdal、
# PROJ_LIB=$HOME/paleo-qgis-deps/Library/share/proj（./paleo-dev.ps1 已内置）。
# 测试还需（ps1 test 已内置）：TEMP/TMP 与 SEISMIC_INDEX_CACHE_DIR 指到
# build/paleo-tmp 内子目录（沙箱监狱对策，见下）、PALEO_PYTHON=
# $HOME/paleo-qgis-deps/python.exe（conda 布局 python 在根不在 Library/bin）。
```

要点：

- cmake ≥3.28（工程要求；VS 自带 3.27 不够——本机用 `C:/Qt/Tools/CMake_64`
  的 3.29）。
- conda 布局里 Qt6 工具在 `Library/lib/qt6[/bin]`，其 DLL 依赖由
  `Library/bin` 供给——configure/build/test 全程都要 PATH 前置 deps bin，
  否则 lrelease 段错误式失败、MOC 偶发崩。
- `Library/bin` 同时住着 qt5 工具（本 env 含 qt5 包）——lrelease 5.15.15
  与 6.11.2 并存，勿用 PATH 裸调 `lrelease`，CMake 钉的是 qt6 绝对路径。
- 退役项：`C:/deps/Qt/6.8.0/msvc2022_64` 与 `C:/deps/qscintilla-install`
  不再进任何 CMAKE_PREFIX_PATH/PATH（QScintilla 头随 conda Qt 走
  `Library/include/qt6/Qsci`，2.14.1 同版）。
- 根位置可用 `PALEO_LOCAL_DEPS` / `PALEO_QGIS_PREFIX` / `PALEO_QTPDF_PREFIX`
  覆盖（paleo-dev.ps1 localdeps 路线）。

qtpdf 覆盖层一次性安装（已在 `C:/deps/Qt/6.11.2/msvc2022_64` 就位的可跳过）：
官方 extensions 仓库取 6.11.2 msvc2022_64 的 qtpdf 7z（ Updates.xml 里
`extensions.qtpdf.6112.win64_msvc2022_64`，版本 `6.11.2-0-202608131017`），
按同目录 `.7z.sha1` 校验后解压覆盖到该前缀。aqtinstall≤3.3 不识别 6.10+
的新仓库布局，`-m qtpdf` 会 404，需手工取档或升级 aqt。

**防回归自检**：`./paleo-dev.ps1 checkenv`（`tools/check_qt_env.ps1`）比对
CMakeCache 的编译侧 Qt 与 DLL 搜索序解析到的运行侧 Qt：major.minor 不一致
即报警退出非零（混链特征）；patch 不一致给警告。构建机换 Qt 后先跑它。

**本机沙箱文件监狱（历代「QTemporaryDir 红脸」真身，2026-10-07 定案）**：
本机安全策略对**镜像文件位于仓库树内的进程**实施写限制——只能写树内路径；
`%TEMP%` 在树外 → `QTemporaryDir`/`QFile` 构造即「拒绝访问」（isValid()=FALSE
无 syscall，cmd.exe 拷入树内同样写不了 %TEMP%，拷出树外全绿——OS 级实证，
与 Qt/QGIS/混链无关；历代 185 红的主体即此）。junction 伪装出树无效（策略
解析真实路径）。**对策**：localdeps 路线下 `paleo-dev.ps1 test/selfcheck`
自动把 `TEMP/TMP` 重定向到 `build/paleo-tmp`（树内、浅层，远离
CMakeLists 注记的 MAX_PATH 深路径顾虑）。根治需在沙箱策略侧放行本仓库
的 `%TEMP%` 写——机器配置问题，移交用户决策。

## 平台 × 版本矩阵

| 平台 | 状态 | 依赖来源 |
|------|------|----------|
| Linux x86_64：deb 闭包需 glibc≥2.43（Ubuntu 26.04 / 同代 Arch）；Debian 13（glibc 2.41）不能运行该闭包，须走 superbuild | Arch 本机通过；Ubuntu 26.04 CI（deb 闭包 vendored，#76 已迁移待 CI 验证） | superbuild 自编译 prefix（首选）→ deb 闭包 `vendor/prefix/usr`（加速档，CI 现行）；发行版 QGIS 4.2.x 仅兜底 |
| Windows x86_64 | CI leg + 本机 localdeps（Qt 6.11.2 统一链，方向 72；全量回归本机可信） | CI：OSGeo4W `qgis` + `qgis-devel` 4.2.x + `qt6-devel`，MSVC /MD；本机：`~/paleo-qgis-deps`（conda qt6-main 6.11.2）+ `~/paleo-qgis-prefix` + qtpdf 官方覆盖层（见上节） |
| 更低 glibc 宿主 | 不支持 | superbuild-on-oldest-target（ExternalProject） |

## 依赖（vendor manifest pin）

QGIS 4.2.x（三路口径统一上游 4.2.3，`tools/check_qgis_versions.py` 门禁看守）· Qt ≥6.6 · GDAL · PROJ · GEOS · QCA-qt6 · QtKeychain-qt6 · libspatialindex · exiv2 · libzip · OpenSSL · sqlite3/spatialite · **ONNX Runtime 1.30.0**（官方 release，sha256 `a5ed5a3c…3b3fd`，manylinux_2_28）。Ubuntu 26.04 的 deb 完整闭包和 SHA-256 在 `vendor/deb-closure.lock`（库解包在 `vendor/prefix/usr/lib/<multiarch>`，`./paleo-dev` 已加入 `LD_LIBRARY_PATH`）；Ubuntu pool 会删除被安全更新取代的旧版本，因此 `fetch-deps.sh` 在 pool 404 时回退到 `https://snapshot.ubuntu.com/ubuntu/<ts>/`，时间戳记录在 `vendor/deb-closure.snapshot`（`--update-lock` 会刷新；可用 `PALEO_DEB_SNAPSHOT` 覆盖），内容仍由锁内 SHA-256 校验；OSGeo4W 安装器摘要在 `vendor/manifest.json`。

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
  ——Linux 可用 `ctest -j8`；构建和测试并行度一律不超过 8，不用 `$(nproc)`。
  **已知边界：Windows NativeFormat 走注册表
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

Debian 闭包升级时，在目标发行版且已配置 QGIS 官方 apt 源的干净环境运行 `./vendor/fetch-deps.sh --update-lock --print-only`，审查并提交新的 `vendor/deb-closure.lock`。正常 bootstrap 只读取锁文件，并仅解包锁内经过大小和 SHA-256 校验的文件。superbuild 升级则同步改 `vendor/superbuild/CMakeLists.txt` 的 `QGIS_URL` 与 `URL_HASH`（补丁文件名随版本重命名，需对目标版本 tarball 重验证 dry-run 干净），`vendor/cache/tarballs/` 缓存与哈希一并更新。**三路版本口径（superbuild 精确 / deb 闭包上游 / manifest `qgis_family` 家族）必须一起动**——`tools/check_qgis_versions.py`（ctest `qgis_versions` + CI lint）不一致即红。ONNX Runtime 升级则更新 `vendor/manifest.json` 的版本和摘要。Windows bootstrap 校验安装器 SHA-256，并按 manifest `qgis_family` 拒绝家族不符（正则从 manifest 派生，单一来源；OSGeo4W installer 只有包名粒度，精确版本 pin 不可表达）；OSGeo4W 包闭包仍由其安装器选择，升级时须复核 CI。

## 实测值（Phase 0，本机 Arch/qgis-4.2.2 已装）

- superbuild 首轮全量 ≈ 56min（含一次 libspatialindex 门禁失败重试）；主构建
  切 `vendor/superbuild/prefix` 重编 789 targets ≈ 11min；ctest 129/129 ≈ 37s；
  `paleo_selfcheck` 8/8，357ms（底账 docs/progress/vendor-superbuild.md）
- 首次 configure+build（3 个目标）：<1min（依赖已装）——Phase 0 规模口径
- `tst_boot`：init+providers+srs.db+渲染 非均匀像素断言 — PASS（102ms）
- `tst_polygonize`：provider 注册+GDALPolygonize C++ 路径 — PASS
- `spikes/onnx/ort_check`：进程内 toy 推理 2.0→42.0 — PASS
- 全套 ctest（wave4 起 62 测试，offscreen）：~48s（TTHW 预算内）；设
  `PALEO_REAL_PROJECT_AREA` 跑真数据档时全套 ~110–130s（wave3 实测
  109.3s/133.1s）

### 增量构建基线（ET14 刷新，方向70 2026-10-07）

Phase 0 的「3 目标 <1min」早已失真；当前规模（475 TU / 5743 ninja 目标），
`tools/measure_incremental.sh` 钉口径：touch 组装根 `src/app/appcontext.cpp`
（叶子层，扇出上限样本）→ 1 TU 重编 + 18 个下游 exe relink = 20 步，本机
MSVC/Ninja -j8 实测 ~15s（链接占绝对大头，编译 <2s）。绝对时长跨机不可比
（仅记录，不是门）；回归口径用比率：tu_ratio=1/475、step_ratio=20/5743，
JSON 落 `build/incremental-baseline.jsonl` 可追趋势。Linux 无 MSVC 增量
链接开销，预计显著低于此；PLAN ET14「单文件改动增量 ≤60s」在当前规模
下仍成立。

**CI 侧落点（方向75 2026-10-08）**：linux 主 job 在 Selfcheck 后追加
`Incremental build baseline (ET14)` 步（step 级 `continue-on-error`——
观察面不拖累合并门禁），每 run 跑一轮 `tools/measure_incremental.sh`，
产物 `build/incremental-baseline.jsonl` 以 artifact
`incremental-baseline-linux` 上传（retention 90 天）。看趋势：run 页面
下载 artifact，逐行对比 tu_ratio/step_ratio；绝对时长只记录不设门
（runner 世代不同不可比）。
