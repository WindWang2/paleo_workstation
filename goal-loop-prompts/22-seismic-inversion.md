# Goal-Loop 方向 22：地震反演（子波库 / 低频模型 / 带限与稀疏脉冲反演 → 波阻抗体）

## 背景（实测事实，勿再勘察）

- **反演全绿场**：repo 无 inversion 代码。但输入端全部就位：
  - **子波/褶积链**：`services/seismictaskservice.h:548` 的 D5.4 合成记录——AC+DEN→波阻抗→反射系数→Ricker 褶积，`syntheticTrace(...)` 已带 `TimeDepthModel` 时深表接线（#12 落）。
  - **井震标定**：`tst_seismic_welltie.cpp` + `ui/correlation/`（合成道与井旁道对比 UI）。
  - **地震数据读面**：SEG-Y 索引 + sequential/traceindex 双策略读面（io-perf 段）；`seismicattr.cpp` 属性提取管线先例。
  - **低频模型的料**：层位栅格（GRID DERIVED）+ 井波阻抗曲线（`welllogset` 读面 #108）+ 网格化（`singlefactor/localidw`/`constraint_idw`，方向 18 还会补克里金）。
  - **产出面**：DERIVED 资产登记 + `publishLocalDirectionJob` 三段式（方向 20 可能抽成 JobRunner——本方向按现状 API 写，不依赖未落地重构）。
- **域语义**：反演是**时间域**运算（地震道域）——井阻抗曲线需 MD→TWT 经 `TimeDepthModel`/`tdTableFor`（#6 已落）。
- 真工区 env `PALEO_REAL_PROJECT_AREA` 有 200P_seismic.sgy + 井 + 时深表可全链实测。
- 纪律：读 DESIGN.md 再做 UI；每 `src/` 文件三层标记；新顶层模块先 `scripts/new_module.sh` 登记；`add_paleo_test`；`check_layering --strict` 绿；oracle 全数值断言；不引外部反演库。

## 目标形态

经典确定性反演链，四件：**子波提取与库 → 低频阻抗模型 → 带限反演（道积分）→ 稀疏脉冲反演**。

1. **`src/algorithms/inversion/`**（新模块登记）：
   - **子波**：`extractWavelet(seismicTrace, reflSeries, t0, len)`——井旁道-反射系数对最小二乘/谱比法提子波（比 Ricker 真实）；`Wavelet` 值对象（采样+主频估计+相位旋转）；子波库持久化（project.sqlite 新表 or 资产 `role="wavelet"`，勘察轮定）。
   - **低频模型**：`lowFreqImpedance(horizons, wellImpedances, grid)`——层位约束的井阻抗低频插值（沿层内插，缺层回退全局趋势），输出低频波阻抗栅格；**明确记模型频段**（0–lowcut Hz）。
   - **带限反演（道积分）**：`bandlimitedInversion(trace, lowFreqModel)`——反射系数→递归积分→相对阻抗→低频模型补偿成绝对波阻抗；逐道独立可并行。
   - **稀疏脉冲反演**：`sparseSpikeInversion(trace, wavelet, lowFreq)`——L1 稀疏约束反射系数序列（软阈值迭代/FISTA 自研，不引库）→ 褶积残差收敛停机；输出阻抗 + 残差能量 QC 图。
2. **编排**：`InversionWorkflow` 三段式（道分块 worker 并行，chunked 输出 DERIVED 栅格/SEG-Y-like 体）；井震标定 UI 挂「提取子波」入口；反演参数面板（频带/正则化/迭代上限）。
3. **QC 面**：合成记录-反演阻抗井旁道对比（复用 correlationtrack）；残差能量分布图；低频模型贡献百分比如实显示。

## Oracle 验收（全部须实测通过并记账本）

