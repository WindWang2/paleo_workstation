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

链上样本（link line 上 paleo 静态档案数）：tst_ui=13（伞式样本）、
tst_domain=3、tst_crashreport=8、tst_linkage=9、tst_catalog=1。

如实说明：**上游模块（domain/store）收益有限**——几乎所有测试经 DAG
闭包都消费它们（domain 被 algorithms/qgis/io/services 全链携带）。收益
集中在下游模块（ui/workflow/services/io，23–53%）。进一步压上游扇出
要动 DAG 分层本身，超出本方向范围。

附带收益：每测试打 `LABELS`（按 LIBS 首模块），`ctest -L io` /
`-LE core` 可分步筛选；`ctest -j4` 全套 27.8s（改前串行 ~48s）。

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

## Unity 实测

`PALEO_UNITY_BUILD=ON`（独立 build-unity 目录）**编译不过，结论如实记录**：
全仓 .cpp 惯用同名匿名 namespace helper（`setError(QString*, const
QString&)`、`connectionNameFor(const QString&)` 等），paleo_store（metadata×
catalog）与 paleo_qgis 首轮合批即 redefinition，逐批粒度调小/逐源
`SKIP_UNITY_BUILD_INCLUSION` 的维护成本高于 unity 收益。开关按任务约定
保留（默认 OFF 不影响现有路径），真正启用前置条件 = helper 去重/改名——
src/ 改动归各模块方向，见文末递延 5。

## 本方向新增递延（不入 TODOS，属主方向跟进）

1. tst_correlation_full 墙钟断言 QTRY_ 化/预算放宽（correlation 方向）。
2. tests/ 存量 clang-tidy 清算（测试属主方向逐包认领；门禁暂只覆盖 src/）。
3. Windows leg 的 QSettings 注册表沙箱（需 qt.conf 或 NativeFormat→IniFormat
   全局切换，跨方向决策）。
4. 上游模块（domain/store）relink 扇出压减——需 DAG 分层评审。
5. 匿名 helper 去重（setError/connectionNameFor 等同名符号收拢到共享
   internal 工具）——unity build 启用前置（数据/QGIS 封装方向）。
6. 资源预算类断言（墙钟/RSS）感知 sanitizer 档：注入
   `PALEO_SANITIZER_BUILD` 编译定义 + budget 测试自行降档/跳过
   （tests 属主方向；布线侧已留 paleo_test_sandbox 注入点）。
