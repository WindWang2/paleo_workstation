# Goal-Loop Ledger — 方向 70：Sanitizer 常态化与构建硬化

Oracle（验收，逐条要证据）：
1. PALEO_SANITIZER_BUILD 定义接线 + 三个 budget 测试降档 + 降档单测 + 非 sanitizer 对拍零差异
2. setError/connectionNameFor 文件级定义 35+8 收敛（rg 证据）；paleo_store/paleo_qgis 合批编译过
3. Unity 10 目标全编过 + ctest 绿（或如实记录剩余冲突）；CMakeLists 注释与实测一致
4. ci.yml 含 linux-asan（不等待运行）
5. measure_incremental.sh 可跑 + BUILDING.md ET14 当前规模口径
6. 全量 ctest 对照 R0 红 diff 空 + layering 三档绿

## 现场事实（勘察结论，基线 2ab3310 = origin/master，任务书 f130fb2 已前移）

- setError 文件级定义 35 处（同构：`if (error) *error = text;`）：
  metadata 9 + catalog 2（同编 paleo_store，互依环）、qgis 8（paleo_qgis）、
  algorithms/stratgrid 6（paleo_algorithms）、io 4（paleo_io）、workflow 6（paleo_workflow）。
  既有共享 inline 先例：io/dataimport_internal.h（paleo::dataimport_detail）、
  workflow/workflowerrors_internal.h（paleo::workflow_detail）——调用点用
  `using paleo::X_detail::setError;`（置于匿名 namespace 内）接线。
- **connectionNameFor 8 处不是同构**（任务书「同构实现」判断与实测不符）：
  前缀/身份分量各不相同（catalog 带 self 指针+绝对路径 hash、layermanifest 带
  线程 id、mapversion 带 _ro 后缀、其余 4 个纯 hash）——合一即行为变化（红线），
  走每文件改名（file-local 匿名符号，改名零外部影响）。偏离 oracle「1+1 处」
  字面，按诚实面记录：setError 35→0（共享 inline 5 处），connectionNameFor
  8→0（每文件唯一名）。
- io 目标 unity 隐患：dataimport_detail 与新增 io_detail 若并存同名 setError，
  混批 TU 内两条 using 导入不同实体同签名 → 调用点歧义。对策：
  dataimport_internal.h 改为 `using paleo::io_detail::setError;` 委托，
  全目标单一 canonical 定义。
- 三个 budget 测试失败面（devex.md ASAN 轮）：
  tst_correlation_full 墙钟（3000/9000/12000 预算 + 1000ms warm reorder）、
  tst_perfbudget（100/500/100ms）、tst_segy_lines RSS（3MB 预算 vs 8.2MB 载荷，
  ASAN 红区放大 ~5MB——放宽 ×3 = 9MB > 载荷，断言空洞；走「跳过增长断言」，
  devex 递延 6 原文授权「降档/跳过」）。
- MSVC 本机：sanitizer 档不可跑（CMakeLists 明确拒绝）；本机验证 = 定义接线
  编译 + 降档单测双二进制（on/off）；CI yaml 人工核对不等待。

## 轮次

第0轮 | worktree 建立（2ab3310）、cmake 配置 OK（vcvars64 + Qt CMake + vcpkg ninja）；基线构建后台进行中 | 进行中 | —
第1轮 | 基线构建 890/1912 链接失败：worktree 无 gitignored vendor/onnxruntime → paleo_ai 空壳变体 → aiassistworkflow.cpp（上游无 PALEO_HAVE_ORT 守卫，2ab3310 已知面非本方向引入）引用 suggestHorizonTracking/runTileInference 无法解析。主仓同样无 vendored ORT（主仓 exe 为陈旧产物，主仓 paleo_ai.obj 同为空壳变体）。修复：手动取官方 onnxruntime-win-x64-1.30.0.zip（版本对齐 manifest linux 钉 1.30.0；manifest 未钉 win 哈希——本机一次性环境补齐，不入库不改 manifest）入 vendor/onnxruntime/{include,lib}，重配 PALEO_HAVE_ORT=TRUE 续建 | 续建中 | —
第2轮 | 续建到 tst_onnx 链接红：AUTOMOC 陈旧（变体切换后 autogen 未重扫，mocs_compilation 是空壳时代产物）。rm timestamp 强制重扫修复；全量构建 NINJA_EXIT=0 | 通过 | R0 ctest 启动
第3轮 | R0 ctest 首轮 255 红：双重环境坑——Git Bash 把 TEMP=/tmp 传子进程（QTemporaryDir 全灭）+ System32 旧 onnxruntime.dll 抢占（0xc0000139）+ qgis_core 按 Qt6.11.2 构建（deps/Library 的 Qt 必须排官方 Qt6.8 前）。修：build-bat/testenv.sh 本地环境脚本 + onnxruntime.dll 拷 exe 旁。R0 终值 58 红/361（84% 过，Windows 环境性红为主），红集合存 /tmp/r0-red.txt | 通过 | —
第4轮 | Unity 清障主体：setError 35→5 共享头（storeerrors/qgiserrors/algoerrors/ioerrors/workflowerrors）；connectionNameFor 8→0（语义各不同：self 指针/线程id/_ro 后缀/纯 hash——任务书「同构」判断与实测不符，按红线走改名）；ensureOpen 7→0 改名（同因：3参 vs 2参两类）。unity r1 实证 store/algorithms 新冲突 | 进行中 | —
第5轮 | Unity 冲突 4 波迭代（-k 0 收全量 + mega-TU 扫描器前瞻）：kNan×3→inversion_internal.h、cluster stopped/report→cluster_internal.h、failure×3 改名（返回类型撞车）、geosArea→faciesmapping_internal.h、ensureGdalRegistered×3→gdalreg_internal.h、addIssue→parserissues_internal.h、sha256Hex+sfpkg 常量→sfpkg_internal.h、sidecar 族→singlefactor_internal.h、Cur/WalkResult/compressionFlags/kRowId/resolveProject/GridSpec·readGrid/safeSegment/kNoData/kInf/kFileNodata/pointOnSegment/polylineLength 改名、windows 宏撞名 2 处（Qt slots→curveexpr 参数改名 valueSlots、rpcndr small/large→ribbonpanels lambda 改名）、Polygon 歧义（wingdi ::Polygon）→全限定、seismic3d Normalize 族→seismic3d_internal.h、kTextMuted/kBorder→correlation_internal.h、isOffscreen→uienv_internal.h、mutedCell 复用 datalistops.h、mono8→datapreview_internal.h | unity r7 全绿 | UNITY_EXIT=0，10 目标全编过
第6轮 | R1 增量重建（首次因 link.exe 增量链接 AV 抖动重试一次后全绿）+ R1 ctest 对照进行中；layering 三档绿；measure_incremental.sh 实测（1 TU + 18 relink = 20 步 15.4s，修 preflight [0/N] 误判 bug）；BUILDING.md ET14/devex.md/CMakeLists 注释同步；ci.yml linux-asan + yaml 验证 | 进行中 | —
第7轮 | **Oracle 全项达成**：R1 ctest 363 项 85% 过（56 红 vs R0 58 红，**新增红 = 空集**，转绿 2）；tst_sanitizer_budget(5/5)+_on(5/5)+tst_perfbudget+tst_segy_lines+tst_correlation_full R1 全绿；4 个原子提交（refactor(src) 清障 / feat(build) 接线降档 / ci linux-asan / build(tools)+docs）；第二遍验证（strict layering+双档单测+rg 计数）全绿 | 完成 | —

