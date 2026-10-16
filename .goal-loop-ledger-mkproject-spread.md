# 方向 95 ledger：mkproject 夹具扩散（tst_import / tst_aiwiring / tst_io_workbook_edges）

- worktree：`.worktrees/mkproject-spread`，分支 `goal/mkproject-spread-20261010`
- 基线：origin/master `f474069b`（任务书行号口径 `32851a1e`；差异核对：
  mkprojectfixture 两件/tst_mkprojectfixture/mini 数据集在该基线下原样，
  tst_aiwiring 重绑段 L186-231、tst_import 结构（makeStack L71）未漂移）
- 约束回顾：fixture 是增广不是替换；单测试墙钟 ≤15s 红线（fixture 段 ≤15s）；
  RUN_SERIAL 标注遵守；零 src/ 改动。

## R0 迁移面评估（2026-10-10）

### 消费现状
方向 78 后消费计数 = 3 文件（mkprojectfixture.{h,cpp} + tst_mkprojectfixture）。
候选测试逐个盘点：

| 候选 | 现状（自建工程段） | 断言面 | 迁移设计 | 取舍 |
|------|--------------------|--------|----------|------|
| tst_import（3079 行） | `makeStack()`（L71）：QgisProjectService+LayerManifest+PaleoProjectStore+DataImportService 自建栈；每用例 QTemporaryDir 逐件导入 testdata/project_area 样本（真实文件、synthetic 摆位） | 单件导入的实体/链接/受管 RAW/SHA/只读位/版本/去重 | 新增 1 槽：buildMiniProject 一次 → 导入域不变量在整工程产物上断言（详见下） | **迁**（最高价值：断言面与 fixture 导入面重叠最大） |
| tst_aiwiring（239 行） | `chatToolCatalogBindsWithoutAssistWorkflow`/`chatToolCatalogRebindsOnProjectSwitch`（L161/L186）addEntity 自建 catalog（A:1 井 / B:2 井） | 装配/绑定/重绑语义（工具上下文井名实证） | 新增 1 槽：synthetic A → mini fixture 工程 B（真实生产 catalog）rebind → wells/summary 快照断言 | **迁**（重绑语义首次在真实工程产物上验证） |
| tst_io_workbook_edges（342 行） | synthetic WorkbookSheet 直调 + io_robustness 边界件 + 本机绝对路径可选真件（QSKIP） | 工作簿/岩屑解析边界 | 新增 2 槽：mini 井位坐标.xml（readWorkbook 对拍 generate.py 单元格）+ A{1,2,3}_岩屑录井.csv（readCuttingsFile 对拍） | **迁**（零墙钟成本：直读仓内文件，无 QProcess） |
| tst_mappingworkbench（1284 行） | WorkflowFixture（helpers/workflowfixture.h）自建 | UI/工作流行为 | — | **不迁**：断言面是 UI 面行为，WorkflowFixture 注入的是领域对象而非工程目录；换 mkproject 夹具需 QGIS 全栈 + 双工程构建（≥14s）买不到对拍面（其井数据用 ComprehensiveWellData 内存构造，不经 catalog 读回） |
| tst_wellattributes（338 行） | 同上 WorkflowFixture | 井属性面板/存储行为 | — | **不迁**（同上；R0 定案） |

### tst_import 新槽设计（双轨增广——原 59 槽零改动）
`miniFixtureProjectMeetsImportDomainInvariants`：
1. `MkProjectFixture::buildMiniProject`（QProcess 全链，~6-8.5s）；
2. 整工程 17 资产逐件断言（tst_import 独有、tst_mkprojectfixture 未盖的面）：
   - 每资产恰一 RAW 版本；
   - 受管 14 件 verbatim：`DataCatalog::resolvedVersionPath` 在盘、
     SHA-256 == 源 mini 文件（sha256OfFile 对拍）、写权限位为零（只读锁）；
   - 井口 convert 件 conv0.dat：受管在盘 + RAW（staging 源已随进程销毁，
     SHA 无源可拍——断言差异如实记录）；
   - 外链 2 件：**mini.sgy（实勘修正：seismic 类型一律外链，
     dataimport_file.cpp:271 `options.linkExternal || cls.type=="seismic"`，
     manifest 未标 linkExternal 也是外链）** + wg1_well.xml（manifest
     linkExternal）——!managed、sha256 == 入库时源件、path 指回源；
   - WellLogSet 并集可达（方向44 面）：A3 双 LAS（A3.las + A3_rerun.las）
     两文件、曲线并集 8 列（主文件 canonical ×4 + 副文件 @basename 别名
     ×4）、深度索引列不进并集；
