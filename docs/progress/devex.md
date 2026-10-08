# wave/devex-infra 交付记录

构建/测试基础设施治理：测试链接瘦身、ctest 并行竞态根治、层护栏闸门、
clang-tidy 门禁、ET1 消费侧收口、构建加速、sanitizer 档、CI 三段化、
新模块脚手架。TODOS.md 递延项不在此重复，文末附本方向新增递延。

## T1 测试链接瘦身

契约：`add_paleo_test(name [LIBS ...])`——单参回退 `paleo_core` 伞
（`cmake/extra-*.cmake` 四个并行方向的旧式调用零改动可用，本分支保留
tst_ui/tst_panels/tst_composeworkflow/tst_rehydrate/tst_threeway 五个
旧式调用作兼容样本，端到端构建通过）；`LIBS` 给该测试 include 面的
最小闭包（分析脚本：逐测试扫 `#include` 归属模块，按 DAG 闭包取最小
覆盖集；DAG 见 CMakeLists.txt 模块化注释）。

**relink 面对比**（口径：`scripts/relink-surface.py`——touch 单模块源文件
后真实 ninja 构建，统计 `Linking CXX executable tst_*` 行数。用真实构建
而非 `ninja -n`：本机 ninja(kitware fork 1.13) 在 regen console 边 + dry-run
组合下会把真实工作漏报为空，实测假绿；链接行计数不受影响）：

| touch 的模块源 | 改前（全伞） | 改后（最小集） | 降幅 |
|---|---|---|---|
| src/domain/types.cpp | 78 | 72 | −8% |
| src/catalog/datacatalog.cpp | 78 | 74 | −5% |
| src/io/dataimportservice.cpp | 78 | 60 | −23% |
| src/services/previewdoc.cpp | 78 | 52 | −33% |
| src/workflow/workflows.cpp | 78 | 45 | −42% |
| src/ui/paleomainwindow.cpp | 78 | 37 | −53% |

此表为 wave/devex-infra 当轮测量快照（测试数与路径均非当前索引）。
`workflows.cpp` 已拆到五个 workflow 实现文件，现行链接集依然按产品模块 DAG
取最小闭包，拆 TU 不会自动缩小同一模块静态库的 relink 面。

链上样本（link line 上 paleo 静态档案数）：tst_ui=13（伞式样本）、
tst_domain=3、tst_crashreport=8、tst_linkage=9、tst_catalog=1。

如实说明：**上游模块（domain/store）收益有限**——几乎所有测试经 DAG
闭包都消费它们（domain 被 algorithms/qgis/io/services 全链携带）。收益
集中在下游模块（ui/workflow/services/io，23–53%）。进一步压上游扇出
要动 DAG 分层本身，超出本方向范围。

附带收益：每测试打 `LABELS`（按 LIBS 首模块），`ctest -L io` /
`-LE core` 可分步筛选；`ctest -j4` 全套 27.8s（改前串行 ~48s）。

2026-10-03 追加对账：`tst_jobrunner` 不引用 QgisLayer/ProjectService，仅需
JobRunner 服务与 QgsApplication 的 QtWidgets 依赖；LIBS 改为
`paleo_services paleo_ui_deps`，移除 `paleo_qgis` 静态库 relink 边。
`tst_propmodelperf` 的 `paleo_io` 已在 workflow 传递闭包，去掉显式重复项，
闭包和 relink 面本身不变。两项保留 LIBS 首模块，因此标签口径保持。
其余点名套件消费真实 QGIS/IO/Workflow/UI 栈，未为了字面短而更换链接归属。

## T2 ctest 并行竞态根治

- 真凶（QSettings）：tst_panels/tst_webviewpanel/tst_composeworkflow 等以
  NativeFormat 写真实 `~/.config/paleo/paleo.conf`，并行互踩（tst_ui/
  tst_procdialog 曾在 .cpp 里 setPath 自救）。修复零 .cpp：`add_paleo_test`
  统一经 `paleo_test_sandbox` 注入每测试独立
  `XDG_CONFIG_HOME/XDG_DATA_HOME/XDG_CACHE_HOME/HOME`
  （`build/ctest-home/<test>/`）。