1. **子波提取**：合成道（已知 Ricker 褶积已知反射系数）→ 提取子波与真子波相关系数 ≥0.95 断言；长度/主频断言。
2. **低频模型**：合成层位+井阻抗 → 低频场在井位处 = 井阻抗低频断言；无层位回退全局趋势断言；频段标注断言（lowcut 值入产物 extra）。
3. **带限反演**：合成层状模型（解析阻抗）→ 反演阻抗逐层误差阈值断言；零相位子波 vs 混合相位对拍（相位失配如实反映——不强求校正）。
4. **稀疏脉冲**：稀疏反射系数序列 → 恢复反射系数 L1 误差阈值断言；含噪道稳定性（SNR 下降如实反映在残差，不崩不 NaN）。
5. **反演闭环**：合成记录（AC+DEN→反射系数→子波褶积）→ 反演回阻抗 → 与原始阻抗相关性阈值断言（全链闭环）。
6. **编排**：DERIVED 登记 + 分块并行 worker + 取消/进度/失败诚实。
7. **性能**：真工区剖面反演（263k 道）比率门记账；道并行加速比断言。
8. ledger 全账 + `docs/progress/seismic-inversion.md`（反演方法口径、子波提取法、低频模型频段语义、稀疏约束实现、递延：多道/AVO 反演、地质统计反演、各向异性）。

## 勘察指引

- `src/services/seismictaskservice.h:548`（`syntheticTrace`/`TimeDepthModel` 接线——反演的正向链）
- `src/ui/correlation/`（井震标定 UI——子波提取挂点）+ `tests/tst_seismic_welltie.cpp`（合成道夹具——反演测试的料）
- `src/algorithms/seismicattr.cpp`（属性逐道处理管线先例）、`src/io/segyreader.*`（道读面/双策略）
- `src/services/welllogset.*`（井阻抗曲线读取——AC×DEN 合成的波阻抗曲线或 `curveexpr` 曲线计算器现成）
- `src/io/constraintstore.*` + `workflows.cpp`（DERIVED 登记与三段式编排先例）
- `src/algorithms/singlefactor/samples.*`（井点样本采集——低频模型插值输入）

## 禁区

- 只做**确定性叠后反演**：不做叠前 AVO/弹性阻抗/地质统计反演（递延写明）。
- 不做多道横向约束反演（逐道独立 + 低频模型约束是本边界）；不做井间横向变差约束。
- 子波提取只单井/井旁道对；不做全工区子波场插值（若做多子波入递延）。
- 不碰 `src/catalog`、`src/services/welllogset` 读面（只消费不改）、方向 18–21 在飞文件面。
- 反演结果**不假装**高频成分——低频模型频段边界如实显示在产物元数据，不许 UI 标「高分辨率」。
- 不引外部库（FFT 自研或复用 Qt/系统 FFTW 先看现状，Eigen 不入）。

## 迭代协议

- **轮0**：勘察定案——子波持久化落点（表 or 资产角色）、`syntheticTrace` 可复用面、低频模型插值核选型（localidw/constraint_idw 复用）、道分块策略；`Wavelet`/反演 API 签名进 ledger。
- **轮1**：子波提取 + `tst_inversion_wavelet`（相关系数断言）。
- **轮2**：低频模型 + `tst_inversion_lowfreq`。
- **轮3**：带限反演 + `tst_inversion_bandlimit`（层状模型闭环）。
- **轮4**：稀疏脉冲 + `tst_inversion_sparse`。
- **轮5**：`InversionWorkflow` 三段式 + DERIVED + UI 挂点 + `tst_inversion_workflow`。
- **轮6**：真工区 + 性能记账 + progress 收口。

## 交付协议（必守）

1. **新建 worktree 分支开发**，禁止在主 checkout 写代码：
   `git worktree add .worktrees/inversion -b goal/seismic-inversion-<date> origin/master`
2. 每轮原子提交，ledger 同步记账。
3. **完成后必须先仔细 review 再开 PR**：`git diff origin/master...HEAD` 全量自审；无调试残留；层标记齐；`check_layering --strict` 绿；vendor 前缀全量构建零新警告；ctest 全绿；Oracle 每条有命令+输出证据。
4. 过门禁后 `git push` + `gh pr create`，不等 CI 不等人。
