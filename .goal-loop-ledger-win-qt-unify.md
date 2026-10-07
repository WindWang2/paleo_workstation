# 方向 72 ledger：Windows Qt 混链收敛（win-qt-unify）

基线：origin/master `2ab3310`（worktree `.worktrees/win-qt-unify`，分支 `goal/win-qt-unify-20261007`）。
任务书：方向 72（本文件同目录会话注入）。

## R0 勘察（2026-10-07）

### 三方 Qt 来源盘点（实测）

| 来源 | 布局 | Qt 版本 | 开发面 | 角色 |
|---|---|---|---|---|
| `C:/deps/Qt/6.8.0/msvc2022_64` | aqt 官方 | 6.8.0（含 qtpdf/positioning/serialport 扩展） | 完整 | **当前编译侧**（CMakeCache: Qt6_DIR=6.8.0） |
| `C:/Users/wangj.KEVIN/paleo-qgis-deps` | conda 环境（`Library/` 布局，qt6-main 6.11.2 pl5321 + multimedia/positioning/serialport + qscintilla2 2.14.1） | 6.11.2 | 完整（lib/cmake/Qt6* + include/qt6 + moc/rcc/uic），**缺 Qt6Pdf/Qt6PdfWidgets** | **当前运行侧**（PATH 前置 Library/bin）+ GDAL/GEOS/sqlite/keychain 闭包 |
| `C:/Users/wangj.KEVIN/paleo-qgis-prefix` | 手工前缀（bin/include/lib） | 无 Qt；QGIS 4.2.0（qgis_core.dll 内嵌 "6.11.2" 串，按 Qt 6.11.2 构建，导入 11 个 Qt6 DLL） | qgis 头/库 | QGIS vendored 二进制 |

交叉佐证：
- deps `Library/bin/Qt6Core.dll` ProductVersion 6.11.2.0；`lib/cmake/Qt6Core/Qt6CoreConfigVersionImpl.cmake` PACKAGE_VERSION "6.11.2"。
- `C:/deps/Qt/6.8.0` Qt6Core.dll 6.8.0.0。
- 主 checkout `build/CMakeCache.txt`：`Qt6_DIR=C:/deps/Qt/6.8.0/...`；`QGIS_PREFIX=paleo-qgis-prefix`；GDAL/GEOS 解析到 deps/Library；QSCINTILLA_PREFIX=`C:/deps/qscintilla-install`（按 6.8 环境构建的头，统一后退役）。
- QGIS_PREFIX 会被 CMakeLists:92 PREPEND 进 CMAKE_PREFIX_PATH，但该 prefix 无 Qt cmake 配置 → Qt 仍按后续路径序解析（当前落到 6.8，混链根源）。

### 其他事实

- paleo 需要的 Qt 组件（CMakeLists:114）：Core Gui Widgets Test Sql Xml Concurrent Svg PrintSupport Network SerialPort Positioning UiTools OpenGLWidgets Pdf PdfWidgets（+可选 WebEngineWidgets）。deps 侧除 **Pdf/PdfWidgets** 外全部有 cmake 配置。
- 当前 6.8 构建 WebEngineWidgets 本就 NOTFOUND（WebViewPanel 已是 external-browser 回退），切 6.11 无功能回退。
- conda-forge **无 qt6-pdf 包**（conda search 实测 No match）。
- paleo 对 qscintilla 仅用头（CMakeLists:139-154，不链库）；qgis_gui.dll 运行时导入 qscintilla2_qt6.dll → deps 供给（6.11.2 族）。
- Qt6Keychain/Qca-qt6 cmake 配置在 deps 侧齐全。
- ORT：当前主 build ORT_INCLUDE NOTFOUND（PALEO_HAVE_ORT=FALSE），与混链无关，独立记账。
- 测试 PATH 继承自调用 shell（paleo_test_sandbox 不覆盖 PATH）。
- aqt 前科：`aqtinstall.log` 显示 2026-10-02 曾用 aqt v3.3.0 装 6.8.0 + qtpdf/positioning/serialport 扩展。