- 已知坑修复：tst_onnx* 的 `set_tests_properties(... ENVIRONMENT ...)` 是
  整体覆盖非追加，会抹掉沙箱 env——改为
  `set_property(TEST ... APPEND PROPERTY ENVIRONMENT ...)`，并在沙箱函数
  注释里立此存照。
- **已知边界：Windows NativeFormat 走注册表不受 env 控制**——Windows leg
  保持串行 ctest（ci.yml 注明）。
- `ctest -j4` ×5 连续 83/83 全绿（本机 16 核）。
- 捕获并修复一处**非竞态**偶发：tst_correlation_full 内嵌墙钟性能断言
  （10 次 warm reorder < 1000ms，tests .cpp 归 correlation 方向），并行
  负载下超时（本轮实测 run4 失败、单跑复现失败、轻载复绿）。布线侧
  `RUN_SERIAL TRUE` 关回串行基线；根治（QTRY_ 化 / 预算放宽）递延给
  测试属主方向。

## T3 层护栏闸门（checker v2）

- 真实 argv 校验：未知参数 exit 2（原 argv 全静默忽略，`--strict` 假绿）。
- `--strict` 语义：baseline 非空即红（防回升——当前已归零）+ 可收缩条目
  红（强制删行）。非严格模式维持提示语义。
- 词表外置 `tools/layering_vocab.json`：新顶层模块不登记即 layer-marker
  全量判违规（by design）；`scripts/new_module.sh` 同步维护。selftest 增
  至 25 夹具（文件/词表/strict 语义），ctest 项 `layering_strict` 入闸。
- AGENTS.md 模块化描述更新（原「各层编进同一 paleo_core 静态库」已过期）。

## T4 clang-tidy 门禁

- `tools/check_tidy.py`：只扫相对 merge-base **改动过**的 src/ TU
  （tests/ 不扫，存量面大；配置 `tools/.clang-tidy`，选题宁缺勿滥）。
- 实操坑两条：① CMake≥3.28+Ninja 默认 C++20 模块扫描往每条编译命令塞
  `-fdeps-format/-fmodule-mapper`（GCC 专属）——`CMAKE_CXX_SCAN_FOR_MODULES
  OFF` 根治（本仓无 named modules，顺带加速编译）；② Arch gcc 默认注入
  `-mno-direct-extern-access`（clang 同语义不同名）——check_tidy 生成
  `build/tidy-db/compile_commands.json` 剥除（文件名/目录名是 clang-tidy
  `-p` 的查找契约，后缀形式对它不可见，实测踩过）。
- CI lint job 钉 `clang-tidy-20`（升级=有意变更）。本机 LLVM 22 实跑：
  空改动绿、注入改动真实执行并出报告。

## T5 ET1 消费侧

- 测试 main() 统一 `qEnvironmentVariable("QGIS_PREFIX_PATH", "/usr")`
  （tst_boot 既有先例），批量对齐 23 个测试 main（单行替换，PR 单列说明）。
- `paleo-dev test`：`vendor/prefix/usr` 存在（deb 闭包路被显式
  bootstrap 过）才注入 `QGIS_PREFIX_PATH` + `LD_LIBRARY_PATH`；系统包路
  零影响。
- `fetch-deps.sh --print-only` 放行无 apt 宿主（冒烟门只读锁）+ 增加锁
  条目格式校验（与下载路径同一断言）+ 修 MiB 单位换算。CI lint job
  挂冒烟步（863 包全校验，零下载）。

## T6 构建加速

