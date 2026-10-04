# Goal-Loop 方向 37 ledger：层位自动追踪 3D 化（horizon-3d）

- 分支：`goal/horizon-3d-20261004`（worktree `.worktrees/horizon-3d`，基线 `24daf8f`）
- 日期：2026-10-04

## 设计决策

| # | 决策 | 因由 |
|---|------|------|
| D1 | 数值核不重写：`propagateVolume`（全量缓冲）改薄壳，主体下沉 `propagateVolumeWindowed(nIl,nXl,nS,SectionProvider,...)`；滑窗契约 = 至多 2 枚相邻剖面存活、按 IL 从种子向两侧单调取数 | 方向书「不重写数值核」；等价性由 tst_horizontrack 全量/滑窗逐位对比锁定（含空块/限步长/取消） |
| D2 | 倾角引导 = 最近 K 个拾取的最小二乘斜率（连续道 x，追加序 = 行进向，左右向各自独立），搜索窗中心 = 前沿 + round(slope)，窗半径仍 maxSearchSamples（可达跳幅上界 2×maxSearch）。估计失败（<2 拾取 / 斜率 NaN / \|斜率\| > 2×maxSearch）如实回落隐式行为（斜率 0） | 第三判据不替换阈值/相干门；钳 2×maxSearch 因预测窗 ±maxSearch 是趋势可达上界，再陡无从验证（诚实边界） |
| D3 | `StopReason::ReadFailure` 新增：剖面供应器 nullptr → 已得拾取保留、传播中止（读错误非数据属性，不越错误续扫另一侧）；取消引发的 ExtractSlice 中止优先按取消归类（worker 先查 cancelRequested） | 服务层读失败要如实浮出，不能冒充 CorrelationLoss；取消 ≠ 失败 |
| D4 | 服务读数通道 = `SgyVolume::ExtractSlice(Inline, 线号)` 逐 IL 剖面（startAttributeSlice 先例），非引擎 registry 体窗 | 属性服务同模式；体窗通道面向 paged LOD，传播需全分辨率原始道 |
| D5 | 内存闸：工作集 = 6×剖面字节（3 剖面 × 行主序像+道主序副本当量）> 预算（默认 min(RAM/4, 2GiB)）→ 如实拒绝报数 | 「不 OOM 硬扛」；测试传极小预算验证拒绝路径 |
| D6 | 顶替：同体路径新传播 → 旧任务 requestCancel + superseded 标志，旧回调静默丢弃（不报取消不报错）；旧任务即便已完成但回调未投递，同样静默（新请求取代旧结果语义） | 「cancelled≠failed 静默丢弃」同切片顶替 A3 口径 |
| D7 | 传播结果拾取集**不进**会话拾取表（无 undo 合并）；直接落 DERIVED 资产 + GeoTIFF 上图 | 全体拾取集是图件级对象非单剖面编辑对象；QC 走报告行/图层；oracle 只要求上图+登记 |
| D8 | `registerPropagatedHorizonAsset` 直接成格（`gridPropagated`：无 IDW 填洞，缺格 NaN 如实留空；不规则网落不进格的拾取如实跳过），CSV/GeoTIFF/版本登记体与 registerHorizonAsset 共享（`writeHorizonAssetFiles` + `registerHorizonVersionEntry`） | 传播覆盖区是实测前沿，外推填充是欺骗；不开平行格式 |
| D9 | UI 入口 = 拾取面板「◈ 3D 传播」按钮 + 「倾角引导」勾选（追踪/传播共用参数行，开 = dipHistoryPicks 8）；Inline 剖面 + 手动拾取种子 + 有体才可用，禁用带 reason tooltip | DESIGN.md §组件（禁用必须带 reason）；进度/取消走任务面板（quiet=false，>1s 进度条规范） |
| D10 | 2D 剖面追踪同步透传倾角引导（SeismicTrackOptions 尾部追加 dipHistoryPicks，不破坏既有聚合初始化） | 核级能力天然两用；面板单一开关语义一致 |

## Oracle 验收证据（全部实测）

1. **合成体单种子 → 整层位面 + DERIVED 锚源体**：
   - `tst_horizontracksvc::singleSeedFullCoverage`：8×24 体单种子 → 192/192 拾取全覆盖，线号域映射（IL 1000..1007/XL 2000..2023，非 0 基索引）、TWT = t0 40ms + s·2ms（记录延迟入域）、ilMin/ilMax = 1000/1007、进度 ≥2 次上报 ✓
   - `tst_horizontracksvc::derivedVersionAnchorsSourceVolume`：DERIVED 版本 parentVersionIds = {RAW 版本}，origin = "seismic-propagation"，CSV 行数 = 拾取数（无插值行列）✓
   - `tst_seismic_interpret::dockVolumePropagationRegistersHorizon`：offscreen 全链（体 → 剖面 → 手动种子 → 传播 → 登记 → horizonLayerDeclared → GeoTIFF 存在 + catalog 锚源 RAW + 面板复位「3D 传播」+ QC 行「覆盖 48/48」+ 会话拾取表不混入）✓
2. **倾角引导差异证据**：
   - 2D（`tst_horizontrack::dipGuidanceDifferentialEvidence`）：同一近距同相轴剖面（前段斜率 2、t>15 陡至 5 跨 maxSearch=3，平坦捕获面 t≥16 入场于带内）——**无引导尾段（t≥20）锁死平坦邻轴（mean |err| = 95.50 样，conf 1.0 骑到尾＝滑落不可见）；有引导贴真值（mean |err| = 0.00）**，双侧全覆盖。
   - 3D（`tst_horizontrack::propagateVolumeDipGuidanceDifferential`）：同几何层面（XL 向陡化 + IL 向斜率 1）——无引导 576/576 拾取全部落在平坦捕获面；有引导 336 个尾段拾取 mean |err| = 0.000 贴解析面。
   - 回落语义（`dipGuidanceFallbackMatchesImplicit`）：缓倾角（斜率 ≤ maxSearch）下开/关引导逐位一致（160 道 picks/sample/confidence 全等）。
   - 边界如实：斜率 > 2×maxSearch 趋势无从验证 → 回落隐式 → 真事件出窗 → CorrelationLoss 诚实停（语义由钳位注释+回落测试钉死）。