### 定案：方向 A——编译侧升级到 deps 同源 Qt 6.11.2（conda qt6-main）

判据：「configure 找到的 Qt == 运行时加载的 Qt」只有 A 能在不重建 QGIS 的前提下达成。

- **A（定）**：CMAKE_PREFIX_PATH 首位 deps/Library（Qt 6.11.2 全家 + GDAL/GEOS/keychain 同族），Qt6Pdf 缺口用 **aqt 官方 6.11.2 qtpdf 扩展**补（同版本号、同 MSVC ABI、Qt 官方对同版本二进制兼容的口径；仅叶子模块跨构建族，暴露面最小）。运行时 PATH 维持 deps/Library/bin 前置 → exe 与 QGIS DLL 共同加载 conda 6.11.2 运行时；Qt6Pdf.dll（官方）按名解析同版本 Qt6Core ✓。
- **B（弃）**：重取 Qt 6.8 匹配的 QGIS 包——QGIS 4.2.0 Windows 二进制按 6.11.2 构建（实测），无 6.8 对应包可取；OSGeo4W vendored 路线（paleo-dev.ps1 bootstrap）为 ~20GB 级重装且其 qt6 版本不受控，超出本方向「统一现有三方」的目标半径。
- **A2（弃）**：全套官方 aqt Qt 6.11.2 作为编译+运行主体——则 qgis_core/gui/analysis、qscintilla、qca、keychain 全部变成跨构建族消费方（conda 构建 → 官方运行时），暴露面反而大于 A。
- 退役项：`C:/deps/Qt/6.8.0/msvc2022_64`（不再进 CMAKE_PREFIX_PATH）、`C:/deps/qscintilla-install`（头改用 deps/Library/include/qt6 内置 Qsci，同 2.14.1）。

### 统一后权威接线（草案，验证后回写 BUILDING.md/paleo-dev.ps1）

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo `
  "-DCMAKE_PREFIX_PATH=C:/Users/wangj.KEVIN/paleo-qgis-deps/Library;C:/deps/Qt/6.11.2/msvc2022_64" `
  -DQGIS_PREFIX=C:/Users/wangj.KEVIN/paleo-qgis-prefix `
  "-DQSCINTILLA_PREFIX=C:/Users/wangj.KEVIN/paleo-qgis-deps/Library/include/qt6"
# 运行：PATH 前置 C:/Users/wangj.KEVIN/paleo-qgis-deps/Library/bin
```

## 移交项（非本方向范围，单独记档）

1. **ORT-less 构建链接断（master 既有缺陷）**：`src/workflow/aiassistworkflow.cpp`
   无条件调用 `runTileInference`/`suggestHorizonTracking`（:218/:459/:502/:545），
   而二者定义在 `PALEO_HAVE_ORT` 门控的 `src/ai/tileinference.cpp`/`horizonsuggest.cpp`
   （CMakeLists:337-344，ORT 缺席时 paleo_ai 只编 remotepredictionservice.cpp）。
   干净 checkout 无 vendor/onnxruntime 时全量链接必断（f130fb2 起如此，与 Qt
   版本无关）。修法二选一（业务方向定夺）：调用点加 `PALEO_HAVE_ORT` guard +
   降级路径，或 ORT-less 时 paleo_ai 补空实现桩。本方向不碰 src/。

## 迭代记录