- ccache/sccache launcher 自动检测（`-DCMAKE_CXX_COMPILER_LAUNCHER` 显式
  指定优先，`PALEO_NO_CCACHE=1` 关）；CI 缓存 `~/.cache/ccache`。
- PCH 评估：**不落地**。模块静态库已把重头摊到 ui/workflow 两个 target；
  PCH 理论收益集中于此，但 PCH 头变更强制全 target 重编（正打最重的
  ui）、与 AUTOMOC/unity 组合易碎、且改变命令形状令 ccache 一次性全
  miss。当前全量 16 核分钟级、增量有 ccache 兜底，痛点重现时再按当时
  数据重评。
- Unity：`PALEO_UNITY_BUILD=ON` 实验开关（默认 OFF；逐产品 target，
  sbm/segyio/saribbon 显式排除）。见下方 Unity 实测小节。

## T7 sanitizer 档

`PALEO_ENABLE_ASAN` / `PALEO_ENABLE_UBSAN`（默认 OFF，GNU/Clang 限定）；
测试注入 `ASAN_OPTIONS=detect_leaks=0`（QGIS/Qt 设计内"泄漏"面，
`tools/lsan-suppressions.txt` 供猎泄漏模式）+ `UBSAN_OPTIONS=
print_stacktrace=1`（可恢复巡检）。实测发现见下方 ASAN/UBSAN 轮小节。

## T8 CI 三段化

lint（clang-tidy 增量门禁 + deb 闭包冒烟）/ linux（编译+测试）/ windows
（原样）。不拆 per-module 测试 job：GB 级 QGIS 依赖在 runner 间流转不
划算，用 `LABELS` + `ctest -LE core` → `-L core` 同机分步（快速隔离模块
测试先跑，伞式重测试殿后）。concurrency 取消超期 run。actionlint 1.7.7
验证通过（`ubuntu-26.04` 标签经 `.github/actionlint.yaml` 显式放行——
矩阵钉定平台，不降级标签）。

## T9 新模块脚手架

`scripts/new_module.sh <name> <层> [target-lib]`：层标记模板 + .h/.cpp +
测试骨架 + 词表 JSON 同步 + 接线片段打印。端到端实测（scratchmod 经
`cmake/extra-devex.cmake` 挂载 → 旧式单参注册 → 配置/构建/ctest 全通），
验证后已清理。

## ASAN/UBSAN 轮实测

`build-asan`（RelWithDebInfo + `PALEO_ENABLE_ASAN=ON` +
`PALEO_ENABLE_UBSAN=ON`，534 目标全编过），`ctest -j4` 一轮 80/83：

- **ASAN 零内存错误**——全套测试无 heap-buffer-overflow / use-after-free /
  SEGV / invalid-free（`ASAN_OPTIONS=detect_leaks=0` 档，LSAN 面按设计
  关闭）。产品侧本轮无内存安全发现。
- **UBSAN**：仅 3 处 Qt 内部告警（`QtCore/qpointer.h:76 downcast …
  QAbstractButton`，触发于 tst_panels 的 widget 销毁窗口期）——Qt 对
  析构中 QObject 的 QPointer downcast 是已知框架行为，非 Paleo 缺陷，
  归入 Qt 已知面。
- 3 个失败全部是**资源预算类断言在 sanitizer 开销下的预期红**，非产品
  缺陷：tst_correlation_full（墙钟 1000ms，RUN_SERIAL 也压不过 ASAN
  ~2× 减速）、tst_perfbudget（性能预算档）、tst_segy_lines
  （RSS 增长预算：9MB 载荷 3MB 预算，ASAN 红区/隔离区放大 ~5MB，
  tests/tst_segy_lines.cpp:210）。budget 断言在 sanitizer 档豁免需要
  测试侧感知编译开关（`PALEO_SANITIZER_BUILD` 定义），改动在 tests
  属主方向，递延 6。

