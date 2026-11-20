# P4 原生 ONNX 预测准入、血缘与发布闭环（2026-11-19/20）

任务：P4（内部代号 S2，方向 B：地质分析身份与成果发布）。
分支：`codex/paleo-p4-onnx-prediction-20261119`（worktree
`../paleo-p4-onnx-prediction-20261119`）。基线：origin/master
`08ca3e52b829f928d38e34d3f7fa3e58376f6076`（2026-11-20 核对，当时与
prompt 证据锚点一致）。最终 head：`5e027a22`（4 个提交；
origin/master 提 PR 前复核未移动）。

## 一、开工核对（scope ledger）

1. 主 checkout `master@c310833a` 落后 origin/master 39 提交，未提交改动仅
   `.zcode/` 与 `docs/progress/review-from-zero-20261019.md`（保留未动）。
   origin = `https://github.com/WindWang2/paleo_workstation.git`。
2. 开放 PR（2026-11-20）：#1049（Windows 离线供货缓存）、#1050（ORT
   Windows 加载位置/便携包自检，base 为 #1049 分支）、#1051（P2 SEG-Y，
   base master）、#1052（离线资源恢复）。#1049/#1050 只动 cmake 依赖接线
   与 Windows 打包脚本，不与 P4 的 `src/ai`、`src/workflow` 写集重叠；
   #1051/#1052 不触预测链。无未提 PR 的预测链分支（`git branch -a` 核对，
   同批 P1/P2/P3/P6 worktree 均从同一 master 基线拉出，P5 未开工）。
3. 本批六任务同基线；P4 不依赖 P3 新接口（analysisunitcontext 主干接口
   足够）。共享文件按 ownership 表登记于
   `<git-common-dir>/paleo-parallel-20261119/p4-ownership.md`。
4. 环境：Arch/CachyOS 宿主，deb 闭包 prefix 共享
   `/home/kevin/projects/paleo-deb-prefix-20261119`（P3 准备，收据校验
   通过）；`vendor/onnxruntime`（1.30.0）、`vendor/ranuts-document`、
   `vendor/cache` symlink 到主仓。configure 需显式
   `-DCMAKE_LIBRARY_ARCHITECTURE=x86_64-linux-gnu`（Arch 无 multiarch
   三元组，P2 先例）+ `QGIS_PREFIX=<prefix>/usr`；构建/测试经
   `./paleo-dev`（注入 LD_LIBRARY_PATH/QT_PLUGIN_PATH 等，否则 rcc 链到
   系统 Qt 报 GNU_PROPERTY_1_NEEDED_INDIRECT_ACCESS）。
5. 工作包登记（当前调用链 / 缺口 / 相关 PR / 本次实现 / 验收证据——
   证据在「八、验证」更新）：

| WP | 当前调用链（master@08ca3e52） | 已复现/待验证缺口 | 相关 PR/issue | 本次实现或复用 |
|----|------------------------------|------------------|--------------|---------------|
| W1 探测 | `loadModelMeta` 直接构造 `Ort::SessionOptions`/`Ort::Session`（onnxpredictionservice.cpp:568-573）；`runtimeAvailable()` 只查文件存在（:948-964） | TODOS「P2 — ONNX 动态库 API 不匹配诊断」；Windows System32 旧 ORT `GetApi(30)=nullptr` 后访问异常不可 catch（静态记录，本机未复现——Linux 进程内 C API 恒可解析） | #1007 邻近但非此项；TODOS P2 章 | 新增 `src/ai/onnxruntimeprobe.{h,cpp}`：C API 探测先于任何 Ort 包装对象；`loadModelMeta` 前置门控；`runtimeAvailable()` 与详细诊断一致 |
| W2 单元准入 | `preparePredictionJob` unitSelection 可省略，resultKey 回退 `analysisLayerKey` 参数或 horizon（predictionworkflow.cpp:279-287） | 无单元 ONNX 张量调用经同一正式发布入口贴工区网格出「某层预测」；`analysisLayerKey` 参数可绕过 | #972（预测入口部分）、#1008 | prepare 强制 freezeAnalysisUnit（两引擎一致）；结果键从冻结单元产生；拒绝无单元/Boundary/伪造 S/aggregate；低层张量 API 原合同不动 |
| W3 精确父引用 | parents = 栅格声明源 + seismicPath；为空时按实体名、`displayName.contains(horizon)` 回退搜集（:371-398） | T7 匹配 T70/T71 等误血缘；scoped 计算未消费的 horizon 栅格仍列为父 | #1008 | 删两段模糊回退；scoped 只登记 seismic 源版本；模型按注册表钉 SHA 冻结身份（不伪造 Catalog 父） |
| W4 冻结贯通 | compute `loadModelMeta` 取 meta.sha256；无期望模型校验；输出仅查「文件写出」 | prepare→compute 模型可被换；输出无回读验证 | 审查 E 系列 | prepare 冻结注册表钉 SHA；compute 校验实际 SHA（`runPinnedTensor`）；输出 GDAL 回读验证尺寸/CRS/geotransform/覆盖 |
| W5 发布顺序 | publish 先 `declare`（:750）后 `publishArtifacts`（:763）；提交失败删 stagedPath 不恢复旧声明 | #1007：稳定 layerId 指向已删文件；旧声明可被失败声明覆盖 | #1007 | 改为先提交（Catalog）后声明（展示）；声明失败保留已提交产物并报告「已提交、展示未完成」；并发覆盖按 prepare 冻结的声明快照拒绝 |
| W6 回执/幂等 | publish 仅 bool；无提交事实查询；重复回调会撞 duplicate version id | 重复回调/崩溃后重试行为未定义 | — | 新增 `src/workflow/predictionpublication.{h,cpp}`：commit-fact 查询（versionId+sha 可证明）、已提交→只补展示；查询不确定→阻断 |
| W7 取消/状态 | 取消检查在 compute；publish 无取消检查；工程切换 discard 不看提交事实（:714-717）；attach_predict 取消清理不看提交状态；错误文案无运行库分类 | 取消/切工程可误删已提交版本；诊断不可区分 | — | publish 提交前后取消检查；discard 前查提交事实；调用方清理按 `committedVersionId` 守卫；预测页用详细诊断文案 |
| W8 回归 | tst_onnx/tst_onnxsessions（服务级，保留原合同）；tst_onnxworkflow（无单元发布——需改造）；tst_workflows 预测段 | 无 API 探测注入、无发布故障矩阵 | — | 新增 tst_p4_onnx_prediction（探测注入/单元准入/血缘/发布故障矩阵/幂等/清理审计）+ cmake/extra-p4-onnx-prediction.cmake |

