# Goal-Loop 方向 13：三维地层格架与属性建模（Stratigraphic Grid & Property Modeling）

## 背景（实测事实，勿再勘察）

- 解释侧构件已齐：层位面（gridding `src/algorithms/mincurvature`/`rasteralgebra`）、断层框架（`src/domain/seismic/faultset`+`faultsetstore`）、层位自动追踪、时深/速度模型（`algorithms/velocitymodel`+`domain/seismic/timedepthmodel`）。
- 测井侧构件已齐：LAS 流式解析、岩石物理公式库（`algorithms/petrophys`：Vsh/φ/Sw）、逐点曲线表达式引擎（`curveexpr`）、wellcomposite 面板/轨类型 12 种。
- 3D 视口已能渲染体（`src/ui/seismic3d/*`，GL 3.3 Core，TF/剖切/扫掠均在）；地震属性体核（`seismicattr`）已证 `SgyDataCache` 体窗读通路。
- **缺的是中间层**：把「层位面序列 + 断层」转成**地层格架（stratigraphic grid / pillar grid-lite）**，再把井曲线**粗化上格**并按**地层坐标插值充填**出属性体（φ/Vsh/Sw/相）。这正是工区从「解释」走向「建模」的缺失一环。
- 真工区 `/home/kevin/projects/paleo_project/data/project_area`（env 门控 `PALEO_REAL_PROJECT_AREA`）有井 LAS + 966MB SEG-Y + 层位/断层成果可全链实测。
- 纪律：读 DESIGN.md 再做 UI；每 `src/` 文件三层标记；`add_paleo_test`；`check_layering --strict` 绿；性能比率门不钉绝对墙钟；oracle 不接受「应该没问题」。

## 目标形态

V1 最小闭环（不追 pillar-grid 全功能）：**层位面选上下界 → 层段内规则 IJK 体网格（XY 跟随层位面范围/ILXL 或平面积分网格，Z 沿等比例分层 ∝ (z−top)/(bot−top)）→ 井曲线按层段粗化（每井×每层段 1 个代表值，加权/众数）→ 3D 插值充填（IDW/power-2 起步，变差函数克里金可后置）→ 属性体落盘 DERIVED 资产 → 3D 视口体渲染 + 剖面切片显示**。

断层本轮只做**阻断语义**（断层两侧的邻域不跨层段插值），不做断块错位网格（pillar/faulted grid 记递延）。

## Oracle 验收（全部须实测通过并记账本）

1. **格架核**（`src/algorithms/stratgrid`，纯数值）：
   - `buildZoneGrid(topSurface, botSurface, nLayers)`：IJK 网格几何正确——柱内单元体积>0、层段厚度=f(z) 单调、top=bot 奇异面如实拒绝、NaN 层位像素 → 死单元标记。
   - 地层坐标 `(i,j,k)` ↔ 物理坐标往返断言；沿地层坐标的邻接查询正确。
2. **井曲线粗化**（`src/algorithms/stratgrid/upscale.{h,cpp}` 或 petrophys 名下）：井轨迹×层段求交 → 段内连续/离散曲线粗化（均值/众数/中位数可选）；断言：恒定曲线粗化值=常数、分层边界处归属正确、井不穿层段 → 无值（不静默补）。
3. **充填插值**：seed 值 → IJK IDW（断层阻断：跨断层对不参与互相插值）；断言：单 seed 全场=该值（恒定场）、两点对称中面值≈均值、跨断层同位点不互相影响。
4. **编排与产物**：`PropertyModelWorkflow`（功能层）串 层位选择→格架构建→粗化→充填→**属性体 DERIVED 资产登记 catalog**（SATR/depth_raster 先例）+ 可复现参数记录（provenance：输入层位/曲线/参数 hash）。
5. **可视化闭环**：属性体在 3D 视口渲染（走现成体渲染/切片路径）+ 剖面画布层段叠加模式（IJK 切面投影回剖面）；NaN=透明先例。
6. **性能/内存**：真工区规模估算（e.g. 200×200×20 层 ≈ 800k cell）单属性链 < 30s、RSS 增量 < 2GB、进度单调、可取消；比率门 + env 门控真机实测。
7. ledger 全账 + `docs/progress/property-modeling.md`（语义决策：地层坐标定义/断层阻断口径/粗化默认聚合器/递延清单）。

## 勘察指引

- `src/algorithms/mincurvature.cpp`/`rasteralgebra.cpp`（面数据结构与栅格代数先例）、`src/domain/seismic/faultset.*`（断层邻接查询接口形态）
- `src/algorithms/petrophys/`（曲线公式与逐段聚合先例）、`src/io/timedeptool.*`（深度/时间坐标转换复用）
- `src/ui/seismic3d/seismicslicerenderer*`、`seismic3dtf`（体渲染挂载点）、`src/ui/wellcomposite/*`（井轨迹/曲线数据结构）
- `src/services/` 任务服务编排先例、`src/metadata/` DERIVED 登记先例
- `docs/progress/gridding-surface-ops.md`、`fault-interpretation.md`、`time-depth.md`（本批已落模块的语义决策，保持一致口径）

## 禁区

- 不做 pillar/faulted grid 全貌（阶梯断块网格、Y 型断层）——递延项写明。
- 不做沉积相带控制建模/对象建模（object modeling）、不做序贯高斯模拟（SGS）——变差/随机建模另立项。
- 不引第三方地质统计库；变差函数若做只实现球状/指数两种标准模型。
- 不改地震读热路径；不写 UI 内计算；不破坏层标记/词汇表登记（新模块先 `scripts/new_module.sh`）。
- 不与 goal/synthetic-welltie 抢同一文件面——井震标定产出若可用则消费，不可用不阻塞。

## 迭代协议

- **轮0**：勘察定案——层位面数据结构实际类型（GeoTIFF/QgsRasterLayer/网格对象）、断层邻接接口、井轨迹深度域可得性；接口签名进 ledger。
- **轮1**：`stratgrid` 格架核 + `tst_stratgrid`（几何断言全绿）。
- **轮2**：井曲线粗化 + `tst_upscale`（真 LAS 夹具）。
- **轮3**：IDW 充填 + 断层阻断 + `tst_propfill`（解析/对称/阻断断言）。
- **轮4**：`PropertyModelWorkflow` + catalog DERIVED + `tst_propworkflow`（含取消/进度/失败诚实）。
- **轮5**：UI——3D 体渲染挂载 + 剖面层段叠加 + 参数面板（只发信号）；`tst_propmodelpanel`。
- **轮6**：真工区实测（env 门控）+ 性能比率记账 + docs/progress 收口。

## 交付协议（必守）

1. **新建 worktree 分支开发**，禁止在主 checkout 写代码：
   `git worktree add .worktrees/property-modeling -b goal/property-modeling-<date> origin/master`
2. 每轮原子提交，ledger 同步记账。
3. **完成后必须先仔细 review 再开 PR**：
   - `git diff origin/master...HEAD` 全量自审：无调试残留/死代码；层标记齐；`check_layering --strict` 绿；
   - vendor 前缀全量构建零新警告，ctest 全绿；
   - Oracle 每条有命令+输出摘要证据；
   - 发现问题先修再验直到干净。
4. 过门禁后 `git push` + `gh pr create`，不等 CI 不等人。