**方向70 收口（2026-10-07）**：递延 6 已落地——CMake 在 ASAN/UBSAN 任一
开启时全局注入 `PALEO_SANITIZER_BUILD=1`；墙钟预算经
`tests/perfbudget_relax.h` 的 `relaxedBudgetMs()` ×3 放宽（tst_perfbudget
100/500/100ms、tst_correlation_full 1000/3000/9000/12000ms），RSS 增长
断言在 sanitizer 档跳过（3MB×3=9MB 超过 8.2MB 样本载荷会变空洞，索引
行为断言保留）；on/off 两档系数由 tst_sanitizer_budget(+_on 强制注入
定义的第二二进制) 钉住，本机（含 MSVC）可验证。CI 侧 ci.yml 新增非阻断
`linux-asan` job（观察档起步，复用 linux 模板 + ASAN+UBSAN + 串行 ctest +
LSAN off 口径）。

**方向75 断链修复（2026-10-08）**：上述 CI 化实际从未生效——asan job 的
安装步 run 的是方向 71 已删除的 apt 脚本（`tools/ci_apt_qgis.sh`），
安装步即红并被 job 级 `continue-on-error` 吞掉，run 页面只显示一个静默
的红 asan 块，**观察档自合入起从未跑进编译步**（gh api 逐 run 核对
10/10 红在同一安装步，证据链见 `.goal-loop-ledger-ci-asan-fix.md`）。
修复：依赖步逐行对拍 linux 主 job vendored 路（checkout/actions 钉 SHA、
Qt6 发行版包 apt、vendor 缓存补 debs/lo 路径、`./paleo-dev bootstrap`）；
Configure 步补 `QGIS_PREFIX_PATH`（裸 cmake 不过 paleo-dev 自动注入）；
Test 步从裸 ctest 改 `./paleo-dev test -j 1`（vendored 路测试期需要
deb 库路径/GDAL 驱动注入，与 linux 主 job 同源；`ASAN_OPTIONS=
detect_leaks=0` 语义不变）。防回升：`tools/check_ci_scripts.py` 进 lint
Source gates——静态解析 workflows `run:` 块扫 `tools/` 引用做存在性
检查（yml 注释/shell 注释不误伤，selftest 8 例双向 + mutation 实证打红）。
修后首跑实证（PR #300，run 37727797358，2026-10-08）：asan job 安装步
（原致命断链步）completed/success，进入 Vendor dependencies（bootstrap
全量构建）——断链修复生效；lint job 同 run 实跑新护栏全绿。ASAN/UBSAN
全量输出对照 80/83 基线**暂被下方移交项阻塞**（deb 闭包缺库致 bootstrap
链接期红，与 linux 主 job 同一处），闭包修复后首跑补对照。

同轮运行侧核对的**移交项**（不属本方向修复，vendor 域）：
- linux/linux-perf 自方向 71 合入（`27ac69d8`，10-07 13:15，最后绿
  `61c8f287`）持续红在 bootstrap 尾部构建链接期——deb 闭包缺
  LAPACK/BLAS（libarmadillo/libarpack 未解析符号）与 libpulsecommon，
  需扩闭包种子重新生成锁；
- windows 红在 OSGeo4W 钉版闸——上游全家族发版（arrow-cpp 25.0.0→
  25.0.1、curl、gdal 等），manifest 钉版需刷新（更早 runs 红在 Test
  步，是另一独立遗留面）。

## Unity 实测

