你在 paleo_workstation 项目实现「数据底座」：project_area 工区的实体—资产—关联—版本数据模型、catalog.json 主存储、分类导入、受管 RAW、SEG-Y 测线级读取、数据页预览标签（对应 docs/PROJECT_AREA_PLAN.md 的阶段 A + B + D，以及 §7 第一段实现）。

【执行方式】
- 单线程串行实现。**禁止**主动启用任何 workflow / teamwork / swarm / 并行子代理编排（不 spawn_subagent、不开 worker）。所有代码你自己写。
- 按 goal-loop 协议执行（若宿主有 /goal-loop 或 /goal skill 则调用之）：
  - 开始前声明一次：目标一句话、完成条件 Oracle（=下方【验收】全部通过）、迭代上限 40 轮
  - 在工作区根维护 `.goal-loop-ledger.md` 账本；每轮只做一处聚焦改动，亲自跑验证命令，记账 `第N轮 | 改动 | 验证结果 | 通过/未通过 | 下一步`
  - 验收未满足禁止宣告完成；卡住时换方法而不是退出；完成后关键验证连跑两遍
- TDD：每个交付物先写 QTest 失败测试（红），再实现到通过（绿）。测试文件用 `add_paleo_test()` 注册。
- 用 /qa、/review 类只读 skill 做自查可以；不要用它们衍生别的执行模式。

【环境】
- 仓库 /home/kevin/projects/paleo_workstation，C++20，Qt 6.11.2，QGIS 4.2.2 系统安装（headers /usr/include/qgis，libs -lqgis_core -lqgis_gui，moc=/usr/lib/qt6/moc）。系统 QGIS 已装，勿 vendor。
- 构建：`ninja -C build`；测试：`cd build && QT_QPA_PLATFORM=offscreen ctest --output-on-failure`。
- 权威契约：`docs/PROJECT_AREA_PLAN.md`（§1 数据事实 / §2 要改的行为 / §3 数据管理契约 / §4 预览标签 / §5 阶段 / §7 第一段）。架构边界 `docs/PALEO_QGIS_PLAN.md`；视觉 `DESIGN.md`；不做清单 `TODOS.md`。
- 先跑 gstack 自检：`_GS=""; for _D in "${GSTACK_ROOT:-}" "$HOME/.claude/skills/gstack" "$HOME/.grok/skills/gstack"; do [ -z "$_GS" ] && [ -n "$_D" ] && [ -d "$_D/bin" ] && _GS="$_D"; done; [ -n "$_GS" ] && echo GSTACK_OK || echo GSTACK_MISSING`
- 可移植的参考实现（只读，不整库依赖）：`/home/kevin/projects/paleo-merged-main/libs/ingest/src/classifier.cpp`（208 行分类规则）、同目录 `well_tops.cpp`、`xml_scanner.cpp`（XML 内容判定）；数据模型读 `/home/kevin/projects/paleo-merged-main/docs/adr/0056-data-catalog-core.md` 和 `0059-workarea-centered-data-management.md`。**只取对象模型和导入规则**——不迁移它的导航树/功能区/井位散点页/概览面板。
- 验收数据源：`/home/kevin/projects/paleo_project/data/project_area`（约 1.4 GB）。**禁止**把 966 MB 的 `200P_seismic.sgy` 和 8 个层位 .dat 提交进仓库；测试夹具从其中切：一条 inline 的 SEG-Y 切片、A1 的 LAS/TD、D61 栅格小样，放 `testdata/`。

【第一步：建 worktree】
```bash
cd /home/kevin/projects/paleo_workstation
git worktree add ../pw-data-foundation -b wave/data-foundation
cd ../pw-data-foundation   # 之后所有工作在此 worktree 内进行
```
注意主工作区可能有其他会话的未提交改动，不要动主工作区文件；只在你自己的 worktree 里干活。

【任务背景】
- 现状要改的行为在 plan §2：导入器把非 tif/img 都声明成矢量；`SegyReader::open` 对整个文件 `readAll()`；层位点可能成 26 万点矢量；JSON 里的 EPSG:4326 会写成图层 CRS。
- 现状可复用：`src/io/lasparser.{h,cpp}`（LAS 2.0）；`src/io/segyreader.{h,cpp}`（IBM 浮点解码保留，重写索引与按需解码）；`src/metadata/layermanifest`（只登记要画进 QGIS 的结果，不兼任文件目录）；`src/metadata/paleoprojectstore`；`src/ui/pages/pagepanels.cpp` 的 `DataPage` 目前只有导入按钮占位。
- 现有 `DataImportService::importFile(kind, path)` 接口要按新契约改造；外部调用点在 `src/ui/paleomainwindow.cpp`（importRequested → importFile）。