6. 复用不再实现：模型池/同字节指纹会话（#144/#402/#479 家族）、
   `readPinnedBytes`、注册表门控（#145/#737）、`freezeAnalysisUnit`、
   `prepareSeismicUnitInput`、DerivedAssetRegistrar stage/commit、
   evidenceRecord、PaleoTaskService 异步三段式、受控依赖供货（#1042-#1048）。
   「正确 DLL 已部署」（#1050）≠「API 不匹配可诊断」，本任务不重做部署。

## 二、实现设计（要点）

- **W1**：`OnnxRuntimeProbe::probe()` 只调 C API
  `OrtGetApiBase()->GetApi(ORT_API_VERSION)`，不构造任何 `Ort::` 包装；
  区分 NotBuilt（无 ORT 编译，编译期）/ApiUnavailable（GetApiBase 空）/
  ApiVersionMismatch（GetApi 返回空，报所需 API、`GetVersionString()` 实际
  版本或「无法读取」、`/proc/self/maps`（Windows: GetModuleHandle）实测
  加载模块路径、受控前缀 `vendorRuntimeDir()`）。部署边界（文档化）：
  动态链接缺库时进程在 main 前被 OS 终止，guard 只覆盖进程内可探测状态。
  测试注入点 `setTestOverride` 确定性返回 nullptr/版本不匹配，负向测试不
  QSKIP。`loadModelMeta` 首行门控（先于 `Ort::SessionOptions`/`ortEnv()`）。
- **W2**：prepare（onnx 与 Processing 两分支）要求 unitSelection 并
  `freezeAnalysisUnit`（allowAggregate=false）；resultKey =
  `analysisLayerKey(frozen)`，与传入 `analysisLayerKey` 不一致→拒绝；
  Boundary/伪造 S 由 freeze 的方案校验拒绝。无单元 ONNX 张量调用从正式
  发布入口消失（低层 `runTensorOn` 原合同不变）；历史无单元成果只读兼容。
- **W3**：删除实体名/`displayName.contains` 回退；scoped 只取
  `parentVersionIdsFor({seismicPath})`；非 scoped（显式 INPUT）只取输入
  图层路径版本。模型身份：`registryEntryFor(name)` 轻查询（锁内拷贝，无
  文件 IO）冻结 `sha256Pinned` 为 expectedModelSha256；无 manifest 开发
  降级不冻结期望（实际 SHA 仍进 provenance），不伪造 Catalog 父。
- **W4**：compute 用 `runPinnedTensor`（期望 SHA 非空时）+ loadModelMeta
  实测 SHA 双保险；输出写盘后 GDAL 回读（尺寸==rows/cols、geotransform、
  CRS 非空、有限值/覆盖统计）并写入 commitExtra。