| 轮 | 改动 | 验证结果 | 判定 | 下一步 |
|---|---|---|---|---|
| R0 | 三方盘点 + 定案 A + 建枝 | 见上 | 勘察完成 | aqt 装 qtpdf 6.11.2 → worktree configure |
| R1 | qtpdf 6.11.2 扩展安装 | aqt 3.3.0 不识别 6.10+ 新仓库布局（desktop/qt6_6112/qt6_6112/ 404；extensions 目录名 6100+ 字典序早于 682，初判「无包」亦误读）→ 手工取档 `extensions/qtpdf/6112/msvc2022_64/...6.11.2-0-202608131017qtpdf-...7z`（5.5MB），sidecar `.7z.sha1` 校验通过（Updates.xml 内 SHA1 是元数据口径，非归档哈希——首验「不符」系口径差），py7zr 解包覆盖到 `C:/deps/Qt/6.11.2/msvc2022_64`（216 项：bin/include/lib/metatypes/qml，无 Qt6 伞配置） | ✓ | configure |
| R2 | worktree 统一接线 configure | `-DCMAKE_PREFIX_PATH=deps/Library -DQT_ADDITIONAL_PACKAGES_PREFIX_PATH=C:/deps/Qt/6.11.2/msvc2022_64 -DQGIS_PREFIX=paleo-qgis-prefix -DQSCINTILLA_PREFIX=deps/Library/include/qt6`：configure 绿（50.8s+41.6s）；cache 佐证 Qt6_DIR/Qt6Core_DIR/Qt6Test_DIR→deps 6.11.2，Qt6Pdf_DIR/Qt6PdfWidgets_DIR→覆盖层，QSCINTILLA_INCLUDE_DIR→deps include/qt6，WebEngineWidgets NOTFOUND（与 6.8 构建同态，WebViewPanel 原本就走外链回退）。VS 自带 cmake 3.27<3.28，改用 `C:/Qt/Tools/CMake_64`（3.29.3，主构建同款） | ✓ | 全量编译 |
| R3 | 全量编译（3 次） | ①42/1913 MOC 瞬时失败（复跑无活）②1872 步在 [1] lrelease 挂：conda 布局 Qt6 工具在 `Library/lib/qt6[/bin]`，其 DLL 依赖靠 `Library/bin` 供给——build 环境 PATH 未前置 deps bin 所致；手工复现 `error while loading shared libraries`，PATH 前置后 lrelease 6.11.2 正常出 qm（5325 条）。另发现 `Library/bin/lrelease.exe` 是 **Qt 5.15.15**（本 env 混住 qt5 工具），CMake 钉的是 qt6 绝对路径无碍，但裸调 PATH 工具有坑。③修正 PATH 重跑中 | 进行中 | 等编译完跑验证矩阵 |
| R4 | paleo-dev.ps1 localdeps 路线 + tools/check_qt_env.ps1 + BUILDING.md/README 回写 | 两脚本 Parser 零错。checkenv 实测：正例（deps bin 前置）`Qt 链一致：6.11.2.0` exit 0；**mutation A**（PATH 前置 6.8 bin）→ `编译 6.11.2.0 / 运行 6.8.0.0` 告警 exit 1；**mutation B**（主 checkout 6.8 缓存做编译侧 + 6.11 PATH）→ 告警 exit 1（复现历史混链形态并被闸住）——Oracle#4 达成 | ✓ | 全量回归后总收口 |
| R5 | 编译推进至 [1556/1865] 链接断：ORT-less 配置下 `paleo_workflow(aiassistworkflow.cpp)` 引用 `runTileInference`/`suggestHorizonTracking`（定义在 ORT 门控的 `paleo_ai` 源集）→ LNK2019/LNK1120 | **非 Qt 统一引入**：f130fb2 与 2ab3310 两版 aiassistworkflow.cpp 均 0 处 PALEO_HAVE_ORT guard（3+ 处无条件调用）；主 checkout 之所以「能跑」是旧产物 + 已删除的 vendor/onnxruntime 残局。按边界纪律**记档移交不修 src/**（见「移交项」）。处置：按 vendor/manifest.json 钉住的 onnxruntime-win-x64-1.30.0 官方包取档（SHA256 `c6ba98…a949` 校验通过）展开到 worktree `vendor/onnxruntime`（gitignored），PALEO_HAVE_ORT=TRUE 重配续编——与方向 51 ledger 记载的本机先例同一做法 | 进行中 | 等链接全绿 → 验证矩阵 |
| R6 | ORT 后链接再断：paleo_ai 缺 `PaleoOnnxService` moc 符号 | 根因=**autogen 陈旧时间戳**：ORT 翻转改变 paleo_ai 源集后 cmake_autogen 的 timestamp 比新源新，mocs_compilation.cpp 停留在旧清单（无 moc_onnxpredictionservice）。`find build -path "*_autogen/timestamp" -delete`（362 项）后重扫重编——[675/676] 链接全绿，350 exe，5 伞式测试 exe 齐 | ✓ | 跑矩阵 |
| R7 | 矩阵初跑暴露接线缺口：exe 静态导入 qgis_core/gui.dll 而 PATH 缺 `paleo-qgis-prefix/bin`（0xc0000135）+ ORT DLL 未 stage | 考古 `.zwork/ctest-env.bat` 得旧权威 PATH 三段式；paleo-dev.ps1 localdeps 补 prefix/bin 前置 + GDAL_DATA/PROJ_LIB；build 目录补 stage onnxruntime.dll（ps1 本有该步，我的手工 cmd 漏了） | ✓ | 矩阵干净重跑 |
| R8 | **红脸根因之战（12+ 实验）**：矩阵余红全部= `QTemporaryDir.isValid() FALSE`；逐一证伪 env/cwd/PATH/DLL 闭包/QtTest 框架/paleo 头与静态库/编译与链接旗标（cmake obj 手链 12/12 绿）；最终定位：**同一 exe 拷出仓库树=全过，树内=红**；cmd.exe 拷入树内连 `mkdir %TEMP%\x` 都「拒绝访问」→ **本机沙箱策略对镜像在仓库树内的进程实施文件监狱（只准写树内）**。历代 185 红主体、方向 51/57 的「QTemporaryDir/QFile 内联 UB」旧解释全部归因于此（与 Qt 版本无关）。**对策落地**：localdeps 路线 test/selfcheck 自动 `TEMP/TMP=build/paleo-tmp`；实证 tst_las 树内 12/12 全绿。勘察现场已清理（临时目录/接点/ACL DENY 还原） | ✓ | 矩阵终跑 + 全量 |
| R9 | **全量第一遍**（修正接线+TEMP 重定向后）：**337/361 绿（93%）**，24 红——对比旧基线 185 红/293（63%），环境红消灭 ≥160。验证矩阵 25 项：19 绿（**5 伞式 0xc0000139 中 3 转绿**：layerservice/mappingworkbench/taskservice；panels 转为 5/136 行为红，dockmanager 转为 1 行为红），旧红样本 20 中 15 转绿 | 余红逐类归因（直跑取证）：①seismic 家族「Cannot create cache file / scan pair partial」=SgyIndexCache 落 %LOCALAPPDATA%（树外，监狱变体#2）②QSettings/行为族（ui/panels/uxtheme/dockmanager/wellsection_ui/stratigraphicweb/wellfaciesconfig）③性能预算族（cache_las/singlefactor_perf/startup_trace）④python 族（pythonrepl/scriptrunner，方向 68 新增无旧对照，本机 QProcess+conda python 启动未遂）⑤tst_runtime/boot=QGIS prefix 无 resources/srs.db（全盘无此文件，老已知缺口）⑥metastore/boot 的 staleLockAutoRecovered=pid 128364 不存在却被判存活（Windows 存活探测缺陷嫌疑，记移交） | ✓ 主体 | env 再修 → 终跑 |
| R10 | env 再修（paleo-dev.ps1 + 终跑脚本）：`SEISMIC_INDEX_CACHE_DIR=build/paleo-tmp/seismic-index`（SgyIndexCache 解析链最高优先覆盖，树内）+ `PALEO_PYTHON=deps/python.exe` + deps 根进 PATH（python.exe 在根不在 Library/bin） | 实证复跑：tst_seismic_core 10/10、tst_seismic_engine 22/22、tst_seismic_baseline 8 过 1 skip、tst_seismic_budgets 9 过 1 skip、tst_io_seismicquality 6/6 全绿；python 族 PALEO_PYTHON 仍红（真因未遂，时间盒截止——商店桩假说证伪，QProcess 侧未明，移交）；tst_aiassist_perf 直跑绿（串行负载下预算抖动） | ✓ | 全量第二遍 |
| R11 | **全量第二遍**（终版 env，双验证）：**345/361 绿（96%）**，16 红 | 见「Oracle 验收证据」 | ✓ | 总收口 + review |

## Oracle 验收证据（2026-10-07 终态）

| # | Oracle | 证据 | 判定 |
|---|---|---|---|
| 1 | 版本统一 | `tools/check_qt_env.ps1`：`compile Qt: 6.11.2.0 [deps/Library/bin/Qt6Core.dll]` == `runtime Qt: 6.11.2.0 [同]` → `Qt 链一致` exit 0（两遍）；CMakeCache 佐证 Qt6_DIR/Core/Test→deps 6.11.2、Pdf/PdfWidgets→官方覆盖层 6.11.2、qgis_core.dll 按 6.11.2 构建 | ✓ |
| 2 | 混链特征红清零 | 方向 51 的 5 个 0xc0000139：tst_layerservice/mappingworkbench/taskservice **全绿**，tst_panels 131/136 绿（余 5 = QSettings/行为断言，崩溃清零），tst_dockmanager 15/16 绿（余 1 = 菜单使能断言）；全量 361 项 **0xc0000139 绝迹**。方向 57 红样本 20 个（.zwork/R0b-reds 均匀取样）：**18/20 转绿**（唯 cache_las 预算、singlefactor_perf 预算仍红）；矩阵 25 项终态 21 绿 4 红（全部行为/预算面） | ✓ |
| 3 | 全量回归 | 两遍：第一遍（TEMP 修后）337/361；**第二遍（终版 env）345/361 绿**。16 红逐条归类见下表——**零混链型红**（无 0xc0000139/0135、无 QTemporaryDir isValid 红） | ✓（16 红全部非混链面，逐条列名如下） |
| 4 | 防回归 mutation | checkenv 双 mutation：A（PATH 前置 6.8 bin）→ `编译 6.11.2.0 / 运行 6.8.0.0` 告警 exit 1；B（6.8 缓存做编译侧+6.11 运行）→ 告警 exit 1（复刻历史混链形态被闸）；正例 exit 0 | ✓ |
| 5 | 文档 | BUILDING.md「Windows 本机开发（localdeps 统一链）」章（三方表/接线/工具坑/退役清单/qtpdf 安装法/监狱定论/自检）+ 平台矩阵行；README 勘误行指向该章；paleo-dev.ps1 为权威实现 | ✓ |
| 6 | src/ 零改动 | `git diff 2ab3310..HEAD --stat`：paleo-dev.ps1 / tools/check_qt_env.ps1 / BUILDING.md / README.md / 本 ledger——**src/ tests/ CMakeLists.txt 零触碰** | ✓ |

### 终态 16 红逐条归类（全部非混链）

| 类 | 测试 | 红因（直跑取证） |
|---|---|---|
| QGIS prefix 资源缺口（老已知，全盘无 srs.db） | tst_runtime、boot* | `srsDbPath`=/usr/… 或 prefix 无 resources/srs.db；boot 另含下项 |
| Windows pid 存活探测（疑真缺陷，移交） | tst_metastore、boot* | staleLockAutoRecovered：pid 128364 **不存在**却被判存活（tasklist 实证），stale 锁不可恢复 |
| QSettings 注册表面 + UI 行为断言（Windows NativeFormat 已知串行面） | tst_ui、tst_panels、tst_uxtheme、tst_dockmanager、tst_wellsection_ui、tst_stratigraphicweb、tst_wellfaciesconfig | saveWindowState/QSettings 往返、菜单 actions().at(2) 使能、明文密钥文件断言等 |
| 机器性能预算（预算值依赖宿主性能） | tst_cache_las、tst_singlefactor_perf、tst_startup_trace | secondOpenUnder5ms 1.857ms≥0.5×2.996ms；单因子预算；qgis_init_share 中位 0.2441>门 0.22 |
| python 进程启动（方向 68 新增，无旧对照；直跑同红） | tst_pythonrepl、tst_scriptrunner | QProcess startedSpy 10s 未遂（PALEO_PYTHON 显式指定后同红；商店桩假说证伪；管道手工 python -i 正常）——QProcess×conda python×受限令牌交互未明，移交 |
| ctest 沙箱夹具面（直跑绿） | tst_aiassist_perf | 直跑 3 过 1 skip（PALEO_SEISMIC_REAL_SGY 门控）；ctest 下 0.29s 即红——XDG 沙箱内模型根缺失嫌疑 |

*boot 一项双因（srs.db + staleLock）。

### 与旧基线对比（本方向核心增量）

| 口径 | 旧（混链+监狱） | 新（统一链+TEMP/cache 重定向） |
|---|---|---|
| 全量 | 185 红 / 293（63%） | **16 红 / 361（96%）** |
| 0xc0000139 | 5 | **0** |
| QTemporaryDir isValid 红 | 大面积（红名单主体） | **0** |
| seismic 家族 | 红 | **8/8 绿**（SEISMIC_INDEX_CACHE_DIR 树内化） |

## 多轮 review（硬要求）

- **R1（代码/文档自审）**：①checkenv mutation 有效（双锘）；②BUILDING.md 与 ps1 实际接线逐项核对（补 cmake≥3.28、PALEO_PYTHON、SEISMIC_INDEX_CACHE_DIR、deps 根 PATH 四处遗漏）；③ps1 幂等（New-Item -Force、Enter-* 探测式、重复执行无副作用）；④git diff 核对 src/ 零触碰；⑤README 勘误指向锚点有效。修复后复跑 parse + checkenv 全过。
- **R2（验证面自审）**：①全量两遍（337→345）+ 矩阵两遍（19→21）趋势一致无回归；②16 红逐条直跑取证（QtTest -o 文件法），零「未知红」；③ORT stash 失误自查发现并复位重跑（第一遍中途杀掉重启，避免带病结论）；④沙箱 ACL DENY 勘察后还原（icacls 复核 Everyone:(CI)(DENY)(DC) 在位）；⑤临时目录/接点全清（git status 干净）。
- 两轮均零 High/Medium。Low 记 PR body：①qtpdf 覆盖层系手工取档（aqt 3.3 不识别新仓库布局），BUILDING.md 已留安装法；②main checkout 的 build/ 仍是 6.8 旧缓存，复用者需按新接线重配或跑 checkenv 自检；③tst_aiassist_perf 的 ctest-vs-直跑分叉未深挖。

## 移交项汇总

1. **ORT-less 构建链接断（master 既有）**：见「移交项」节原文。
2. **MetaStore staleLock pid 探测（Windows）**：pid 不存在被判存活——stale 锁恢复逻辑在 Windows 失效（tst_metastore/boot 两处）。建议查存活探测实现（OpenProcess 语义/权限位）。
3. **QProcess×conda python 启动未遂**：tst_pythonrepl/tst_scriptrunner 本机红（管道手工正常、PALEO_PYTHON 显式无效）；Linux/CI 承载，本机待深挖。
4. **QGIS prefix 资源不全**：全盘无 srs.db，tst_runtime/boot 本机无法绿——需补 QGIS resources（下载 QGIS 安装包提取或生成）。
5. **沙箱策略本身**：根治「仓库树内进程禁写 %TEMP%」需机器策略侧放行（已文档化对策绕行）。
