# Goal-Loop 方向 19：井轨迹空间化（测斜模型 / MD↔TVD / 定向井贯穿消费面）

## 背景（实测事实，勿再勘察）

- **测斜全绿场**：整个 repo 只有一个解析器——`src/io/wellcompositexml.h` 的 `XmlDeviationStation` + `parseDeviationSurvey()`（井场 XML 格式，方位/井斜/MD 站点表），**无域模型、无实体角色、无 MD/TVD 变换、无消费方**。所有井都当**直井**处理。
- **井多文件模型已立**：`services/welllogset.*`（#108）——`WellLogFile/WellCurveRef/wellCurveIndex/readCurve`，曲线带文件溯源。但曲线只有 MD 域。
- **井位锚点**：`io/welltopspiper.*` WellTopsPiper 有 `role="tops"`、`io/wellheadxml` 有 `role="well_head"`（含井口坐标）——新角色 `well_deviation` 同族照抄；`meta.pathForRole("tops")` 映射先例。
- **消费面现状全是直井假设**：`sectionworkbench` 剖面井轨垂线、`mappingworkbench` 井位点、`propertymodelworkflow` 井-网格相交（`wellCells`/`wellCellsForRefinement` 按垂直井筒）、`geoobjects` 3D 井显示、`wellcomposite` 综合图全 MD 轴。
- **井数据井深域语义**：`projectdata.assetFilePathFor`/`topsFor`/`tdTableFor` + `welllogset` 读面都在 `services/projectdata.*` 一族。
- 真工区 env `PALEO_REAL_PROJECT_AREA` 有井 + 分层 + 层位，定向井含量待查（勘察轮第一件事：真工区有没有测斜数据可导）。
- 纪律：读 DESIGN.md 再做 UI；每 `src/` 文件三层标记；`add_paleo_test`；`check_layering --strict` 绿；oracle 全数值断言；不静默无效值。

## 目标形态

井不再是垂直线。**测斜站点 → 三维轨迹 → 全部消费面按真实轨迹吃井**。

1. **域模型**：`WellDeviationSurvey`（站点表 MD/incl/azimuth + `tvdAt(md)/xyAt(md)/position3D(md)` 插值——最小曲率法 minimum curvature，行业标准）入 `src/domain` 或 `src/services`；`role="well_deviation"` 链接挂井实体；`nextOrdinalForRole` 照抄先例。
2. **导入**：测斜文件导入（文本站表 + 井场 XML `parseDeviationSurvey` 现成解析器 + LAS 里 DEV 段若有）；`classifyImportFile` 加测斜判据（incl/azimuth 列名表识别）。
3. **MD↔TVD 域服务**：`welllogset` 每条 `WellCurveRef` 增 TVD 域读法（MD→TVD 逐点映射；多文件逐文件）；`tdTableFor` 的同族接口 `trajectoryFor(wellId)` 走 projectdata。
4. **消费面全迁**：
   - **剖面**：定向井按真实轨迹投影剖面（不再垂线），井轨曲线随轨迹；
   - **平面图**：井位点 → 井底位移轨迹线（surface→TD 投影）；
   - **3D**：`geoobjects` 井轨迹管线实体；
   - **属性建模**：`wellCellsForRefinement`/`wellCells` 井-网格相交按真实三维轨迹（定向井穿层段正确性——这是本方向最硬的域语义）；
   - **综合图**：保留 MD 轴主显 + TVD 轴可选轨（`propertymodel` 与 `petrophys` 的曲线域选择面）。
5. **UI 落点**：井页/实体树挂测斜文件链；综合图 MD/TVD 域切换；剖面轨迹如实显示。

## Oracle 验收（全部须实测通过并记账本）

