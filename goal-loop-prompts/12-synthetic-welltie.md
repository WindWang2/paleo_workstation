# Goal-Loop 方向 12：合成地震记录与井震标定（Synthetic Seismogram & Well-Tie）

## 背景（实测事实，勿再勘察）

- `src/io/lasparser.cpp` + `LasDoc`：LAS 曲线已在仓（BOM/多曲线/块边界已修硬），DT（声速时差）/RHOB（密度）曲线名解析走 `aliasCanonicalization`（`tst_perf_las` 已验别名族）。
- `src/io/timedeptool.{h,cpp}`：TD 表双程时间↔深度插值，TdResult.status 区分缺表/越界/无序——合成记录重采样（深度域采样→时间域等间隔道）必须走它，**不许自己另写插值**。
- `src/algorithms/velocitymodel`（goal/time-depth-velocity）：V0-k 模型 + checkshot 锚点；井震标定产生的时深对最终要回灌/对比该模型（平移-漂移量 → checkshot）。
- `src/algorithms/seismicattr`：自研 radix-2 FFT 现成——子波估计、相关分析、频带工具直接复用，**禁引 FFTW**（钉位政策先例）。
- `src/linkage/` 井-震联动 + `src/ui/seismicsection/*` 剖面画布：合成道对比显示挂在剖面井位处（先例：`seismicsectiontool` + `viewpanel` 的叠加层机制）。
- `src/services/` 任务服务（`SeismicTaskService` 先例）：计算走异步任务+取消+进度，UI 只发信号。
- 真工区 `/home/kevin/projects/paleo_project/data/project_area`：966MB `200P_seismic.sgy`（263k 道）+ 井 LAS 可实测（env 门控 `PALEO_REAL_PROJECT_AREA`）。
- 设计纪律：UI 决策前读 `DESIGN.md`；每 `src/` 文件头三层标记；`add_paleo_test` 注册；`tools/check_layering.py --strict` 必须绿。

## 目标形态

井旁闭环：**LAS(DT+RHOB) → 波阻抗 AI=ρ·v → 反射系数 R → 褶积子波 → 合成道**；与井旁真实地震道并排显示 + 相关分析（相关系数/时移扫描）；用户调整（时移/stretch、子波、频带）→ 标定结果导出为 checkshot 时深对喂给 velocitymodel。

## Oracle 验收（全部须实测通过并记账本）

1. **数值核正确性（断言级，非截图）**：
   - AI 计算：`AI_i = ρ_i·v_i`（v=1e6/DT us-ft 换算常量化钉死）；R_i=(AI_{i+1}-AI_i)/(AI_{i+1}+AI_i)；恒定介质 R≡0、阶跃界面幅值精确断言。
   - 褶积：合成道 = R ⊗ w；用 delta 反射系数断言合成道等于子波本身（时移对齐后逐样本近似 1e-6 容差）；频域/时域两种实现交叉对拍。
   - 深度采样→时间重采样走 `TimeDepthTool`：checkshot 锚点处合成道时间与 TWT 误差 < 1 采样。
2. **子波提取**：从井旁道估计子波（统计子波：道整形平均或反褶积）+ Ricker 理论子波发生器（主频参数化）；断言提取子波主频落在 ±20% 真值内（用已知合成数据反演验证）。
3. **标定工作流**（功能层 `src/workflow/welltie` 或并入现有编排）：时移扫描 ±100ms 找最大互相关 lag；stretch/squeeze 分段漂移支持；产出 `T_D_pair` 表（时间↔深度对）→ 存为 checkshot 类资产登记 catalog（DERIVED 产物先例），供时深模型对比。
4. **UI 闭环**：井列表选井 → 合成道参数面板（子波类型/主频/时移）→ 剖面画布井位处并排显示「合成道|井旁道」+ 相关系数读数；参数改动实时重算（≤200ms 感知）。视图只发信号。
5. **性能**：单井合成链（读曲线→合成道→对比）端到端 < 500ms；批量 20 井 < 10s；比率门不钉绝对墙钟。
6. **取消/进度/失败诚实**：缺 DT 或 RHOB → 如实告知缺哪条曲线（不静默补 0）；任务可取消；进度单调。
7. ledger 账本 + docs/progress/synthetic-welltie.md 记档（语义决策/递延清单/实测数字）。

## 勘察指引

- `src/io/timedeptool.h`、`src/algorithms/velocitymodel/*`（插值语义锚点先例）
- `src/algorithms/seismicattr`（FFT/Hilbert 复用）、`src/services/seismictaskservice.*`（异步任务先例）
- `src/ui/seismicsection/seismicsectiondockwidget.*` + `seismic3d/seismicslicerenderer` 叠加层先例（属性叠加 NaN=透明已落）
- `src/linkage/*`（井位→剖面道号映射）
- `src/metadata/` DERIVED 资产登记先例（SATR/depth_raster）
- `docs/PALEO_QGIS_PLAN.md`、`TODOS.md`、`BUILDING.md`「依赖来源策略」

## 禁区

- 不改 `SegyReader`/`SegyIndexCache` 热路径（读道走现成接口）。
- 不引第三方 DSP/FFT 库；不碰 vendor/。
- 不做全工区批量标定调度（本轮单井闭环+批量顺序跑即可，分布式调度另立项）。
- 不重写曲线计算器（#8 petrophysics 已有逐点表达式引擎——AI/R 派生曲线若要进 LAS 写回走它，不重复造）。
- UI 不引入 DESIGN.md 外的新视觉语言；不往 `src/ui` 塞计算逻辑。

## 迭代协议

账本轮次推进（每轮一个原子可验增量）：

- **轮0 勘察定案**：确认 DT/RHOB 别名族覆盖、井旁道抽取接口、叠加层挂载点；把技术选型+接口签名写进 ledger。
- **轮1 数值核**：`src/algorithms/synthetic/{impedance,reflectivity,wavelet,synthtrace}.cpp` + `tst_synthetic`——delta/阶跃/恒定介质解析解全断言绿，再进编排。
- **轮2 编排**：`WellTieWorkflow`（LAS→AI→R→conv→compare→checkshot 产出链），任务服务+取消+进度+失败诚实；`tst_welltie`。
- **轮3 UI**：合成道参数面板 + 剖面井位并排道叠加 + 相关系数读数；`tst_welltiepanel`（keyClick 断言先例）。
- **轮4 标定深化**：时移扫描/分段漂移/checkshot 回灌 velocitymodel + 真工区实测（env 门控）。
- **轮5 收口**：ledger 全绿、docs/progress、BUILDING/AGENTS 如需更新、TODOS 递延项（反演/多井联合标定等）。

## 交付协议（必守）

1. **新建 worktree 分支开发**，禁止在主 checkout 写代码：
   `git worktree add .worktrees/welltie -b goal/synthetic-welltie-<date> origin/master`
2. 每轮原子提交（信息写「为什么」），ledger 同步记账。
3. **完成后必须先仔细 review 再开 PR**——硬性 checklist：
   - `git diff origin/master...HEAD` 全量自审：无调试残留/printf/死代码；层标记齐；`check_layering --strict` 绿。
   - vendor 前缀下全量构建零新警告，ctest 全绿（含新测试）。
   - Oracle 每条在 ledger 有命令+输出摘要证据，不接受「应该没问题」。
   - 发现问题先修再验直到干净。
4. 过门禁后 `git push` + `gh pr create`，不等 CI 不等人。
