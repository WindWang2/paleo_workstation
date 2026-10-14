# Ledger：地震显示、井震联动与现有 AI 预测运行链稳定化（方向 03）

- 分支：`goal7/seismic-ai-prediction-20261014`
- 工作树：`../paleo-wt-goal7-seismic-ai`
- 基线：origin/master `b0520429c21f858a1c053b876f511680088564a9`（2026-10-14 远端核对 identical）
- 写权限白名单：src/ui/seismicsection/、src/ui/seismic3d/（仅轻量路径）、src/ui/ai/、
  src/ai/、src/domain/seismic/、src/services/seismictaskservice*、
  src/algorithms/seismicattr.*、src/algorithms/horizontrack.*、
  src/linkage/seismicmaplink.*、src/workflow/aiassistworkflow.*、src/workflow/aichat*、
  本方向专用测试。

## 排除指纹（执行时动态核对，先排除后择题）

- Open PR：仅 #437（68 文件写禁区，`gh pr view 437 --json files` 实时为准）。
  与本方向交集：mappingworkbench_predict.cpp / wellpredictionreview.cpp /
  predictionevidence / selectioncontext / wellmaplink —— 全部只读。
- Open Issues 70 项（#352–#421）全部排除；与本案直接相关的指纹：
  #365（切片点击 sample0）、#366（LOD 不失效）、#367（增益曲线缓存键）、
  #370（最近井有符号比较）、#371（反演任务守卫）、#372（空轴越界）、
  #391（切片缓存键 mtime）、#402（ONNX 销毁竞态）、#405（aichat 取消窗）、
  #409/#410（formationPrediction 测试缺口）、#412（paint.cpp amplitude 越界 +
  seismic3dtf 单stop）、#417（semblanceCoherenceWeighted 维度校验序）、
  #421（sectionui sleep 杠杆 + 3dui || true）。
- #435/#436 已交付不重做：合成记录逐井时深、AC/DEN 单位防御、
  baseCurveMnemonic、faciesCodeForLabel 确定性扩展、tile provenance、
  取消钩子、相干逐切片几何校验、预测两表事务化。

## 环境接线（本机 CachyOS，Arch 系特有）

vendor/prefix、vendor/onnxruntime → symlink 到
`../pw-seismic-runtime-closure-20260930-215353/vendor/`（只读共享）。
**configure 命令**（Arch 无 dpkg multiarch 自动搜索 + master #344 起新需
Qt6Core5Compat 而闭包未含，需显式接线；5compat 从 Ubuntu pool 拉
6.10.2-1 匹配 deb 解压到 `../paleo-wt-goal7-deps/qt6-5compat/extracted`，
不进仓库、不写共享 prefix）：

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DQGIS_PREFIX=$PWD/vendor/prefix/usr \
  -DQt6_DIR=$PWD/vendor/prefix/usr/lib/x86_64-linux-gnu/cmake/Qt6 \
  -DQt6Core5Compat_DIR=../paleo-wt-goal7-deps/qt6-5compat/extracted/usr/lib/x86_64-linux-gnu/cmake/Qt6Core5Compat \
  -DCMAKE_PREFIX_PATH="$PWD/vendor/prefix/usr/lib/x86_64-linux-gnu/cmake;../paleo-wt-goal7-deps/qt6-5compat/extracted/usr/lib/x86_64-linux-gnu/cmake" \
  -DCMAKE_LIBRARY_PATH="$PWD/vendor/prefix/usr/lib/x86_64-linux-gnu;../paleo-wt-goal7-deps/qt6-5compat/extracted/usr/lib/x86_64-linux-gnu" \
  -DCMAKE_INCLUDE_PATH="$PWD/vendor/prefix/usr/include;../paleo-wt-goal7-deps/qt6-5compat/extracted/usr/include/x86_64-linux-gnu/qt6"