- **W5/W6/W7**：publish 重排为「commit-fact 查询 → （未提交）拷贝+提交 →
  声明（冲突检查：现声明 vs prepare 冻结快照）→ 声明路径==提交版本路径
  复核 → predictionDone」。声明失败/取消后：不删已提交文件、不重提交；
  报「成果已提交（版本 X）、展示未完成」。重复回调经 commit-fact 幂等。
  取消检查在拷贝后/提交前与声明前（进入 publish 前的取消由 compute 入口
  与调用方状态机拦截）；工程切换 discard 前先查提交事实。
  `publishPredictionJob` 签名改非 const（记录 committedVersionId 供调用方
  清理守卫；调用方：runPrediction、attach_predict、tests）。

## 三、文件接缝（实现后回填核对）

主写：`src/ai/onnxpredictionservice.{h,cpp}`、
`src/workflow/predictionworkflow.cpp`、`workflows.h`（仅 Prediction 区域）、
新增 `src/ai/onnxruntimeprobe.{h,cpp}`、`src/workflow/predictionpublication.{h,cpp}`、
`tests/tst_p4_onnx_prediction.cpp`、`cmake/extra-p4-onnx-prediction.cmake`。
窄缝：`src/ui/paleomainwindow_attach_predict.cpp`（取消/失败清理按
committedVersionId 守卫）、`src/ui/pages/predictpage.cpp`（运行库诊断文案，
2 处）、`CMakeLists.txt`（_paleo_extra_manifest 单条登记）、`TODOS.md`
（ONNX 诊断条目改记已交付）。
测试改造：`tst_onnxworkflow.cpp`（无单元发布用例改新合同拒绝断言，删除的
无单元正向覆盖迁移至 tst_p4/tst_onnxsessions）。tst_onnxsessions.cpp 与
tst_workflows.cpp 本任务未改动——前者本就覆盖张量 API 原合同，后者预测
用例因 Processing 引擎保留可选单元 legacy 合同而无需补单元（独立 review
核对后修正本节，最初草稿误列）。

## 八、验证（2026-11-20，构建/回归均在本 worktree）

环境：Arch/CachyOS（glibc 满足 deb 闭包），deb 闭包前缀
`/home/kevin/projects/paleo-deb-prefix-20261119`（QGIS_PREFIX=…/usr，
CMAKE_LIBRARY_ARCHITECTURE=x86_64-linux-gnu 显式注入），ORT=受控
vendor/onnxruntime 1.30.0（主仓 symlink），构建 `./paleo-dev build`
（-j4/-j2，ld 偶发 ENOSPC/段错误为宿主 btrfs 元数据压力，串行重链通过）。

构建：全量 2556 目标绿（cmake -G Ninja RelWithDebInfo）。

直接回归（offscreen + 受控 LD_LIBRARY_PATH，逐套运行）：

| 套件 | 结果 |
|---|---|
| tst_p4_onnx_prediction（新增，17 个用例函数；QTest 计数 19 含 init/cleanup） | 19 pass / 0 fail（无 QSKIP 负向） |
| tst_onnxworkflow（改造） | 8 pass / 0 fail |
| tst_workflows | 24 pass / 0 fail |
| tst_panels | 62 pass / 0 fail |
| tst_predict_assembly | 4 pass / 0 fail |
| tst_factorworkflow | 52 pass / 2 skip（既有 skip） |
| tst_onnx / tst_onnxsessions | 9 + 26 pass（张量 API 原合同不动） |
| tst_tileinference / tst_modelregistry / tst_remotepredictrouter / tst_aiassistworkflow / tst_predictionevidence | 24/32/20/11/9 pass |

Source gates（最终 head 逐项）：layering --strict ✓（无层违规）、
--qgs-audit ✓（134=baseline）、check_qgis_spread ✓（169=baseline）、
check_file_budget --strict ✓（51=baseline，新增 0——predictpage 一度 +1
已压缩回）、ui_size ✓、ui_invariants ✓、i18n ✓、ui_tokens ✓、
qgis_versions ✓（4.2.3 三路一致）、docs_drift ✓、cpp_mainline ✓、
layering --transitive（QGIS 头注入受控前缀）exit 0（黄牌清单在基线内）。

关键验收矩阵核对（tst_p4 逐项对应）：
- 所需 API 返回 nullptr/版本不匹配（注入）：probeInjectionApi*——首个
  Ort 包装对象前拒绝、公开码 + 所需版号 + 实际版本 + 实测模块齐备、
  进程不崩、负向不 QSKIP。
- 无 ORT 构建：noOrtBuildReportsExplicitlyUnavailable（#if 分支断言，
  编译于两种构建；本机为 ORT 构建，非 ORT 分支由编译期保证并留断言）。
- OS main 前加载失败：文档化部署边界（onnxruntimeprobe.h 头注 + 测试
  注释），不伪造 guard 捕获。
