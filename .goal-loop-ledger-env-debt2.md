# 方向 81 ledger：环境债二期（env-debt2）——16 红清单逐条处置 + QSettings 沙箱决策

基线：任务书行号按 origin/master `e3c8d31d`；开工时 master 已前进到 `f31ee461`（多出 #296/#297，
其中 #297 引入的 OSGeo4W 钉版逻辑把 master 的 Windows CI 卡死在 Vendor 步，见 R1）。
worktree `/workspace/audit/env-debt2`，分支 `goal/env-debt2-20261009`。
验证模式（业主定）：Linux 盒改码 + PR 的 GitHub Actions Windows CI 验证（单轮 ~1.5h）；本机 Windows
全量 ctest 由业主自跑两遍。**CI（OSGeo4W vendored、全新 HKCU、无文件监狱、-LE perf）与业主本机
（localdeps、长期使用的 HKCU、仓库树文件监狱、全量含 perf）环境不同**——每类给出「CI 能否复现/验证」。

## R0 红集合分类（源：方向 72 ledger R11 终态 16 红，2ab3310 基线）

| 类 | 测试 | 方向 72 取证 | 根因判定（本方向） | CI 能否复现 |
|---|---|---|---|---|
| srsdb | tst_runtime、boot | prefix 无 resources/srs.db | localdeps 前缀只有 bin/include/lib；srs.db 由 qgis-providers-common postinst 生成，任何「只解包」路线都缺 | 否（OSGeo4W 前缀自带 srs.db）；工具本身经 selftest 在 CI 跑 |
| pidprobe | tst_metastore、boot | pid 128364 不存在却判存活 | QLockFile 判 stale 先比主机名：Windows 锁文件写 COMPUTERNAME，比对侧是 DNS 名，拼写/大小写/后缀不同 → 当异机锁，永不回收 | **能**：新用例用变体主机名，旧逻辑在 Linux 上也红（已实证） |
| qsettings | tst_ui、tst_panels、tst_uxtheme、tst_dockmanager、tst_wellsection_ui、tst_stratigraphicweb、tst_wellfaciesconfig | 窗口状态往返、菜单使能、明文密钥文件断言 | 测试读写开发者真实 HKCU（及 %LOCALAPPDATA% 配置文件）——CI 每次全新 HKCU 所以绿，本机长期状态污染所以红 | 部分：沙箱 hook 生效由 tst_test_sandbox 在 CI 钉住；7 项转绿只能本机验 |
| python | tst_pythonrepl、tst_scriptrunner | QProcess startedSpy 10s 未遂 | 最可能为 #274（e8ed7069，started 同步发射测试缺陷，不在 2ab3310 内）；另 PALEO_PYTHON 从未进 ps1（BUILDING.md 与代码不符） | 断言能（CI 不设 PALEO_PYTHON 时为空操作）；转绿只能本机验 |
| perf-host | tst_cache_las、tst_singlefactor_perf、tst_startup_trace | 比率/墙钟门超限 | 宿主机性能 | 否（CI -LE perf） |
| perf-selfcheck | tst_aiassist_perf | ctest 红、直跑绿 | 计量不自洽：单发量 64²、批式每块 80²（1.56× 偏置）；亚毫秒单轮受调度抖动；夹具目录不校验 | 否（-LE perf）；Linux 本机跑通 |

## QSettings 三选一（决策记录）

判据：①本机 7 红转绿且测试从此自洽（不依赖开发者 HKCU 状态）②不碰用户数据（零迁移风险）③CI 可验证
机制本身 ④改动半径。