```

测试运行需 LD_LIBRARY_PATH 追加 5compat lib 目录（见各轮记录）。

## R0 基线结论（S1–S4 四路并行只读审计 + 主 Agent 亲核）

详见 `baseline.md`。核实候选（按价值排序）：

1. **[A/B 旗舰，verified]垂直剖面显示垂直镜像**：vendor 契约行 0=最深样
   （SgyVolume.cpp:456-462，tst_seismic_core.cpp:284-288 钉死）；canvas 整套
   时间映射（timeToPixelY/nav.cpp:169、hover TWT/interact.cpp:201-206、
   imgDest 顶=paint.cpp:44-47、拾取 addPickFromCanvas、井分层/合成记录按物理
   TWT 绘制）假定行 y=采样 y 升序；dock 不翻行直喂
   （dockwidget.cpp:373/574）。后果：标尺↔数据镜像、hover TWT 是幅值镜像
   时间、追踪种子落在镜像样点、井分层只在画面中点对齐。追踪内核侧同类
   混淆已由 D4.2 修复（interp.cpp:77-93 注释自证「采样升序空间」才是全链
   预期），显示侧从未修。修复方向：canvas 垂直图入口（setSectionData/
   setCompareData/setAttrOverlay/setZoneOverlay）统一翻行为采样升序，
   契约注释钉死；测试 fixture 改为 vendor 行序生成。
2. **[D/F，verified] tile 推理成功但产品落盘失败时工具谎报成功**
   （aiassistworkflow.cpp:542-557 + aichattoolrunner.cpp:225-235）。
3. **[C，verified] LLM 错误路径不合成未应答 tool 帧**
   （aichatcontroller.cpp:58-68，cancel 路径 :514-535 有完整先例）。
4. **[E，verified] startAttributeSlice 相干分支不兑现面板参数**
   （_slice.cpp:477-497/599-604：固定 ±1 线、无权变体、IL/XL 半窗映射颠倒、
   SATR provenance 失真、沿向窗≥剖面宽静默全 NaN 成功）。
5. **[E，verified] readTraceHeader 起始偏移硬编码 3600**
   （_transcode.cpp:896-898，忽略扩展文本头 trace0）。
6. **[A，verified] 卷帘 B 图绕过 displayValues 链且参数变化不刷新**
   （nav.cpp:26-38 + params.cpp）；**书签视口恢复被异步 fitToWindow 覆盖**
   （bookmark.cpp:53-62）；**拾取面板"定位"no-op**（setup.cpp:721-730）；
   **井轨迹标题后缀无限累积**（dock_wells.cpp:202-203）。
7. **[C/E，likely] trace scorer 输出无 [0,1] 域校验**（horizonsuggest.cpp:173-196）。
8. **[G] mock/fixture 缺口**：loopback 无状态码注入（认证失败不可测）、
   OnnxLocalPredictor 错误分支零直测、合法 ONNX 前缀截断夹具缺失。

登记不做（他人指纹/越界）：Seismic3DColorMap::buildLut 单 stop（#412 同型
残留——留给 Issue Agent，记入「留给 Issue Agent」表）；seismichorizoncluster
TimeRaster 零测试 + clusterSeismicHorizon 吞 readInline 失败（S1-G3，文件不在
白名单）；远端降级主线程同步（S3-G3，生产不可达，登记递延）。

## 逐轮记录

### 轮 1（A 旗舰）：垂直剖面行序镜像修复——先红后绿

- **红证据**（未修复代码、新用例 sectionRowOrderMatchesPhysicalTime）：
  `displayValueAt(2,5)` 实际 316.5（= 样 58 真值，58 = 63-5 镜像位），
  期望 303.25（样 5 真值）。镜像机制实锤。
- **修复**（视图边界统一翻转，升序=canvas 设计契约）：
  - `seismicsectioncanvas_internal.h`：新增 `ToAscendingSampleRows()`
    （values+rgba 同步翻转）+ 契约注释（vendor 行 0=最深 ↔ canvas 升序）。
  - `seismicsectioncanvas.cpp` setSectionData：入图翻转。
  - `seismicsectioncanvas_render.cpp` bakeOverlay：attr/zone 烘焙翻转（单点）。
  - `seismicsectioncanvas_nav.cpp` setCompareData：卷帘 B 图烘焙翻转
    （m_compareSlice 存原图防 setCompareEnabled 重喂时二次翻转）。
  - `seismicsectiondock_wells.cpp` WellSideTraceWidget：按采样升序取道。
  - 生产喂数点（dockwidget 373/574、attr 59、attach_models 575）零改动。
- **fixture 对齐**（升序→vendor 行序）：tst_seismic_sectionui makeSection、
  tst_seismicattrui makeSection/makeAttr（图案按 s 写入行 N-1-s，断言语义
  不变）；均匀值 fixture（faultsectionui/propmodelpanel/welltie/section）
  翻转不变，零改动。
- **新用例**：sectionRowOrderMatchesPhysicalTime（真体→dock→canvas：样值
  公式对拍 + hover TWT/幅值自洽）。#421 指纹行未触碰。

### 轮 2（D/F）：tile 推理成功但产品落盘失败 → 任务如实 Failed

- aiassistworkflow.cpp finished 槽：writeTileProducts 失败（含 workflow 已亡）
  → `task->failPublication(err)`（Succeeded→Failed 翻转，PaleoTask 既有 API）
  + tileClassificationFailed。任务页/toolrunner（按 state() 分派）/dock 卡片
  三消费面同步看到失败。
- 新用例 asyncClassificationPublishFailureFailsTask（tst_aiassistworkflow）：
  catalog 解绑 → 推理 Succeeded 但任务终态必须 Failed + errorText 非空 +
  失败信号。修前任务停 Succeeded、工具回灌 ok=true+「已登记 DERIVED」。

### 轮 3（C）：LLM 错误路径协议完整性

- aichatcontroller.cpp errorOccurred：对齐 cancel() 口径——按「尾部 assistant
  的 toolCalls 是否已有 role=tool 应答」补齐错误应答帧（m_unansweredCalls
  只在成功收尾填充，中段出错时为空但历史挂着未应答帧）+ 摘空占位。
- 新用例 errorAfterToolFramesKeepsProtocolIntegrity（tst_aichatcontroller）：
  FakeLlmServer 新 ToolCallsHang 模式（工具帧送达后挂起→空闲超时）→ 断言
  每个 callId 都有应答。修前红（裸 clear）。

### 轮 4（E 部分）：readTraceHeader 首道偏移 trace0

- seismictaskservice_transcode.cpp：硬编码 3600 → 复用 vendored segyio
  `segy_trace0()`（与 vendor 读取通道同源；rev2 3521 首道偏移/rev0-1 3505
  扩展头数）；异常字段回落 3600。
- writeTestSegy 增 extTextHeaders 参数（bin 头 3505 字段）；新用例
  traceHeaderServiceExtendedTextHeaders（N=2：偏移 3600+2·3200+…）。
  常规文件既有断言（3600+…）不变。

### 轮 5（E 旗舰）：剖面相干兑现面板参数

- seismictaskservice_slice.cpp startAttributeSlice 相干分支：
  - 邻线窗宽 normalHalf（IL 剖→coherenceIlHalf / XL 剖→coherenceXlHalf）
    替代硬编码 ±1；边缘守卫按窗宽拒绝（「无双侧 ±N 邻线」）。
  - 内核换 semblanceCoherenceWeighted（等权档与旧核逐位一致的既证路径），
    道距 = mapper 拟合轴向量 × 轴平均步长（与属性体扫描 _attr.cpp 同式），
    mini 体两轴按 (法向, 剖面列向) 映射；加权档要求 mapper 可用，否则如实
    拒绝（内核 InverseDistance+非正道距=全 NaN，不静默降级）。
  - 沿向窗超出剖面宽（2·alongHalf > width-1）→ 如实拒绝（旧实现静默
    产出全 NaN 的成功，SATR 失真）。
  - SATR provenance（header 字段写 params）自此与实算一致。
- 新用例（tst_seismicattrsvc）：coherenceHonorsNormalHalfWindow（±2 越轴端
  拒绝 [旧实现成功=红]；线 12 成功同波形 ≈1；加权档冒烟）、
  coherenceRejectsOversizedAlongWindow（沿窗 5 > 宽 9 拒绝 [旧=全NaN成功=红]）。

### 轮 6（A/B 交互）：书签视口/卷帘 B 配方/定位 no-op/标题累积

- 书签（bookmark.cpp + dockwidget.cpp）：跨线/跨模式书签触发异步重提取，
  到达回调 fitToWindow 复位视口——新增 m_pendingViewReplay 暂存 + 提取
  完成后重放（一次即清）；同线书签保持直接应用。新用例
  crossLineBookmarkRestoresViewport（dock 级：换线→fit 复位→applyBookmark
  →重提取完成→zoomX/panY 恢复书签值；旧实现停在 fit=红）。
- 卷帘 B 图（params.cpp + nav.cpp）：setGain/setContrast/setPolarityInverted
  重烘 m_compareImage（B 与 A 同配方）；B 烘焙补极性翻转（AGC/增益曲线
  B 侧不消费——既有口径注释写明）。新用例 curtainCompareFollowsDisplayParams。
- 拾取面板「定位」（setup.cpp）：旧实现 viewState 读出又写回（no-op）——
  实装纵向滚动（panY 使拾取 TWT 落画布中部），换线路径走同款重放通道。
- 井轨迹标题后缀（dock_wells.cpp）：computeWellTrajectories 每次任意线
  提取都追加「时深用默认均速…」→ 幂等守卫。

### 轮 7（C/E 补强）：trace scorer 域守卫 + OnnxLocalPredictor 直测

- horizonsuggest.cpp：scorer 输出 p∉[0,1±1e-3] → 整跑拒绝（「疑似
  logits/未归一输出——拒绝建议」）；修前静默钳位 → score>1、置信恒 1
  （bernoulliConfidence clamp），解释员被全置信建议误导。
- onnxfixture 增 writeTraceLogitScorer（Mul+Add 无 Sigmoid——域守卫钉子）。
- OnnxLocalPredictor 四分支直测（tst_onnxsessions 新增）：缺 samples 红线
  （「不造假」）、非网格输入模型拒绝、256 类越界拒绝、happy path argmax
  解析对拍（此前仅经 router 间接覆盖）。

### 轮 8（验证）：综合重建 + 全套受影响套件

13/13 套件通过（tst_seismic_section / sectionui / seismicattrscan /
seismicattrsvc / seismicattrui / welltie / interpret / propmodelpanel /
faultsectionui / aichatcontroller / aiassistworkflow / ai_horizonsuggest /
onnxsessions），含全部新用例。

### 轮 9（R7 前半）：独立 Review（S5）与修复闭环

S5 独立 reviewer（未参与实现）审查全部 5 提交。结论：无 P0；**2 P1**
（均已修）+ 5 P2（3 修 2 登记已知限制）。

- **P1-1 zone 叠加镜像回归（已修）**：bakeOverlay 无条件翻转会把属性建模
  的 zone 图（stratgrid IJK 投影，k=0=最浅层，本就升序）翻反——相带
  垂直镜像。修复：bakeOverlay 增 flipRows 参数（attr=true 走 vendor 序
  翻转 / zone=false 升序直通），契约注释钉死。新钉子
  zoneOverlayDirectionMatchesTimeAxis（上下半区不同值，顶部像素色阶
  必须对应浅部值）。
- **P1-2 定位跨模式丢线号（已修）**：else-if 使 modeChanged 分支不再
  setValue——切到 IL 后显示测网中线而非拾取线。修复：模式切换后仍判定
  线号并 setValue。
- **P2-1 pending 视口残留（已修）**：失败/空切片/setVolume 路径清
  m_hasPendingViewReplay。
- **P2-2 cancel() 流式期同型协议洞（已修）**：cancel 复用「尾部 assistant
  直查」补帧（与 errorOccurred 同口径）。新钉子
  cancelDuringStreamingAfterToolFramesKeepsProtocol。
- **P2-4 井旁道分层刻度分母（已修）**：与波形映射统一 (n-1) 口径。
- **P2-3 卷帘重烘无节流（登记）**：A 侧每 tick 全图烘焙是既有行为，B 侧
  翻倍；合并/延迟优化递延（避免引入定时器合并新复杂度）。
- **P2-5 标题幂等守卫不可达（登记）**：按当前调用序 setLineTitle 先重置、
  累积不可复现；守卫作为防回归保留，声明如实降级为「防御性」。

reviewer 同时确认：行序翻转入口清点齐全（含纹理缓存键两侧、瓦片流、
m_lastSlice 不双翻）；failPublication 连接顺序假设成立、无重入；错误路径
补帧无 UI 副作用；segy_trace0/相干兑现索引数学与边界逐条对拍无误；
24 文件指纹合规（#437 保护区与全部 open issue 指纹零触碰）；无新增
资源/线程问题。

### 验证状态（最终）

- `cmake --build build -j4`：全量 133 步重建成功（清空间后；见环境节）。
- **全量 `ctest -LE perf -j4`：394 项全绿**。初次跑 5 失败逐一归因复测：
  - tst_catalog：`-j4` 负载 flake，串行复测通过（#435 ledger 同款）。
  - tst_singlefactor_faults / tst_singlefactor_asynccontour /
    tst_factorworkflow / tst_algorithm_harness：**本机环境性失败**——
    vendored PROJ 数据路径未进默认 ctest 环境（`proj.db 找不到`→CRS
    解析失败）。补 `PROJ_DATA/GDAL_DATA` 重跑 4/4 全绿。这 4 项的源码
    均不在本分支 diff 内（非回归；主 checkout 同环境同样红）。
- 触及套件（13 个）在默认环境全绿，连续多轮。
- `python3 tools/check_layering.py --strict` ✓；`git diff --check` ✓。
- 反向验证：`git diff origin/master...HEAD --name-only` 26 文件全部在
  白名单内，与 PR #437 的 68 文件写禁区零交集，未触碰任何 open issue
  指纹（S5 reviewer 独立复核确认）。

## 环境事故记录（占本轮墙钟大头，供续跑参考）

- **磁盘满**：`/home/kevin/project` 独立 nvme 分区（458G）被 7 个并行方向
  worktree 的 build 挤到 99M 剩余——ld 间歇 ENOSPC、QTemporaryDir 失败。
  处置：删除 4 个已合并（branch --merged origin/master 验证）legacy
  pw-* worktree 的 gitignored build 目录（释放 55G）。活跃 goal7-*
  worktree 未动。
- /tmp（32G tmpfs）为全部 worktree 链接共享，高并发时瞬时挤满。

## 留给 Issue Agent（发现但不属本方向所有权，本 PR 不动）

| 发现 | 位置 | 说明 |
|---|---|---|
| Seismic3DColorMap::buildLut 单停靠点 stops_[k+1] 越界 | seismic3dcolormap.cpp:100-105 | #412 TF 半的同型未修实例（非复制品）；需补单 stop 分支 |
| clusterSeismicHorizon 静默吞 readInline 失败 | seismichorizoncluster.cpp:206-209 | 错误归因失真（「有效交集不足」）；文件不在白名单 |
| seismichorizoncluster TimeRaster 路径零测试 | seismichorizoncluster.cpp:34-118 | 生产入口无覆盖；文件不在白名单 |
| 远端降级主线程同步执行（潜伏） | remotepredictrouter.cpp:337-344 | 生产装配无人填 samples 故不可达；填了则 GUI 冻结（哈希+推理） |
| LoopbackServer 无状态码注入 | tests/tst_remotepredictrouter.cpp | 认证失败/HTTP 500 JSON 错误体场景不可测 |
| tst_seismic_sectionui 既有 sleep 杠杆 | #421 指纹行 | 他人指纹不动 |

### 环境（构建阻塞排障，详见上「环境接线」）

- Arch 机三大坑逐一排掉：Qt6Core5Compat 缺失（Ubuntu pool 拉 6.10.2-1
  匹配 deb）、系统 qt6keychain 0.17（Qt_6.11 tag）与 vendored Qt 6.10.2
  混链（拉 0.15.0-2 dev+runtime、Qt6Keychain_DIR 钉版本）、Qt 工具/测试
  运行需 QT_PLUGIN_PATH 指向 vendored 插件目录。
- 跨 worktree 全局锁实测有效：本轮排队等待兄弟方向（singlefactor/
  well-sections）构建释放。

## 发布前指纹复核（2026-10-15 push 时点）

- master 仍 `b0520429`（identical）。
- Open PR 1→5：新增 #445（qgis-layers 方向06 壳）/ #446（well-sections 方向02）/
  #447（codex/fix-core-issues）/ #448（qgis-layers 方向05）——逐一 `gh pr view
  --json files` 核对，与本分支 26 文件**零交集**。
- Open Issues 70→77：新增 #438–#444（保存版本/证据审计/图谱查询域）——
  与本方向无文件/机制交集，全部入排除表（本 PR 不触碰）。