3. CMake：tst_import 追加 fixtures/mkprojectfixture.cpp 第二源 +
   MKPROJECT_BIN/MKPROJECT_MINI_DIR 定义 + add_dependencies(paleo_mkproject) +
   RUN_SERIAL TRUE（QProcess+QGIS 子进程资源面，对齐 tst_mkprojectfixture）。

### tst_aiwiring 新槽设计
`chatToolRebindsOntoMkprojectFixtureProject`：
1. synthetic A（1 井，现口径）bind → query（快照基线）；
2. buildMiniProject → DataCatalog::open（真实工程）→ 同一 controller rebind →
   query wells：count 3、井名 A1/A2/A3（生产导入产物读回）；
   query summary：well 3 / seismic_survey 1 / auxiliary 2；
   systemPrompt 摘要常驻段「井 3 口」（A 是 1 口，可区分）；
3. CMake：cmake/extra-ai-chat.cmake 的 tst_aiwiring 追加同款第二源/定义/
   RUN_SERIAL（add_paleo_test 注册后 target_sources 补源，保持并行方向零破坏）。

### tst_io_workbook_edges 新槽设计（零 QProcess）
- `miniWellHeadSpreadsheetMlMatchesGenerator`：readWorkbook(mini/well/井位坐标.xml)
  → format=spreadsheetml、单 sheet、表头 5 列、3 行井（A1 1010/5020/1048.5/1820
  等——与 generate.py 写入单元格逐一相等）；
- `miniCuttingsCsvsMatchGenerator`：readCuttingsFile(A{1,2,3}_岩屑录井.csv) →
  各 4 段区间、顶深逐井 +12.5m 偏移（A1 1500 / A2 1512.5 / A3 1525）、
  litho 序列与 generate.py 字面一致、issues 空；
- CMake：MKPROJECT_MINI_DIR 编译定义（无 fixture 链接——paleo_io 瘦链接面保持）。

### 生产路径触达增量（Oracle 2 预登记，实施后核实）
- readWorkbook（SpreadsheetML 路径）：tst_io_workbook_edges 新增实文件对拍
  （原仅边界件 + tst_io_outsource 竞赛样本）；
- readCuttingsFile（岩屑 CSV 全链 parseTextCuttings+parseCuttingsSheet）：
  同上（原有合成直调）；
- DataCatalog::resolvedVersionPath / sha256 校验面：tst_import 新槽（原有
  synthetic 单件）；
- WellLogSet::wellLogFiles/wellCurveIndex：tst_import 新槽（原有 synthetic 栈）；
- AiChatToolRunner query_project（wells/summary）：tst_aiwiring 新槽（原有
  synthetic catalog）。

### 墙钟预算（迁移前基线，实测后回填）
- tst_import 前：待测；后：前 + ~7s（单次 mkproject）
- tst_aiwiring 前：待测；后：前 + ~7s
- tst_io_workbook_edges：+0（文件读，毫秒级）
- RUN_SERIAL 影响：tst_import/tst_aiwiring 变串行（与 tst_mkprojectfixture/
  tst_perfbudget 同先例）。

## 执行记录

### E1 环境债实勘（2026-10-10，构建期发现）
- **master 在本机 no-ORT 配置下断链（既有债，非本方向引入）**：
  `aiassistworkflow.cpp`（paleo_workflow 无条件成员）无条件调用
  `runTileInference`/`suggestHorizonTracking`（仅 PALEO_HAVE_ORT=TRUE 时
  编进 paleo_ai）。本机主仓与各 worktree 的 localdeps 配置
  `ORT_INCLUDE=NOTFOUND` → 任何链接 paleo_workflow 的目标 LNK2019。
  主仓 build 的 tst_aiwiring.exe 是 10/6 陈旧产物（10/10 重建的
  paleo_workflow.lib 已无法满足其链接）。
- **处置**：按 BUILDING.md「独立 worktree 开发」共享口径，worktree
  `vendor/onnxruntime` 以 junction 指向 `.worktrees/sf-geostat/vendor/
  onnxruntime`（实体 ORT 1.30 drop；只读共享不清理不重建）→
  reconfigure 后 PALEO_HAVE_ORT=TRUE 全链可编。