【交付物】

1. **局部测网 CRS**（plan §3）
   - 工作坐标 = 局部直角米。图层 CRS 用自定义工程坐标 PROJ `+proj=eqc +units=m +no_defs`，authid 不写 EPSG:4326。
   - 井保存 `surface_x/y` 与 `coordinate_status`（`ok`/`untransformed`/`invalid`/`missing`）；本工区 20 口井全为 `untransformed`。在出现真投影参数前不写 `project_x/y`，地图读 `surface_x/y`。

2. **数据模型 + catalog.json**
   - 实体（井=稳定 id+井名+UWI+别名；地震体=SeismicSurvey；层序界面；辅助实体）→ 显式关联 `entity_asset_links`（实体类型、实体 id、资产 id、角色、主版本、未决）→ 数据资产 → 不可变版本（RAW/DERIVED/INTERMEDIATE/OUTPUT）。
   - `catalog.json` 是唯一主存储和本阶段查询源。角色名用已有的：`well_head`、`well_log`、`tops`、`time_depth`、`horizon`、`seismic_volume`。
   - **字段契约已钉在 `src/services/projectdata.h`**（主工作区已有；若你的 worktree 里没有，从主仓原样拷入）。顶层 entities/assets/versions/entity_asset_links 及字段名逐字遵守——编图链路包按该契约读，你按它写。不要改该头文件。
   - 受管路径 `{stage}/{asset_id}/{version_id}/{filename}`；默认导入=受管 RAW：边复制边算 SHA-256，落盘只读。用户明确选择链接外部才不复制；SEG-Y 走外部链接。D61 装箱栅格是 DERIVED，父版本指向 RAW。

3. **分类器**（plan §3 表格）：井位/、井分层/、时深|td 段、层位/、`.las`、`.sgy|.segy`、`.geojson`、`.pdf/.ppt/.pptx/.doc/.docx`、`.png/.jpg/无地理变换的.tif`、`.xml` 再看内容（井口或测井，判不出作参考）。

4. **解析器**
   - `井位/ExportWellHead.dat`：井名、X、Y、KB、TD（20 口 A1–A20）。一份多井文件=一个资产+每井一条关联；预览按当前井过滤，不拆文件。
   - `井分层/DC.dat`：与 `parse_well_tops_text` 同列（井名、层名、MD、X、Y、Z、TVD、Time(ms)）；`#` 跳过；-99999 的 Time/TVD/MD 视为空。
   - `时深/TD/*.dat`：TIME(ms)、TVDSS、TVD、MD。
   - LAS 复用现有 lasparser。

5. **绑定规则**（plan §3）
   - 井名比较前去首尾空白、连字符、空格，忽略大小写。恰好一个匹配→挂接；零个→测井再按文件名主名（`A1.Las`→A1），分层/时深同理；仍零个或两个候选→`unresolved`，不新建井、不并井。
   - LAS 先读 `~W` 的 WELL；分层/时深先读文件里的井名列。
   - 层位文件名在 {C3,C6,D53,D61,D62,D63,D71,D72} 内→挂该界面；之外→建未决层位实体，不进编图 chip。

6. **D61.dat → 装箱时间栅格**（plan §3 精确参数）
   - RAW 登记层位文件；派生 DERIVED 栅格：网格 411×641；P1(inline 1315, xline 4165)=(0,0)，P2(1315,4805)=(12793,0)，P3(1725,4805)=(12793,16406)；X 随 crossline、Y 随 inline 增加；dx=12793/640，dy=16406/410；北向上 geotransform 原点 (0,16406)、行方向 -dy；空道 nodata=-9999；同像元多点保留最后一点并在栅格元数据记碰撞数。
   - 栅格登记进 LayerManifest（图层清单只管要画的结果）。

7. **SegyReader 重写为测线级**
   - 打开时从道头冻结 survey 角点、inline/crossline 范围、采样间隔、起始时间；建立 inline/crossline→文件偏移索引，不 readAll。
   - 提供单条 inline 或 crossline 解码（约 411/641 道，901 样点，2 ms）；补 crossline 道头（默认字节 193）。打开内存不随文件大小线性增长（测试断言）。

