# P4 原生 ONNX 预测准入、血缘与发布闭环（2026-11-19/20）

任务：P4（内部代号 S2，方向 B：地质分析身份与成果发布）。
分支：`codex/paleo-p4-onnx-prediction-20261119`（worktree
`../paleo-p4-onnx-prediction-20261119`）。基线：origin/master
`08ca3e52b829f928d38e34d3f7fa3e58376f6076`（2026-11-20 核对，当时与
prompt 证据锚点一致）。

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
  取消检查在拷贝前/提交前/声明前；工程切换 discard 前先查提交事实。
  `publishPredictionJob` 签名改非 const（记录 committedVersionId 供调用方
  清理守卫；调用方：runPrediction、attach_predict、tests）。

## 三、文件接缝（实现后回填核对）

主写：`src/ai/onnxpredictionservice.{h,cpp}`、
`src/workflow/predictionworkflow.cpp`、`workflows.h`（仅 Prediction 区域）、
新增 `src/ai/onnxruntimeprobe.{h,cpp}`、`src/workflow/predictionpublication.{h,cpp}`、
`tests/tst_p4_onnx_prediction.cpp`、`cmake/extra-p4-onnx-prediction.cmake`。
窄缝：`src/ui/paleomainwindow_attach_predict.cpp`（取消/失败清理按
committedVersionId 守卫）、`src/ui/pages/predictpage.cpp`（运行库诊断文案，
2 处）、`CMakeLists.txt`（_paleo_extra_manifest 单条登记）。
测试改造：`tst_onnxworkflow.cpp`（无单元用例改单元夹具/拒绝断言）、
`tst_onnxsessions.cpp`（探测注入负向）、`tst_workflows.cpp`（受影响预测
用例补单元）。

## 八、验证（最终 head 后回填）

- 待实现后填写：构建命令、ctest 项、Source gates、结果与剩余限制。