- **定第三类：保留产品 NativeFormat 与 Windows 串行，测试侧补 Windows 私有注册表 hive**。
  不切换用户存储格式；七项若转绿便无需豁免。`tests/support/paleo_test_registry_sandbox.cpp`
  以 OBJECT 库直链进每个测试 exe，静态初始化期 `RegLoadAppKeyW` 挂载 `ctest-home/<test>/registry.hiv`
  （树内——文件监狱允许；每次先删 → 全新），种子拷入 Shell Folders / 每用户字体 / Control Panel
  International、Desktop 保持路径/字体/区域解析不变，`RegOverridePredefKey(HKEY_CURRENT_USER)` 重映射本进程
  HKCU。第一档失败（如 Wine 的 RegLoadAppKey 是桩）退到 `HKCU\Software\paleo-test-sandbox\<exe>` 专用键（先删）；
  tst_test_sandbox 在 ctest 下断言必须是第一档。产品零改动、注册表语义原样。
- **弃 1：qt.conf 强制 IniFormat**——不可行：qt.conf 没有 QSettings 格式键（Qt 文档规定库路径与平台插件参数）。
  复核：[Qt 官方 qt.conf 文档](https://doc.qt.io/qt-6/qt-conf.html)规定 QLibraryInfo 路径与平台插件参数，未提供 QSettings 存储格式键。
- **弃 2：产品全局改 IniFormat**——28 处显式 `QSettings(org, app)` NativeFormat 构造点；现有用户注册表配置需迁移
  （丢配置风险），收益只在测试侧。
- **弃第三类的纯串行 + 七项永久豁免实现**——7 红永久挂账；且测试 `.clear()` 会抹掉业主真实的 `HKCU\Software\paleo\paleo`。
  保持档的产品决定不变，通过测试环境隔离解决该风险；最终七项是否免豁免仍以本机双遍为准。
- 附带：tst_wellfaciesconfig 的配置文件在 Windows 落 `%LOCALAPPDATA%`（known folder，不看 XDG，且树外被文件监狱拒写）
  ——新增 `PALEO_WELL_FACIES_CONFIG` 全路径覆盖（与 `_URL/_API_KEY` 同族），测试全平台钉进用例临时目录。

Wine 指示性验证（非权威，RegLoadAppKey 为桩只能测第二档）：唯一 token 写入沙箱并读回，`HKEY_USERS\<SID>`
真实 hive 里只有首次未沙箱运行遗留的旧值、无本轮 token ——隔离成立。第一档只能在真 Windows（CI）验证。

## perf 处置

- 三项 perf-host：**豁免名单**（`tools/env_redset.json`），不硬修、阈值不动。豁免只限 localdeps 路线，且 QtTest
  输出里每条 `FAIL!` 都要命中该测试自己的预算断言文案（崩溃/别的断言/无输出不放行）；selftest 校验正则与测试源码
  文案同步。弃「relax 系数」：tst_cache_las 的 0.5、singlefactor 的 6×/3.5×、startup 的基线×2.5 都是比率门，
  乘系数会改语义（任务书要求保留比率门语义）。
- tst_aiassist_perf：环境自洽优先——同尺寸比（单发按 (tile+2·halo)²）+ 两侧 3 轮取最小 + 夹具目录校验；4× 门不动。

## 迭代记录

| 轮 | 改动 | 验证结果 | 判定 | 下一步 |
|---|---|---|---|---|
| R0 | 分类（上表）；master Windows CI 勘察 | master `f31ee461` Windows leg 卡在 Vendor 步 | 见 R1 | 先修 CI |
| R1 | `aa07d7c7` fix(ci)：pin_osgeo4w.py 的 setup.ini 解析让 `[prev]/[test]` 段旧版本覆盖当前版本；paleo-dev.ps1 Read-InstalledDbMap 把 `INSTALLED.DB 2` 头行与 `name-ver.tar.bz2` 文件名当包名/版本。双修 + 按现网 setup.ini 重钉（qgis 4.2.3-1、qt6-devel 6.11.1-1、114 包闭包） | 与 CI 实装 114 包逐一比对零差异；selftest 加 [prev]/[test] 与 installed.db 夹具 | ✓ | pid |
| R2 | `95539cd1` fix(store)：processprobe + ProjectLock::reclaimIfOwnerGone | tst_metastore 18/18；突变①删回收分支 → staleLockRecoveredDespiteHostSpelling 红 ②探针 ESRCH 不判死 → 该用例 + processProbeStates 红 | ✓ | QSettings |
| R3 | `99779a1a` test(sandbox)：注册表 hive 沙箱 + tst_test_sandbox + PALEO_WELL_FACIES_CONFIG | Linux tst_test_sandbox / tst_wellfaciesconfig 绿；mingw -Wall -Wextra 编译过；Wine 第二档隔离成立；突变：去 path() 覆盖 → tst_wellfaciesconfig 红 | ✓（第一档待 CI） | python |
| R4 | `211e6d8f` test(python)：PALEO_TEST_PYTHON 注入 + 断言；ps1 设 PALEO_PYTHON | `-DPALEO_TEST_PYTHON=/usr/bin/python3` 下两测试绿、ctest ENVIRONMENT 含 PALEO_PYTHON；突变：unset / 指向不存在 → initTestCase 红 | ✓ | srs.db |
| R5 | `15d6b00e` fix(env)：ensure_qgis_resources.py + ps1 ensure-resources + fetch-deps.sh 补 postinst | 真实锁内两包 → 5510 文件、srs.db 13720 条 tbl_srs；selftest 绿；5 处突变全红 | ✓ | perf |
| R6 | `ade48595` test(perf)：aiassist_perf 计量自洽 | Linux 绿 | ✓（本机待验） | 防回归 |
| R7 | `9c80d339` test(env)：env_redset.json + check_env_redset.py + envdebt 标签 + ps1 单元自检 | 检查器 selftest 10 场景 + 4 结构突变绿；5 处检查器突变全红；ps1 自检绿、-Mutate 抓到 #297 旧实现、删 PALEO_PYTHON 行 → 红 | ✓ | 推送 + CI + 全量 ×2 + review |
| R8 | review R1：`8344f885`——ps1 调 python 失败（机器无 python）在 `$ErrorActionPreference=Stop` 下会终止整个 test verb → try/catch，-Auto 只告警；注册表沙箱第二档退出删键、头注释改正 | ps1 自检补 2 例，旧实现被抓；mingw 编译 + Wine 复测通过 | ✓（0 High/Medium） | 推送 → PR #304 草稿 + CI |
| R9 | Linux 全量 ctest ×2（串行，376 项，`-DPALEO_TEST_PYTHON=/usr/bin/python3`） | #1：364/376；#2：365/376。两遍共同红 11 项 = 10 项本机基线红 + tst_singlefactor_perf（L median 15573ms > 15000，名单豁免正则实测命中）。#1 多出的 tst_error_history_dock（剪贴板断言）单跑 HEAD/基线各 5/5 绿 = 全量上下文抖动。基线取证：工作区临时切回 `aa07d7c7` 源码只重编这 11 个目标 → 10 项同样红（tst_singlefactor_fieldcontours / workflows / composeworkflow / import / mapping / errorhub_storm / aichattoolloop / crossplot_controller 段错 / horizontracksvc / wellcomposite_perf），均为本盒 GDAL 3.10 / 系统 QGIS 4.2.3 / 机器性能所致，与本 PR 无关。layering / layering_strict / layering_selftest 两遍全绿；16 项环境债清单在 Linux 除 singlefactor_perf 外全绿 | ✓ | 定位源码树污染 |
| R10 | `59f8adc5` test(manifestproj)：全量后工作区出现 testdata/fixture.gpkg 二进制改动——逐个跑引用 FIXTURE_GPKG 的 10 个测试，定位 tst_manifestproj（OGR provider 以更新模式打开并写回）→ 改用进程级临时副本；`19d0dfbf` ci 步骤名更正 | 修后单跑不再污染源码树 | ✓ | 等 CI 37736364862 收尾后推送（避免并发组取消在跑的 Windows leg） |
| R11 | Windows CI 首轮（run 37736364862，HEAD 8344f885）：**Vendor 步通过**（#297 钉版双缺陷修复实证，master 此前卡在此步）；Build `-k 0` 下唯一失败 `tst_shell_projectlifecycle.cpp` 的 `<qgis/qgsproject.h>`（#297 引入，OSGeo4W 头为扁平布局，C1083）→ `885cc0a6` 改扁平头。lint（clang-tidy 增量门含 processprobe/projectlock/wellfaciesservice）✓。linux：fetch-deps.sh 新步骤在 CI 实跑（`写入 vendor/prefix.new/usr/share/qgis/resources/srs.db`），随后 paleo_selfcheck / tst_runtime 链接断（vendored deb 闭包 libarmadillo/libarpack/libpulse 未定义符号——master 既有，非本 PR）；linux-asan 装包步 exit 127（master 既有） | 修复已推，CI 第二轮 run 37742119031 | ✓ | 等 Windows Test 步 |
| R12 | 2026-10-08 15:50（UTC+8）：①本盒 GDAL 3.10 兼容补丁（src/io/constraintstore.cpp、tests/tst_realizationworkflow.cpp，仅为在 Debian GDAL 3.10 上编过）**决定丢弃、不入库**：它属于本盒环境适配，不是本方向的产物；补丁存档在盒内 `/workspace/audit/env-debt2-local-only/gdal310-local-only.patch`，Linux 全量 ×2 已在补丁在位时跑完，工作区现已干净。②CI run 37742119031 的 linux-asan 7s 失败已定性：`tools/ci_apt_qgis.sh: No such file or directory`（exit 127），ci.yml 引用了仓库里不存在的脚本；master f31ee461 run 37720706843 在同一步同样失败，属 **master 既有缺陷，非本 PR 引入**，超出本方向范围，记为 Low。③PR body 已终稿化（分类表、QSettings 决策、突变、Linux ×2、本机核验清单、Low）。推送本条会取消进行中的 run，Windows CI 结论以新 run 为准。 |

## 本机（Linux 盒）环境注记

- Debian 13 无可用 QGIS 4.2 → 装 qgis.org trixie 源 QGIS 4.2.3（postinst 生成 /usr/share/qgis/resources/srs.db）。
- 系统 GDAL 3.10 与工程用到的 3.11+ API 不符：`src/io/constraintstore.cpp`（OGRWktOptions 字段）与
  `tests/tst_realizationworkflow.cpp`（GDALSetGeoTransform 非 const 参数）打了**仅本机**补丁（行尾
  `// LOCAL-ONLY gdal3.10`），从不提交。
- 构建目录 RelWithDebInfo 每个测试 exe ~500MB 调试信息，盘满一次；改 `-Wl,--strip-debug` 链接。

## Review

- R1（`8344f885` 前）：1 Medium（ps1 无 python 时终止 test verb）已修；其余 Low 记下。
- R2（全 diff 复读：projectlock/processprobe、注册表沙箱、tst_metastore、检查器、ps1、CMake 注入面）：0 High / 0 Medium。

## 移交 / Low（PR body 同步）

- L12：ci.yml 的 linux-asan「Install QGIS 4.2 development packages」步调用了不存在的 `tools/ci_apt_qgis.sh`（exit 127）。master 上同样失败，属既有缺陷，另立方向处理。

1. 同机判据的短名匹配：锁**无 hostid**（手写/旧格式或机器 id 不可得）且两台机器短主机名相同、域不同时会被当成同机。hostid 存在且本机 id 可得时只做精确比较；本次 Qt 6.11.1 Windows CI 的 foreignHostId 用例确认该判据可用。
2. 资源包版本：deb 资源 4.2.3 vs localdeps QGIS 前缀 4.2.0（同 4.2 族）；srs.db 未跑 crssync（与 Debian 包安装后、触发器同步前一致）。
3. Windows 解 `data.tar.zst` 需 Python≥3.14 / `zstandard` / PATH 上的 zstd 之一，缺则 ensure-resources 明确报错（-Auto 只告警）。
4. 注册表沙箱第一档（RegLoadAppKey）只能在真 Windows 验证（Wine 为桩）——依赖 CI 的 tst_test_sandbox。
5. `findBasePython` 未对 WindowsApps 商店桩做防御（PALEO_PYTHON 已显式化，移交）。
6. tst_aiassist_perf 在业主本机「ctest 下 0.29s 红」的具体断言无存档，无法区分是 initTestCase 失败还是比率门；本 PR 消除计量偏置并让夹具失败可诊断，需本机复核。
7. 本 Linux 盒 10 项基线红（GDAL 3.10 / 系统 QGIS 4.2.3 / 机器性能），与本 PR 无关（R9 基线取证）。
8. tst_error_history_dock 剪贴板断言在全量上下文偶发红（单跑 10/10 绿），既有抖动。
9. CI linux / linux-asan 在 master 上既有断（deb 闭包链接、asan 装包），本 PR 不碰；Linux 侧证据来自本盒全量 ×2。
10. `vendor/manifest.json` 按现网 setup.ini 重钉，与其他改 manifest 的 PR 可能冲突。
11. perf-host 豁免按机器生效：在别的机器上这些项转绿会报 STALE（信息性，`--strict` 才算失败）。

## 接续验证（2026-10-08，Arch / vendored superbuild）

本次接续在 `/home/kevin/.codex/worktrees/env-debt2/paleo_workstation`，沿用
`goal/env-debt2-20261009` / PR #304，接续点 `dc3132c0`。主 checkout 的未提交
改动保留。依赖只读共享主仓 superbuild prefix / ONNX Runtime；独立 `build/`，
构建 `-j8`，测试 `-j1`。前述 R9 的 Debian 盒证据与本段分开；本次没有 GDAL
本机补丁。当前执行主机是 Linux，不能代替业主 Windows localdeps 的 Oracle。

### R13：已结束的 Windows CI（dc3132c0）

[run 37745497132](https://github.com/WindWang2/paleo_workstation/actions/runs/37745497132)
于 16:51（UTC+8）结束：Windows **342/348**，6 红。16 项清单中被 CI 实际
执行的 **12 个 fixed 项全部通过**：runtime、boot、metastore、7 个 QSettings
测试、两个 Python 测试。`tst_test_sandbox` 日志明确显示私有 app hive 路径，
沙箱与真实 HKEY_USERS 的双向隔离断言通过；这是第一档的真实 Windows 证据，
取代 L4 的“仅 Wine 指示性验证”。Qt 6.11.1 / Windows Server 2025。

CI 用 `-LE perf`，未执行 aiassist_perf 和三项 perf-host；不能证明这四项，也
不能证明 localdeps、文件监狱、用户既有配置或双遍稳定。原始 JUnit / QtTest
输出下载在 `build/envdebt-evidence/windows-dc3132c0/`。6 红逐条移交，不豁免：

| 测试 | 原始断言/现场 | 终态 |
|---|---|---|
| tst_workflows | faciesPolygonsDeclaredFromRaster：相多边形数 1，期望 2（tests/tst_workflows.cpp:741） | 清单外业务断言；待独立基线对照 |
| tst_composeworkflow | conformable 边界写入未拒绝；QA 报告 0，期望 1（:349/:479） | 清单外业务断言；待独立基线对照 |
| tst_import | preview.at(0) 是 zheads.dat，期望 a.las（:2132） | 清单外排序断言；待独立基线对照 |
| tst_fspath | 同一中文路径，实际反斜线、期望正斜线（:81） | 清单外 Windows 路径断言；另立修复 |
| tst_mapping | exportPdfWritesD61Map 的 layout 为 nullptr（tests/tst_mapping.cpp:1479） | 清单外业务/环境待定；保留现场 |
| tst_crossplot_controller | CTest SegFault | 不按环境抖动放行；另立诊断 |

这里不把未经 Windows 基线对照的 6 红称为“master 既有”，也不据其推定本 PR
无回归。它们不在授权真修面（pid）或 16 红面，保持红并移交。

### R14：接续 diff 自审发现并修复

| 级别 | 问题 | 修复与验证 |
|---|---|---|
| Medium | ensure-resources 没有方向 71 已有的锁时 snapshot 回退 | `7accd3a0`：仅 Ubuntu 官方 pool 回退同一 snapshot；锁内 size/SHA 继续验证；404 夹具与坏快照拒绝自检绿 |
| Medium | 两档沙箱均失败时只打印警告，测试仍可 `.clear()` 真实 HKCU | `ec882155`：ctest 必须第一档；直跑两档都失败也在 main 前退出；新增失败 hive 子进程用例（Linux 跳过；待新 HEAD Windows CI） |
| Medium | fixed 项 ctest 再红可被直跑绿掩盖，且 Python 检查器缺失时无回归保护 | `d1330a98`：PowerShell 原生读取 fixed 清单，首轮红即阻断；AST 抽函数自检及删 guard 突变实跑通过 |
| Medium | 旧 QtTest 输出可被误当本轮预算证据；空报告不能证明结果 | `d1330a98`：清首轮输出/JUnit；超时/崩溃不豁免；空/缺报告拒绝；双遍比较要求范围相同、16 项齐全、13 fixed 连续绿 |

另 `483d1b64` 新增真正持锁的子进程夹具：活持有者不可抢，kill 后 wait/reap，
探针判 Dead，另一进程接管，第三实例仍不可抢。Linux 相关测试绿。

### R15：接续资源实取与突变证据

- `ensure-resources` 实取两个锁内包到独立临时前缀，清单 **5510** 文件，
  `srs.db` **13720** 条 tbl_srs，`PRAGMA integrity_check=ok`。providers-common
  SHA `5994f6ac760d73deaf8638741940881a11f2c7947e0f70dd79299cb0ead9f079`；
  qgis-common SHA `b23097f9efe2484ebb5a589d088f2db3e1c9f331040cd379f3a80f411f3c30fb`。
- 工具突变：删 snapshot 回退、绕过摘要、将 CTest Timeout 当普通失败、缺报告
  放行、去双遍范围检查、跳过 fixed 放行，**6/6 打红**。每个影子文件先运行
  未突变控制组（绿），保持 REPO 指向本仓；证据 `build/envdebt-evidence/tool-mutations.json`。
- PowerShell 删 fixed 首轮回归 guard → 自检红，控制组绿；证据
  `build/envdebt-evidence/ps1-mutations.json`。临时 portable pwsh 7.6.6 只用于
  本次验证，不入产品依赖。
- pid：删 fallback 回收分支 → staleLockRecoveredDespiteHostSpelling 红；
  ESRCH 改 Unknown → processProbeStates/死亡探针断言红；恢复源码重编后
  tst_metastore 绿。证据 `build/envdebt-evidence/pid-mutations.json` 及各用例文本。
- layering 三档、资源/红集合 selftest、自检名单及 PowerShell 自检绿。

### 接续 review

- R16：修复 R14 后按环境正确性、真修质量、决策判据、防回归、文档五维复读，
  0 High / 0 Medium；Windows 新增失败 hive 用例的执行证据待 CI，作为明确限制。
- R17：再复读源文件与 targeted 测试/突变输出，0 High / 0 Medium；补正双遍
  汇总的计数和 Windows 六红的基线措辞，不扩大产品/视觉改动。

Windows 本机全量双遍仍未取得；BUILDING.md 已给保存两遍首轮证据并比较的命令。
本段的 Linux 全量双遍与新 HEAD CI 结果将在后续记录追加，不能把“已实现”当
“Oracle 全部通过”。

### R18：本次 Linux 全量双遍与基线对照

全量串行、含 perf、376 项：**#1 370/376，1026.67s；#2 370/376，923.68s**。
16 项环境债 **两遍全部绿**（13 fixed + 3 perf-host 原阈值通过），layering 三档
两遍全绿。失败集合相同，未观察到新漂移。原始证据分轮保存于
`build/envdebt-evidence/run{1,2}/Testing/`；清单比较输出 `double-redset.txt`。
检查器仍对六个清单外红退出 1，没有按“目标面绿”放行全量失败。

六红：workflows、composeworkflow、import、mapping、crossplot_controller
（SegFault）、wellcomposite_perf（testDepthTransformMath 浮点模糊比较）。
为排除本次产品 delta，临时用 origin/master 的 `projectlock.cpp` 与
`wellfaciesservice.cpp` 重建仅这六个目标、同前缀/夹具/env 逐项复跑；这两份
cpp 是 Linux 上全部产品行为 delta（其余两份头只改注释，新 processprobe
在旧 projectlock 中不被引用，注册表 hook 在 Linux 不构建）。**6/6 同样红，
五项断言函数相同，crossplot 同样 SegFault**。此证据是 Linux 最小产品基线
对照，不是 Windows 或 master 全量验收。源码已恢复，metastore /
wellfaciesconfig 控制组复跑绿。详见 `build/envdebt-evidence/baseline/`。

第一遍归档暴露 Linux wrapper 的 JUnit 相对路径问题：CTest 切到 build 后，
报告落在 `build/build/Testing/ctest-junit.xml`。原始报告完整，已从实际落点
回收归档；`c62b50a1` 把两条入口改为绝对输出路径，短轮验证落点/内容绿，
第二遍正常落在 `build/Testing`。无需重跑第一遍或编造报告。

`f914dc56` 让沙箱错误诊断采用 ASCII，避免 main 前错误支路受 CRT 区域编码
影响；不改变沙箱判据。新失败 hive 用例仍待本次 HEAD 的 Windows CI。

补充 Python 突变：临时删 CTest 的 PALEO_PYTHON 注入，两测试均在
initTestCase 的路径断言打红；恢复 CTest 元数据后两控制组绿。记录在
`build/envdebt-evidence/python-mutations.json`；未改产品解释器发现逻辑。

### 当前 Oracle 边界

| Oracle | 本次已有证据 | 尚缺 |
|---|---|---|
| 红集合收敛与稳定 | Linux 376 项双遍，16/16 连续绿；六个清单外红经最小产品基线对照 | Windows localdeps 全量双遍；不能以 Linux 或排除 perf 的 CI 代替 |
| 资源闭包 | 锁内包/SHA/5510 文件/13720 CRS/SQLite 完整性；Windows CI runtime/boot 绿 | 业主前缀实际落盘与两个测试本机绿 |
| pid | 真实持锁进程 kill→接管；删回收/不判死突变红，恢复绿 | 新持锁子进程用例的本次 HEAD Windows CI |
| QSettings | 保持 NativeFormat / 串行；旧 HEAD Windows 第一档隔离与七测试绿，零格式迁移 | 本机既有 HKCU 不变及七项本机双遍；新 fail-closed 用例的 Windows CI |
| Python | 绝对解释器 ctest 注入断言；两个测试双遍绿；去注入突变红；旧 Windows CI 两项绿 | 本机 conda 根 python.exe 的实际运行 |
| 防回归/层 | 清单、标签、双遍比较器、工具/PID/Python/PS1 突变与 layering 三档绿 | 本机完整结果层验收 |

R19/R20 接续终审：代码、targeted 与双遍/突变证据、Windows 限制、QSettings
三类判据和 Low 再复读，两轮 **0 High / 0 Medium**。Low 和未验项保留在 PR。
**本方向尚未取得 Windows localdeps 最终 Oracle，不标已完成。**

### R21：接续补充突变与日志级覆盖审计

在本次 Linux worktree 补充两个现场突变，各自先跑控制组、只重编相关目标
（`-j8`）、串行 ctest，
随后恢复原始源码字节并重编复跑。两组均为 **控制绿 → 突变红 → 恢复绿**：

- 去掉 `WellFaciesConfig::path()` 的环境覆盖分支：initTestCase 的路径比较失败，
  实际回到 XDG 下的产品默认路径，期望为用例私有临时文件；未放任测试继续写入
  真实用户配置。证据 `build/envdebt-evidence/followup-mutations/wellfacies-config-override-mutant.txt`。
- 在计量结果中模拟 `batchPerTileMs = 5 * singleTileMs`：
  fixtureThroughputRatioGate 的原 **4×** 断言失败（现场 1.455 / 0.291 ms）。
  这是验证比率门拒绝退化的测量输入突变，不是实际测得管线退化；正式源码与
  阈值未改。证据 `build/envdebt-evidence/followup-mutations/aiassist-ratio-mutant.txt`。

汇总 `followup-mutations/results.json` 保存每组前后控制及突变退出码。

另外逐项审计两遍 16 项的 QtTest 原始输出（不只读取 CTest 状态）：runtime
与 boot 各 13 / 5 passed；两个 Python 测试各 6 / 14 passed、无跳过；
三个 perf-host 均执行原门并通过；metastore 的真正
`killedLockHolderCanBeTakenOver` 两遍 PASS。其 `holdProjectLockChild` 在父级
总跑中按设计跳过，它已由接管用例作为子进程启动。aiassist 的合成夹具比率门
两遍 PASS，可选 966MB 真工区吞吐用例因没有数据而跳过。UI 的可选截图、外部
工作区及图形平台 WebEngine smoke 跳过保持原口径；不能据此声称原生视觉或
真实工区吞吐已验收。完整函数列表与 Totals 保存于
`build/envdebt-evidence/oracle-coverage.json`。

旧 Windows `dc3132c0` 的 12 项也逐项核对：两个 Python 测试的 roundTrip /
脚本执行用例实际 PASS，仅 Unix-specific 孤儿进程断言跳过；native registry
sandbox 的隔离用例实际 PASS。该日志仍不能证明 conda 根解释器的 localdeps
接线。对应覆盖记录 `windows-dc3132c0/oracle-coverage.json`；本次行为代码对应提交
`0f0a02f3` 的 Windows CI run 37758942925 仍在运行，未用旧 HEAD 冒充新用例证据。

本批仅补充证据和覆盖范围说明。五维自审两轮：环境与平台口径、真锁恢复的
执行覆盖、QSettings 选择、突变与恢复、防回归及文档表述均与原始日志一致，
**0 High / 0 Medium**。Windows 本机执行入口尚未取得，最终 Oracle 继续未验。

### R22：按用户指令停止等待 CI，提交当前证据

用户明确要求“不要等CI”。已停止本地 `gh run watch`，不再等待或轮询远端
运行；远端任务不由本方向手动取消。后续提交仅补充 ledger/PR 的证据边界，
产品、测试与工具行为仍对应 `0f0a02f3`。

停止等待前的最后一次观测：Windows run 37758942925 的 Build 和 Selfcheck
成功，串行测试仍执行中。未取得该轮测试结果，不能声称新 fail-closed 或真实
持锁子进程用例已在 Windows 通过；旧 `dc3132c0` 的结果仍只证明旧版本。

本次 Linux 双遍及突变证据已完成，PR 保持草稿。最终阻塞是尚无可访问的
Windows localdeps 本机执行入口或完整双遍结果，无法验证业主的 prefix、conda
根解释器、真实 HKCU 保持及全量收敛。此前已请求入口，当前未获提供。
该限制不因停止等待 CI 而解除，方向不能标为 Oracle 已通过。

本批文档五维复读两轮：对照原始双遍/突变记录、最后已观测的 CI 阶段与平台
边界，确认没有新增行为改动、扩大验收结论或遗漏既有 Low，均
**0 High / 0 Medium**。不为文档提交重复全量测试。

### 合并事实补记（方向 100，2026-10-10）

上节 R22 为方向执行时点的边界记录。合并事实：**PR #304 已于 `b23fe459`
（方向 81 环境债二期，2026-10-09）合入 master**，本账本随之入库；「PR 保持
草稿」等尾段描述的是 R22 时点状态，不再反映现状。R22 记录的未验项不变：
**Windows localdeps 本机执行入口（业主 prefix、conda 根解释器、真实 HKCU
保持及全量收敛）的 Oracle 仍欠**，见 R22 边界——本补记只登记合并事实，
不扩大验收结论。