~~`PALEO_UNITY_BUILD=ON`（独立 build-unity 目录）**编译不过**~~
**方向70 清障后（2026-10-07）10 目标 BATCH_SIZE=8 全编过**：早期同名匿名
helper 已按两口径收拢——同构实现进共享 internal 头（store/qgis/algo/io/
workflow 的 `*errors_internal.h` + inversion/cluster/faciesmapping/
singlefactor/gdalreg/parserissues/sfpkg/seismic3d/correlation/uienv 共
×15 个），语义不同的逐文件唯一改名（connectionNameFor×8 / ensureOpen×7 /
failure×3 / GridSpec·readGrid / safeSegment / kNoData / kRowId /
WalkResult / compressionFlags / pointOnSegment / polylineLength / kInf /
kFileNodata / resolveProject / Cur 等）；另修两处 Windows 宏撞名
（Qt `#define slots` 抹掉 curveexpr 参数名、rpcndr `small` 撞 ribbonpanels
lambda 名）。**已知余量（mega-TU 超集分析暴露、当前分批未触发，源序
重排可复暴露，后续跟进）**：cluster `valid`/`distance`、`PALEO_NODATA`
宏双定义、datapreview `qssHeight` 重载歧义、
previewhistogramwidget 的 QFont helper 同名。开关默认仍 OFF——全量 ctest
长跑对照未做，TU 合并的链接期收益未量化，启用决策留待有数据时再定；
启用前置（同名符号收敛）已大幅完成，新代码请沿用「helper 进
`*_internal.h` 或取文件唯一名」的约定防回升。

## 本方向新增递延（不入 TODOS，属主方向跟进）

1. tst_correlation_full 墙钟断言 QTRY_ 化/预算放宽（correlation 方向）。
2. tests/ 存量 clang-tidy 清算（测试属主方向逐包认领；门禁暂只覆盖 src/）。
3. Windows leg 的 QSettings 注册表沙箱（需 qt.conf 或 NativeFormat→IniFormat
   全局切换，跨方向决策）。
4. 上游模块（domain/store）relink 扇出压减——需 DAG 分层评审。
5. ~~匿名 helper 去重~~ **方向70 已收口（2026-10-07）**：setError×35/
   connectionNameFor×8 及后续 30+ 处同名符号按「同构→共享 internal 头 ×15 /
   语义不同→逐文件唯一名」收拢，unity 10 目标全编过（见上节；余量清单
   同上）。新增同名 helper 请进 `*_internal.h` 或取文件唯一名。
6. ~~资源预算类断言感知 sanitizer 档~~ **方向70 已收口（2026-10-07）**：
   `PALEO_SANITIZER_BUILD` 全局注入 + 墙钟 ×3/RSS 跳过 + 双二进制单测
   （见上节 ASAN/UBSAN 段收口注）。

## 方向 78：mkproject 夹具工厂化（测试面新增）

`paleo_mkproject` 清单外置（`--manifest`）+ 微型合成数据集后，「真跑生产
导入路径」的夹具面进树（synthetic `PerfFixtures` 只灌 catalog，绕过 io
解析/QGIS 面）：

- **布线**：`tst_mkprojectfixture` 手动注册（非 `add_paleo_test`——需要
  `fixtures/mkprojectfixture.cpp` 第二源 + `MKPROJECT_BIN` 生成器表达式）；
  LABEL `services`（core 段，不进 perf）；`RUN_SERIAL TRUE`（QProcess 子
  进程 + 子进程内 QgsApplication init 的资源面，对齐 tst_perfbudget 先例，
  CI 并行负载防抖）。
- **沙箱口径**：QProcess 默认继承父环境——ctest 的 XDG 沙箱/
  QGIS_PREFIX_PATH 与 paleo-dev 的树内 TEMP/TMP 同监子进程（方向 72 监狱
  口径下 QTemporaryDir/QProcess 可用，无需注入；实测两连实例通过）。
- **夹具资产**：`tools/reference/mkproject/mini/`（~150 KiB，stdlib-only
  生成器可复现；SEG-Y 复用 `tools/make_segy_fixture.py` 道头约定单一真源）。
- **覆盖增量**（io 解析面从 synthetic-only → 生产路径）：wellfileparsers
  位置/头驱动双面、readWorkbook SpreadsheetML、lasparser、segyreader、
  cuttingsdoc、井附件角色词表（目录关键词）、outsource 外链——文件:行号
  对照见方向 78 ledger。