- 无单元/Boundary/伪造 S/aggregate/analysisLayerKey 绕过：
  noUnitFormalPredictionRejectedAtPrepare +
  analysisLayerKeyMismatchAndForgedUnitRejected。
- 同层段 S1/S2 不碰撞：unitOnnxPredictionEndToEndWithPreciseLineage 尾段。
- T7/T70/T71 重名干扰不入 parents：同上（decoy 资产断言）。
- 模型 prepare 后被换：modelSwapAfterPrepareRejectedAtCompute
  （model_revision_changed + 冻结 SHA）。
- 形状不合法拒绝：scopedModelShapeMismatchRefused（+compute 输出侧
  shape 校验保留）。
- 首次 Catalog 失败：catalogCommitFailureLeavesNoDanglingDeclaration。
- v2 提交失败不覆盖 v1：同上（v1 声明/文件/版本可打开）。
- v2 提交成功声明失败：declareFailureAfterCommitKeepsResultAndRetryRestoresDisplay
  （重试只补展示，版本数不增）。
- 重复回调/重放：duplicateCallbackIsIdempotent。
- 并发展示不覆盖：concurrentDisplayWinnerNotOverwritten。
- 取消（提交前/后）、切工程（提交前/后）：cancel*/projectSwitch* 四象限，
  已提交文件在守卫清理后仍存在。
- 原件不变：全部用合成夹具；无 gitlink/依赖版本改动（git diff 核对）。

剩余限制（如实）：
- OS 在 main 前终止进程的加载失败不可被进程内 guard 覆盖（部署边界，
  交由 paleo-dev/selfcheck/便携包校验在进程外发现）。
- 无 manifest 开发降级时无注册表钉 SHA 可冻结（expectedModelSha256 空，
  实测 SHA 仍进 provenance）——生产路径（有 manifest）不受影响。
- DerivedAssetRegistrar 的 stage 期版本号预分配在「两个同名资产任务都
  prepare 且都未 commit」时可能撞 versionNumber（commit 拒绝 duplicate）；
  该行为是共享底座现状，本任务不扩权修改（登记于上表 W3，生产三段式
  单任务串行不触发）。
- `paleo-dev test`（ctest）在 deb 前缀 LD_LIBRARY_PATH 下宿主 ctest 自身
  符号冲突（XML_GetCurrentColumnNumber64），本机回归按套件直跑二进制；
  CI 车道不受影响。
- Processing 引擎无单元调用保留 legacy 合同（生产 UI 恒带单元；#972 的
  因子/约束链执法属 P3 范围）。
- 并发胜者拒绝覆盖时 publish 返回成功并按稳定 layerId 发 predictionDone
  （展示目标当时确有有效图层，指向较新版本）；任务详情不另行提示「本次
  成果已提交未上屏」，需要时由 UI 后续按 catalog 记录补充呈现。
- 既有（非本任务引入）：Processing 分支 prepare 在 stage 之后仍可能失败
  （如 P0-1 输入解析失败），留下空 staging 目录——旧行为保持，未纳入本
  任务清理面。

## 九、独立复审（review → fix → 复审记录）

独立 reviewer（未参与实现）通读 `git diff 08ca3e52..HEAD` 并独立重跑
gates 与三个关键测试二进制后给出结论：无阻断项；清理安全（每个删除点
逐址核对，含提交后错误分支无 unset 的 committedVersionId）、探测先于
包装对象、准入无绕过、父引用无模糊回退、发布顺序与冲突边界、签名变更
调用方、层门全部确认。要求修复项与处置：

| 发现 | 级别 | 处置 |
|---|---|---|
| ledger 误列 tst_onnxsessions/tst_workflows 改动 | MAJOR | 已修（三节改为与实际写集一致） |
| 空 unitSelection map 绕过 prepare 拒绝文案 | MINOR | 已修（ONNX 门改按冻结结果非空判定） |
| availableModels/registryEntryFor 双锁交错窗口可静默退化为不钉身份 | MINOR | 已修（先取条目单锁冻结，可用性仅作拒绝门） |
| 提交成功后 GUI 线程二次哈希 | MINOR | 已修（publishArtifacts 成功即提交证明，只取版本记录，sha 复核留重放路径） |
| compute 非 scoped 分支不可达无说明 | MINOR | 已补注释（手工 job 兜底，无生产入口） |
| TODOS 公开码措辞 | MINOR | 已修（公开码=runtime_incompatible，api_* 为消息前缀） |
| 「19 项」计数含 init/cleanup | NIT | 已修（17 个用例函数） |
| 并发胜者成功无区分提示 | NIT | 维持现状 + 剩余限制登记 |
| QDirIterator 未用 include / 回读注释措辞 / 文档「拷贝前」措辞 | NIT | 已修 |