8. **数据页预览标签**（plan §4）
   - DataPage 加页内普通 `QTabWidget`（可关闭标签，样式走 DESIGN.md dock 面板，不用工作流标签蓝下划线）。建议新文件如 `src/ui/datapreview/`；pagepanels.cpp 只做最小接线改动。
   - 九类资产预览：well_log（单井曲线，默认 GR 可换 AC/DEN）、well_stratification（分层表，Time 空则空）、time_depth（TIME–TVD 曲线）、well_head（井名/X/Y/KB/TD/coordinate_status，选中时地图高亮该井）、horizon（网格尺寸、Z 单位范围、派生栅格状态+「在地图上显示」）、seismic（选一条 inline/crossline 解码显示）、image_reference（按面板宽缩放）、document（文件名/类型/「用系统程序打开」）、geojson（要素数/坐标范围/相名字段，未配准标明不进地图）。
   - 状态文案：空态「还没有打开的预览 — 在列表中选择一条数据」；读取中「正在读取」+文件名；失败给原因+文件名；外链源缺失「找不到源文件」+路径。重选已开资产聚焦已有标签。
   - 导入向导沿用 §42.7 五步；SMI 文本列固定展示识别结果、不让用户重排；确认入库后才开标签。

9. **阶段 D — 辅助资料**
   - PNG/PPTX/PDF 作 `document`/`image_reference` 受管入库，角色 `reference`，挂辅助实体。
   - 三份 GeoJSON 入库并标未配准、不生成地图图层；相/亚相/微相名称收成图例字典。
   - `参考资料/` HZ28-6-1 XML 固定为辅助参考实体，不按内容挂井、不并进 A1–A20；其他 XML 按内容区分井口或测井。

10. **时深工具**：TVD 对 TD 表 TVD 列线性插值得 TIME(ms)；井 TVD 空则用 MD 对 MD 列；-99999 不进插值。井在剖面的位置 = 离井口最近的层位采样点的 inline/crossline。

【验收 Oracle】（全部满足才算完成）
- `ninja -C build` 零错误；`QT_QPA_PLATFORM=offscreen ctest --output-on-failure` 全绿（含新增测试）。
- 新测试至少覆盖：分类逐类、单井匹配挂接、双候选→unresolved、多井 tops 文件→多条关联、文件名不在集合→未决层位、D61 geotransform 数值与 nodata、同像元碰撞计数、复制 LAS 的 SHA-256 与只读位、任意类型外链缺失、预览标签重选聚焦、SEG-Y 打开内存不随体增长、图层 authid 非 EPSG:4326、TD 插值（TVD 优先/MD 兜底/跳 -99999）。
- 手工核对：A1 落在 (5288.67, 8219.94) 容差半像元、压在 D61 栅格上。
- 打开 project_area 目录跑一次导入冒烟（真数据路径，不提交数据本身）。

【纪律】
- 不碰：`src/workflow/workflows.*`、`src/ui/pages/pagepanels.cpp` 里 Predict/Constraint/Compose/Validate 部分（另一并行包会动 ValidatePage 区域——pagepanels.cpp 只许为 DataPage 做最小增量）、`src/ui/paleomainwindow.cpp` 除 importRequested 接线外不动、不改 DESIGN.md 已定视觉。
- CMakeLists.txt：新增源文件/测试集中放在一个连续块里，方便另一包合并时解冲突。
- catalog.sqlite **不做**（TODOS P3 已递延）；多 realization、相界类型、体系域、砂地比/距井距离/TIN/等值线/屏障 IDW 不做（plan §6）。
- 完成后在 worktree 分支 `git add -A && git commit`，提交信息遵循仓库惯例（单行小写摘要+要点列表）。
- 合并：回主仓 `cd /home/kevin/projects/paleo_workstation && git merge wave/data-foundation`（非快进时解冲突），合并后主仓 `ninja -C build && QT_QPA_PLATFORM=offscreen ctest` 连跑两遍全绿才算交付。
- 最终报告：测试通过数、改动文件清单、与另一包（wave/mapping-pipeline）的接口约定（catalog.json 结构、实体/资产 id 规则、派生栅格的 LayerDeclaration 形式、TD/ tops 读取 API）、给集成者的注意事项。