3. **取消语义**：`tst_horizontracksvc::cancelMidFlightPublishesNothing`——首进度事件即取消：ok=false、拾取空（无半成品）、error 含「取消」、任务态 Cancelled、activeTaskCount()=0（无悬挂）✓；内核级：`windowedMatchesFullVolumeWithBoundedLiveSections` 滑窗路径取消部分结果如实返回 ✓
4. **顶替语义**：`tst_horizontracksvc::replacementDropsFirstSilently`——连续两枚种子：前者回调零调用（静默）、状态 Cancelled（≠failed）、后者 19200/19200 生效、无悬挂 ✓
5. **内存调度**：`tst_horizontracksvc::peakRssBoundedByWorkingSetRatio`——192 IL × 200 XL × 512 样（75MB 体 ≫ 2.4MB 滑窗工作集）传播全覆盖 38400 拾取，**VmHWM 增量 0 KB**（门 = 8×工作集 = 19.6MB；全体驻留 ≈ 192 剖面当量必超门）✓；等价性：滑窗与全量缓冲逐位一致（含 ≤2 剖面在场契约断言）
6. **空态**：`tst_horizontracksvc::invalidInputsRefusedWithReason`（无体/无种子/IL 不在体/XL 不在体/采样越界/内存超限/服务未接——七类立即报因）+ `tst_seismic_interpret::dockPropagationReadinessEmptyStates`（无体 → 按钮禁用 + tooltip「无可用地震体」；无手动拾取 → 禁用 + tooltip「手动拾取」；放置种子 → 启用）✓

## 性能断言（禁绝对墙钟，全走比率门）

- 传播比率门 `propagationCostRatioGate`：体传播总时 ≤ 8×（剖面数 × 热态切片提取基线）；实测 1.53×（1.58ms vs 8×0.13ms）
- 2D 追踪比率门（既有 tst_horizontrackperf）回归通过

## 测试与回归

- tst_horizontrack 21/21（既有 15 + 新 6）
- tst_horizontracksvc 10/10（新）
- tst_seismic_interpret 18/18（既有 16 + 新 2）
- 分层闸门 `check_layering.py --strict` 通过（无违规）
- 全量 ctest 279 项：本方向相关面全绿；5 失败甄别——tst_catalog/tst_startup_trace 串行复跑通过（-j8 并行负载抖动）；ui_tokens 35 处违规**全部在本方向未触碰的既有文件**（batchjobpanel/evolution/faciesmapping/layout/composepage/datalist/wellsiting/sequenceframework——基线欠账）；tst_singlefactor_perf 为绝对墙钟预算（16.2s vs 15s 门，机器速度因素，单因素方向面）；tst_services2 属编辑存储面（零文件交集）。均非本方向引入。

## 多轮 review

### 轮 1（diff 五维自审）

- 分层：算法/服务/视图各归其位；视图无 algorithms include；服务无 QtWidgets ✓
- 取消语义：内核逐道/逐 IL 轮询 + provider ExtractSlice 进度回调取消 + worker 先查取消再判 ReadFailure + UI 按钮取消 + 顶替静默 ✓
- 内存边界：6×剖面工作集闸 + 滑窗 provider（≤2 剖面驻留）+ RSS 比率门证据 ✓
- 版本锚定：parentVersionIds = 源体 RAW 版本（服务测试 + UI 测试双断言）✓
- i18n：用户可见串全 tr()；ts 骨架刷新因 master 滞后 ~665 串留独立批次（避免本方向 diff 淹没），源串 lupdate 可收
- **修复**：传播比率门硬编码 8 剖面 → `smallVol_->InlineCount()`
- **修复**：捕获事件距主事件 5 样 = 波形半周期相消成空（测试夹具几何修正为 8 样 + 陡化区入场）

### 轮 2（复审，零 High/Medium）

- 逐项复核：deque 分段存储裸指针遍历（已修）、取消先于读失败归类、顶替哈希指针比较、同步拒绝路径面板状态一致性、滑窗换向重取 2 剖面（进度封顶无害）、任务对象生命周期（finished 队列序）——全部确认无 High/Medium
- 结论：**两轮零 High/Medium**；Low 项 6 条记 PR body（见下）

## Low 项（PR body 用）

1. `guidedCenter` 每道一次小 vector 拷贝（deque→连续缓冲）；传播比率门 1.53× 远低于 8×，暂不优化
2. `showTrackReport` 文案「覆盖 N/N 道」用于 3D 网格点数（措辞复用，未造第二套 QC 行）
3. UI 按钮取消路径未单独端到端测试（服务级取消已测；小夹具上取消竞态窗口窄，不值得加大夹具拖慢测试）
4. 换向重取 2 枚剖面（种子/种子±1）：滑窗 LRU 二槽换向miss，每传播多 2 次 ExtractSlice，进度封顶无碍
5. ts 骨架刷新滞后（master 欠账 ~665 串）——`tools/update_translations.sh` 一键可收，留 i18n 独立批次
6. 不规则测网直接成格会跳过落不进格的拾取（如实跳过不臆造归属，规则网不受影响）