- ps1 侧已知坑两则（记录备查）：Windows PowerShell 5.1 解析无 BOM
  UTF-8 中文注释报解析错——须 pwsh 7；VS 自带 cmake 3.27 低于工程要求
  3.28——PATH 须前置 `/c/Qt/Tools/CMake_64/bin`（3.29.3）。

### E2 迁移实施与首轮验证（2026-10-10）
- 三槽落地（diff 证据见 PR）：
  - `tst_import::miniFixtureProjectMeetsImportDomainInvariants`（+126 行；
    原 59 槽零改动）；
  - `tst_aiwiring::chatToolRebindsOntoMkprojectFixtureProject`（+82 行；
    queryWells 重构为 queryProject(topic) 泛化 + wells wrapper——既有两槽
    调用面字节等价）；
  - `tst_io_workbook_edges::{miniWellHeadSpreadsheetMlMatchesGenerator,
    miniCuttingsCsvsMatchGenerator}`（+90 行；零 QProcess 直读）。
- CMake：tst_import/tst_aiwiring 补 fixtures 第二源 + MKPROJECT_BIN/
  MKPROJECT_MINI_DIR + add_dependencies(paleo_mkproject) + RUN_SERIAL TRUE；
  tst_io_workbook_edges 仅 MKPROJECT_MINI_DIR 定义（paleo_io 瘦链接面保持）。
- **实勘修正 1**：mini.sgy 是外链不是受管副本——dataimport_file.cpp:271
  `options.linkExternal || cls.type=="seismic"`（seismic 类型一律外链）。
  新槽分组按 14 verbatim 受管 + conv0 + 2 外链断言。
- **实勘修正 2**：QVERIFY2 宏展开含 `return;`——不能用于带返回值的
  lambda（MSVC C3487/C2562）；`WellLogWarnings` 是全局作用域非
  WellLogSet 成员。两处编译红已修。
- **首跑抖动（如实记录）**：ps1 首轮 trio ctest 中 tst_import 判红一次
  （614.54s，QTest 输出 59 过 0 挂——ctest 判红细节被 ps1 直跑复核覆盖，
  直跑亦全过 EXIT=0）。此后三次复跑全绿：ctest 单项 60.92s Passed、
  ctest trio -j8 100%（87.48s）、ps1 直跑 EXIT=0。未复现，判为首轮
  资源装配抖动；CI 再红可追。

### E3 stash 栈共享事故与修复（2026-10-10，执行纪律记录）
- **事故**：为跑基线对照 `git stash push` 后，误用 `git stash pop`（无 ref）
  ——pop 到栈顶的 **tree-roles-incremental 会话的 stash「dir92」**（stash
  栈全 worktree 共享），其 untracked 4 件（catalogroles.{h,cpp} /
  tst_catalogroles.cpp / 其 ledger）落进本 worktree。
- **修复**：tracked 污染（Testing/Temporary/LastTest.log）checkout 还原；
  4 untracked 移 /tmp/tree-roles-rescue/ 保险副本；按 ref `git stash pop
  stash@{1}` 取回本方向 5 文件。tree-roles 的 stash@{0} 完整保留在栈中
  （其 pop 时会再生 untracked 文件；/tmp 副本留作对账）。
- **教训（写进本 ledger 供后续方向）**：多 worktree 并行会话共用 stash
  栈——pop 必须带 ref，先 `git stash list` 核对归属。

### E4 墙钟对比（前/后，ctest 口径，-j8）
| 测试 | 基线 | 改动后 | 增量 | 备注 |
|------|------|--------|------|------|
| tst_io_workbook_edges | 0.11s | 0.08s | ~0（噪声级） | 直读仓内文件 |
| tst_aiwiring | 0.42s | 3.72s | +3.3s | 单次 mkproject 子进程 |
| tst_import | 50.97s | 60.92s | +9.95s | 单次 mkproject 子进程 + 17 资产逐件 SHA |
- 新槽单跑墙钟（QTest 口径）：import 新槽 2.94s；aiwiring 新槽 ~3s。
  ≤15s 红线全部满足；RUN_SERIAL 两测试已标（ctest 标签 io/app）。