## Oracle 验收证据汇总

1. **PALEO_SANITIZER_BUILD**：CMakeLists `add_compile_definitions(PALEO_SANITIZER_BUILD=1)`（ASAN||UBSAN 时，MSVC 仍被 FATAL_ERROR 先挡）；tst_sanitizer_budget 双二进制 5/5+5/5 PASS（系数=1/3 逐值断言）；非 sanitizer 对拍 = R1 新增红空集 + 系数 1 档基线原值断言。
2. **Unity 清障 rg 证据**：setError 文件级定义 35 → 5（storeerrors/qgiserrors/algoerrors/ioerrors/workflowerrors 五头，io 侧 dataimport_detail 经 using 委托单一 canonical）；connectionNameFor 8 → 0（每文件唯一名：catalogConnectionName 等 8 个）。paleo_store/paleo_qgis 合批编译过（unity r7）。
3. **Unity 终态**：10 目标 UNITY_BUILD BATCH_SIZE=8 全编过（UNITY_EXIT=0，MSVC/Ninja 本机实证）；CMakeLists 注释改写为实测口径 + mega-TU 超集余量清单（cluster valid·distance / PALEO_NODATA / qssHeight / QFont helper——源序重排可复暴露，如实记录）；默认仍 OFF（全量 ctest 长跑未做，devex 说明）。
4. **CI job**：ci.yml linux-asan（continue-on-error 观察档）；yaml 解析验证 job 键/step 折叠/run 命令正确；未等待其运行（用户门禁语义）。
5. **增量基线**：tools/measure_incremental.sh 本机跑通（JSONL 落盘：tu_ratio 1/475、step_ratio 20/5743、wall 15.4s 仅记录）；BUILDING.md ET14 段刷新为当前规模口径。
6. **对照**：R0 58 红/361 → R1 56 红/363（+2 新测试全绿），comm -13 diff = 空；layering/layering_strict/layering_selftest 三档绿（selftest 内 FAIL 行为夹具预期，退出码 0）。

## 环境坑位存档（后续方向复用）

- Git Bash 下 ctest：TEMP=/tmp 会传子进程致 QTemporaryDir 全灭——必须 export Windows 路径 TEMP/TMP。
- 本机两套 Qt：qgis_core 按 deps/Library 的 Qt 6.11.2 构建，PATH 序错（官方 6.8.0 前置）→ 0xc0000139 入口点缺失；Library/bin 必须前置。
- System32 有旧 onnxruntime.dll，DLL 搜索序先于 PATH——exe 旁拷贝 vendor 1.30 副本压制。
- worktree 无 gitignored vendor/onnxruntime：本机 Windows 侧 manifest 只钉 linux-x64 包；手动取官方 win-x64 1.30.0 zip 铺 vendor/onnxruntime/{include,lib}（版本对齐；manifest 未钉 win 哈希，不入库）。
- AUTOMOC 变体切换（空壳 paleo_ai→完整）后 mocs_compilation 陈旧：rm build/<tgt>_autogen/timestamp 强制重扫。
- MSVC 增量链接偶发 Access violation：重跑即过（工具链抖动非代码）。


