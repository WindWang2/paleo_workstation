# Goal-Loop 方向 15：多文件井曲线汇聚读面（Well LogSet / 跨文件并集曲线）

## 背景（实测事实，勿再勘察）

- **数据模型已支持一井多文件**，只是语义被打了折：`EntityAssetLink`（`src/catalog/datacatalog.h:49`）有 `role="well_log"` + `isPrimary` + `ordinal`（注释明写「多 LAS 加载顺序」）+ `unresolved`；`linksForEntity` 返回全量链接。
- **痛点在读写两侧**：
  - 导入侧：`src/io/dataimportservice.cpp` LAS 绑定井后一律 `link.isPrimary=true`（~1107 行），而 `DataCatalog::addLink` 的「同 (entity,role) 只留一条主关联」会把前一个文件降级——**同井导入第二个 LAS，前一个文件的曲线就从所有消费面消失**（后导入者通吃）。
  - 读侧：几乎所有消费方只取 `isPrimary` 一条：`ProjectDataFacade::assetFilePathFor`（src/services/projectdata.cpp:98）、`src/workflow/sectionworkbench.cpp:172`、`src/services/petrophyscomputeservice.cpp`（挑一条 ~446 行）、`src/workflow/propertymodelworkflow.cpp`（~675 行一井选一 LAS）。
  - **正确姿势的现存先例**：`src/services/crossplotsources.cpp:27` 遍历该井全部已决 well_log 链接、逐文件 `parseHeader` 枚举曲线——照它的口径做统一化。
- LAS 读面已有列级能力：`src/io/lasparser.h` 的 `parseHeader`（只读 ~C 段）、`parseRange`/`parseDepthRange`（按深度窗/列选读），不需要整文件载入。
- 井的其他文件类已各有角色：`well_head`（井位 XML）、`well_stratification`（分层）、`time_depth`（时深表）；分类器 `src/domain/projectclassifier.cpp`。
- 操作面现成：`setLinkPrimary`（links() 序 index → 主关联切换，datacatalog.h:208）、`attachLink`/`setLinkUnresolved`；实体面板/数据列表已能显示链接（`src/ui/pages/entitypanel.cpp`、`datalist.cpp` 按 role 分行）。
- 井综合图消费口：`WellCompositePanel::loadLasCurves(wellName, curves, intervals)`（`src/ui/wellcomposite/wellcompositepanel.cpp:539`）收的是预解析曲线组——天然适配「汇聚后的曲线集」输入。
- catalog 纪律：owner-thread 写守卫（#106）、produce-then-commit journal、BatchSave——**本方向是纯读侧+导入语义修正，不碰这些机制**。
- 纪律：读 DESIGN.md 再做 UI；每 `src/` 文件三层标记；`add_paleo_test`；`check_layering --strict` 绿；oracle 不接受「应该没问题」。

## 目标形态

**一口井 = 有序文件集 → 并集曲线表（带溯源）**。

在 `src/services/`（数据层，不碰 QtWidgets）新增 `WellLogSet` 读面（挂 `projectdata` 或独立文件，读 DESIGN/层规后定）：

```cpp
struct WellLogFile {     // 该井的一条已决 well_log 链接
  QString assetId, versionId, path;   // 当前版本解析后路径（managed/外链统一）
  bool isPrimary; int ordinal;
  QStringList curveNames;  // parseHeader 即得，不读数据体
};
struct WellCurveRef {    // 并集曲线表的一条
  QString mnemonic;        // 曲线名；跨文件重名时保留为 GR@文件名 别名
  QString sourceVersionId; // 溯源
  QString path; int column; bool canonical;
};

QVector<WellLogFile>  wellLogFiles(wellId);   // 已决链接，按 ordinal→版本序
QVector<WellCurveRef> wellCurveIndex(wellId); // 跨文件并集 + canonical 仲裁
```

- **冲突策略**：同名 mnemonic 跨文件 → 主文件列保留原名并标 canonical，其余以 `<mnemonic>@<文件basename>` 别名入表（不丢不静默覆盖）；同文件内同名列如实报 warning。
- **canonical 切换**：`setLinkPrimary` 现成；index 里 canonical 随主关联翻转。
- **消费侧迁移**（按序）：petrophyscomputeservice → propertymodelworkflow → sectionworkbench → wellcomposite 入口改收 WellCurveRef 集。crossplotsources 换统一 API（删重复头解析）。
- **导入修正**：井已有已决 well_log 主关联时，新 LAS 以 `isPrimary=false, ordinal=max+1` 入库（不挤掉主文件）；`ordinal` 分配同 next*Id 纪律。首个已决文件仍自动为主（行为不变）。
- **最小 UI**：实体/资产面板的 well_log 组按 ordinal 列全部文件 + 单文件「设为主文件」动作（走 setLinkPrimary 现成面）；视图只发信号。

## Oracle 验收（全部须实测通过并记账本）