### E6 全量对照结论（Oracle 5，2026-10-10）
- 基线全量（stash 后重建，ps1 test 跑法）：93%±，19 红（E5 名单）。
- 改动后全量（native ctest -j8，ps1 直跑复核因 crossplot SegFault 触发
  crash-class 提前中断不可用，红集合从 LastTestsFailed.log 取齐）：93%，25 红。
- **红集合漂移逐项定性（19 → 25，新增 8）**：
  - 隔离复跑回绿（负载抖动）：tst_perf_catalog、tst_perf_regress、
    tst_ui（trio 组合下抖）、tst_perf_segyindex、tst_seismic_sectionui
    （安静复跑回绿）；
  - ctest 下持续红但**基线代码同样红**（判别实验：stash 回基线重建后
    ctest 复跑三连红——环境持续因素，与本方向零代码关联）：
    tst_correlation_full（50 井渲染 11.6s>3s 预算；裸 env 直跑 16/16 绿）、
    tst_previewmap_perf（首开 1090ms>900ms）、tst_correlation_async
    （心跳 gap 264ms>200ms）——全部为 UI 渲染/心跳墙钟预算类；
  - 反向漂移（基线红→改动后绿，同为抖动生态）：tst_officepreview、
    tst_seismic_budgets。
- **结论**：新增红无一是断言/逻辑红，无一项与本方向改动存在依赖关系
  （判别实验实证）；本方向触及的 4 测试（含 tst_mkprojectfixture）基线
  绿 → 改动后绿（最终验证 4/4 Passed）。「本方向引入的新红 = ∅」成立；
  环境噪声双向漂移如实记档（本机 perf 抖动生态，历史口径见方向 81
  环境债红名单）。
- 基线轮红集合本身是单样本（一次全量），correlation 三项在基线轮的绿
  属偶然（判别实验里基线代码同红）——后续方向对照时引以为鉴。

### E7 最终验证（2026-10-10）
- trio + fixture 四件 ctest：100% Passed（tst_import 300.63s 为连跑
  冷启值，干净单跑 60.92s 见 E4；tst_aiwiring 6.72s；
  tst_io_workbook_edges 0.66s；tst_mkprojectfixture 31.50s）。
- layering 三档：100%（E5）。
- RUN_SERIAL 注册核验：tst_import/tst_aiwiring 均为 TRUE
  （CTestTestfile.cmake 实查）；tst_io_workbook_edges 无 QProcess 不需要。

## Oracle 验收对账
1. **迁移执行**：3 测试指定段改吃 mini 夹具（tst_import 整工程不变量槽 /
   tst_aiwiring 重绑槽 / tst_io_workbook_edges 对拍双槽）——diff 证据见
   PR；迁移清单与 ledger R0 一致（mappingworkbench/wellattributes R0
   定案不迁，理由在案）。✓
2. **覆盖增量**（生产路径触达清单，文件对照）：
   - `readWorkbook`（SpreadsheetML 分派 + 单元格规范化）：tst_io_workbook_
     edges 新槽实文件对拍（原仅 io_robustness 边界件）；
   - `readCuttingsFile`/`parseTextCuttings`/`parseCuttingsSheet` 全链：
     同上（原仅合成 WorkbookSheet 直调）；
   - `DataCatalog::resolvedVersionPath` + 受管 SHA/只读锁 + 外链 sha 冻结：
     tst_import 新槽（原 synthetic 单件面 → 整工程 17 资产面）；
   - `WellLogSet::wellLogFiles`/`wellCurveIndex`（跨文件并集 + @basename
     别名协议）：tst_import 新槽（原 synthetic 栈 → 生产导入产物）；
   - `AiChatToolRunner::query_project`（wells/summary 主题）+
     `bindChatToolRunner` 重绑：tst_aiwiring 新槽（原 synthetic catalog →
     生产工程 catalog）。✓
3. **墙钟**：E4 表（io +0 / aiwiring +3.3s / import +9.95s；新槽单跑
   2.9s ≪15s 红线）；RUN_SERIAL 合规（E7）。✓
4. **断言保留**：三文件均为纯增广——tst_import 原 59 槽零改动（+1）；
   tst_aiwiring 原 6 槽保留（queryWells 经 wrapper 字节等价，+1）；
   tst_io_workbook_edges 原 14 槽保留（+2）。零断言删除。✓
5. **全量对照**：E6——本方向引入新红 = ∅；layering 三档绿。✓