1. **最小曲率法数值断言**：直井（incl=0）轨迹 = 垂直线断言；90° 造斜 → 水平位移精确断言；合成造斜-稳斜剖面 TVD/位移手算对拍（≤3 站小规模）。
2. **导入**：`role="well_deviation"` + 链接 + ordinal + unresolved 诚实面；XML 与文本站表双格式。
3. **MD/TVD**：合成曲线逐点 MD→TVD 映射断言（直井恒等；定向井非恒等）；多文件井每文件各自映射。
4. **消费面**：剖面定向井轨迹与理论投影差断言（像素/坐标容差）；propmodel `wellCells` 定向井穿层段数 > 直井断言语义（真实轨迹相交）；平面图轨迹线端点 = 井底投影坐标。
5. **无测斜井**：直井回退语义显式（`trajectoryFor` 返回 nullopt → 消费面保持原直井路径），不虚构造斜。
6. **迁移**：老工程无 deviation 链接 → 全直井路径行为不变断言。
7. ledger 全账 + `docs/progress/well-trajectory.md`（最小曲率法公式口径、插值策略、域切换面、消费面迁移清单、递延：扭转角/井径校正/多分支井）。

## 勘察指引

- `src/io/wellcompositexml.h`（`XmlDeviationStation`/`parseDeviationSurvey` 现成解析器与站点字段）
- `src/io/welltopspiper.*`（`role="tops"` 的完整角色先例：类型判据/meta 映射/井实体链接/迁移）
- `src/services/welllogset.*`（#108 的多文件曲线读面——TVD 域在这里挂）、`src/services/projectdata.*`（`topsFor`/`tdTableFor` 同族接口落点）
- `src/workflow/sectionworkbench.cpp`（剖面井轨）、`src/workflow/mappingworkbench.cpp`（井位）、`src/workflow/propertymodelworkflow.cpp`（`wellCells`/`wellCellsForRefinement` 井-网格相交）
- `src/domain/wellcompositemodel.h`（`TrackSpec.depthFamily` MD/DEPTH 域先例）、`src/ui/wellcomposite/`（综合图轴）
- `src/app/dataimportservice.cpp`（classifyImportFile 类型判据表）、`metadata/entitystore.h`（AttachLink/unresolved 契约）

## 禁区

- 只单分支井轨迹——分支井/侧钻（sidetrack）显式拒绝或递延，不硬做。
- 最小曲率法以外的高级插值（radius of curvature、样条）递延；不做扭转角/井径/磁场校正。
- 不改 `welllogset` 的曲线溯源/别名协议；不动 `src/catalog`。
- 不做测斜数据编辑 UI（导入→用，不做交互式编站点）。
- 无测斜井**绝不**虚构造斜——直井回退是显式语义不是兜底猜测。
- 单一职责：不动栅格化/克里金（方向 18 的地统包不交叉）。

## 迭代协议

- **轮0**：勘察定案——真工区有无测斜数据、`parseDeviationSurvey` 站点字段实测、`wellCells` 相交算法现状；`WellDeviationSurvey` API 签名 + `role="well_deviation"` 注册点进 ledger。
- **轮1**：域模型 + 最小曲率法 + `tst_deviation`（轮1 Oracle 数值断言）。
- **轮2**：导入 + 角色链接 + `tst_deviation_import`。
- **轮3**：MD/TVD 服务 + welllogset 域 + `tst_welllogset` 扩面。
- **轮4**：消费面迁移（剖面/平面/3D/综合图）+ propmodel 相交 + `tst_welllogset_tvd`/`tst_sectiontrajectory`。
- **轮5**：真工区实测（无测斜井回退面断言）+ progress 收口。

## 交付协议（必守）

1. **新建 worktree 分支开发**，禁止在主 checkout 写代码：
   `git worktree add .worktrees/welltraj -b goal/well-trajectory-<date> origin/master`
2. 每轮原子提交，ledger 同步记账。
3. **完成后必须先仔细 review 再开 PR**：`git diff origin/master...HEAD` 全量自审；无调试残留；层标记齐；`check_layering --strict` 绿；vendor 前缀全量构建零新警告；ctest 全绿；Oracle 每条有命令+输出证据。
4. 过门禁后 `git push` + `gh pr create`，不等 CI 不等人。