1. **并集核**：夹具——同井两 LAS：fileA 含 DEPT+GR+SP、fileB 含 DEPT+RT+NPHI（不同深度段）。`wellLogFiles` 返回两条且序正确；`wellCurveIndex` 返回 GR/SP/RT/NPHI 并集，每条带正确 sourceVersionId。
2. **重名冲突**：fileA 与 fileB 都含 GR → index 出两条：主文件的 `GR`（canonical）+ 次文件的 `GR@fileB`；切主关联（setLinkPrimary）后 canonical 翻转。同文件同名列 → 告警计数，不静默。
3. **导入序回归**：连续导入同井 LAS×2 → 两条已决链接共存、首个仍为主、ordinal 递增；导入前 catalog 无井 → 未决链接行为不变（旧 oracle 不回归）。
4. **消费迁移验证**：petrophys 批处理对「曲线跨两文件」的井能取到 RT 并产出结果曲线；sectionworkbench 井轨能显示非主文件曲线；propertymodel 的 requestFromCatalog 曲线集 = 并集。各新增一条断言级测试。
5. **性能**：`wellCurveIndex` 只跑 parseHeader（不读数据体）；100 井 × 3 文件合成工程 < 500ms 比率门内，单井调用 O(#links)。
6. **诚实面**：文件缺失/损坏 → 该链接如实跳过并计 warning 面；井无已决链接 → 空集（不臆造）。
7. ledger 全账 + `docs/progress/well-logset.md`（冲突仲裁口径、canonical 语义、迁移清单、递延项）。

## 勘察指引

- `src/catalog/datacatalog.h`（链接结构与 setLinkPrimary/attachLink 面）、`src/io/ingestplan.cpp:629`（ordinal 稳定排序先例）
- `src/services/crossplotsources.cpp:20-50`（多链接枚举先例——这是本方向的蓝本）、`src/services/projectdata.cpp:98-140`（assetFilePathFor 单文件口径、要改的对象）
- `src/io/lasparser.h`（parseHeader/parseRange 列级读面）、`src/io/lasdoc.h`（LasDoc/LasCurve）
- `src/io/dataimportservice.cpp:1079-1120`（LAS 绑定与 isPrimary 写入点——修正处）
- 消费点清单：`src/services/petrophyscomputeservice.cpp:440`、`src/workflow/propertymodelworkflow.cpp:675`、`src/workflow/sectionworkbench.cpp:172`、`src/ui/wellcomposite/wellcompositepanel.cpp:539`、`src/ui/datapreview/datapreviewtabs.cpp:2689`
- `src/ui/pages/entitypanel.cpp:1295`/`datalist.cpp:1257`（well_log 链接展示行——加「设为主文件」动作的落点）

## 禁区

- **不动存储底座**：catalog.json 不换库（catalog.sqlite 是另一个立项方向，本方向不碰）；不新增 SQLite 表。
- 不新增文件格式支持（DLIS/LIS/BE 等二进制测井格式另立项）；不碰 seismic 读路径。
- 不改 `addLink` 的「同角色单主」不变量——多文件靠多链接共存，不靠多主关联。
- 不抢 UI 重构：只做实体面板/数据列表的最小增量（列文件 + 设主动作），不重排页面结构。
- 不破坏 unresolved 链接语义与 appendUnresolvedLasLink 既有行为（未决资产不得被本方向「顺手」挂接）。
- 曲线数据本身不落库/不内联进 catalog——文件永远是本体，索引只记头信息。

## 迭代协议

- **轮0**：勘察定案——确认 LasParser 的 parseHeader 签名/耗时、linksForEntity 返回序、RoleRegistry 对 well_log 的角色词表校验细节；接口签名进 ledger。
- **轮1**：`WellLogSet` 读面（wellLogFiles/wellCurveIndex/冲突仲裁）+ `tst_welllogset`（Oracle 1/2/6 断言）。
- **轮2**：导入序修正（非首 LAS 不再抢主）+ ordinal 分配 + `tst_import`/`tst_folderimport` 增回归（Oracle 3）。
- **轮3**：消费迁移 petrophys + propertymodel + sectionworkbench + `tst_*` 断言（Oracle 4）。
- **轮4**：crossplotsources 换统一 API + wellcomposite 入口 + UI 最小增量（文件列 + 设主）+ `tst_entitypanel` 级回归。
- **轮5**：真工区实测（`PALEO_REAL_PROJECT_AREA` 门控：多册 LAS 的井验证并集）+ 性能记账 + docs/progress 收口。

## 交付协议（必守）

1. **新建 worktree 分支开发**，禁止在主 checkout 写代码：
   `git worktree add .worktrees/well-logset -b goal/well-logset-<date> origin/master`
2. 每轮原子提交，ledger 同步记账。
3. **完成后必须先仔细 review 再开 PR**：
   - `git diff origin/master...HEAD` 全量自审：无调试残留/死代码；层标记齐；`check_layering --strict` 绿；
   - vendor 前缀全量构建零新警告，ctest 全绿；
   - Oracle 每条有命令+输出摘要证据；
   - 发现问题先修再验直到干净。
4. 过门禁后 `git push` + `gh pr create`，不等 CI 不等人。
