# Goal-Loop Ledger — 方向 78 mkproject 夹具工厂化

分支：goal/mkproject-fixtures-20261009（基线 origin/master f31ee461）
迭代上限：15 轮。

## 设计决策（无人授职权限内自行定案）

### D1 manifest schema（v1，声明式）

- 位置：`tools/reference/mkproject/manifest.schema.json`（JSON Schema 草 2020-12
  描述 + README 双口径文档）。
- manifest 实例字段：
  - `schema`: 1
  - `georeference`: 可选；**形状与 project.paleo 的 georeference 节完全一致**
    （paleoGeoreferenceToJson/FromJson 同一序列化），mkproject 用
    `paleoGeoreferenceFromJson` 直接解析——单一形状两个载体。
  - `imports`: 有序数组，元素 `{path, type?, convert?, linkExternal?}`。
    - `path` 相对 manifest 文件所在目录解析（绝对路径直用）。
    - `type` = DataImportService forceType（well_head/well_stratification/
      well_log/outsource_workbook/…），空 = 分类器自判（图片/SEG-Y/docx 走这条）。
    - `convert` ∈ {well_head, well_stratification}：输入是工作簿（.xlsx/.xml），
      先走竞赛同款 convertWellHeadXlsx/convertStratXlsx 转规范井文本再导入
      ——复用 readWorkbook 生产路径。
    - `linkExternal`: true → ImportOptions.linkExternal（外委工作簿外链口径）。
    - 井口先行契约由 manifest 排序承载（README 明示）。
  - `expect`: 可选自检期望（mkproject 第 5 步对拍）：
    `wells` / `entitiesByType{}` / `unresolvedLinks` / `georeferencedWells` /
    `seismicGeometry{inlineMin,inlineMax,xlineMin,xlineMax}`。只断言出现的键。
- `--manifest` 与 `--data` 互不依赖：manifest 模式不要求 --data（相对路径锚
  manifest 目录）；两 flag 同给时 manifest 路径仍锚 manifest 目录，--data 忽略
  （README 记明）。`--out` 缺省 = manifest 目录（就地工程语义，与竞赛模式对称）。

### D2 向后兼容红线（对拍口径）

- 结构：竞赛路径代码零改动（`--manifest` 分支在 dataRoot 检查**之前**早退）；
  唯一共享面 = 井名泄露兜底块原样提炼为
  `attachUnresolvedWellLogByStem()`（verbatim move，两路共用）。
- 行为对拍：竞赛数据本机不存在 → 造「竞赛形状树」（`make_compshape_tree.py`，
  与 mini 生成器同仓）：R0 mkproject（f31ee461）vs 改造后 mkproject 同树跑
  全链，stdout + 退出码逐字节一致才算零差异。

### D3 mini 数据集（tools/reference/mkproject/mini/）

- `generate.py`（stdlib-only，确定性）生成并入库提交：
  - 3 井（A1/A2/A3）× wellhead.dat（井名 X Y KB TD）；
  - 分层两源：tops.dat（规范 井名 层名 MD）+ tops_header.dat（'#' 头驱动列序
    ——覆盖 sniffTopsColumnMap 面）；
  - LAS×4（A1/A2/A3 + A3 第二份——多文件井面），每件 ~400 采样 ×
    DEPT/GR/DT/RHOB/NPHI，~W WELL 名 = 井名；
  - 微型 SEG-Y：subprocess 复用 tools/make_segy_fixture.py（道头约定单一
    真源），4 IL × 5 XL × 50 采样；
  - 岩心 JPG×3（base64 内嵌合法最小 JPEG + EOI 后缀字节区分哈希；深度锚在
    文件名）、岩屑 CSV×3（顶深/底深/岩性/描述）、薄片 JPG×1；
  - 参考文档 docx×1（复制 testdata/project_area/mini_report.docx）；
  - manifest.json（含 georeference：3 控制点精确 + 1 点微扰 ~3m —— 残差断言
    有物理意义）。
- 体量目标 ≤2MB、全链 ctest 秒级。

### D4 夹具工厂（tests/fixtures/mkprojectfixture.{h,cpp}）

- `// 层：测试壳` 头（tests/ 惯例，check_layering 不扫 tests）。
- QProcess 直跑 `$<TARGET_FILE:paleo_mkproject>`（编译定义 MKPROJECT_BIN），
  环境全继承（ctest ENVIRONMENT 的 XDG/QGIS_PREFIX_PATH + ps1 的树内 TEMP——
  沙箱监狱下子进程同监）。
- API：`buildMiniProject(outDir[, timeoutMs])` → RunResult{exitCode, stdout,
  projectDir, qgz/paleo 路径}；`openCatalogSummary(projectDir)` →
  {wells, entitiesByType, perWellRoleCounts, unresolvedLinks}（DataCatalog::open
  直读 artifacts/metadata/catalog.json）。

### D5 消费测试（tst_mkprojectfixture，3 面复用）

1. 导入全链：逐实体类型断言 catalog 登记（井数/角色计数/版本数/实体类型表）。
2. round-trip：qgz+project.paleo 双件在；重开摘要与首开一致。
3. 配准：project.paleo georeference 解析回读，maxResidualM ≤ 阈值
   （mini 生成器写死微扰上限 ~3.5m，断言口径 ≤5m 记此处）。
4. 复用性：同测试二连实例（两个 QTemporaryDir 各跑一次全链均成功）。
- 链接 paleo_services paleo_qgis（LABEL services，core 段，非 perf）。

### D6 沙箱/QProcess（R0 验证项）

- Windows localdeps：ps1 test 已把 TEMP/TMP 树内化 + QGIS_PREFIX_PATH 前置，
  QProcess 默认继承 systemEnvironment → 子进程同口径。若监狱挡子进程，
  按 SEISMIC_INDEX_CACHE_DIR 先例树内化（R0 实测后记账）。

## 迭代记录

| 轮 | 改动 | 验证 | 结果 | 下一步 |
|----|------|------|------|--------|
| 0 | 勘察 + R0 构建启动 | mkproject.cpp 619 行全读；测试布线/沙箱/角色词表/解析器格式确认；CMake 3.29 前置后 R0 构建跑起 | 进行中 | R0 红集合基线 |

| 1 | 批1-5 全部落码：mkproject --manifest（分支早退，竞赛体零改动+兜底块 verbatim 提炼）、schema、mini 生成器、夹具工厂、消费测试、CMake、README/devex | 手工烟测 manifest 全链 exit 0 全期望过；首轮发现三问题 | 通过（有修） | JPEG 换三真异色板、岩屑按井差异化、expect 补 auxiliary |
| 2 | mini 修正重跑 | 17 项全入库（dedup 清零）、cuttings 3、auxiliary 2、pyramid 告警 0、全部期望 OK、exit 0 | 通过 | R0 对拍 + ctest |

## 实测口径记录（首轮烟测发现）

- **`reference` 角色不在内置词表**：docx/外委 xml 挂井/辅助实体时 role=
  "reference"，catalog 诚实降级（记 invalidRoleLinks 诊断）——**竞赛路径同
  样行为**（既非本方向引入也非缺陷），stdout 会出现 4 条 `未知角色:
  reference`，对拍两侧一致。
- **重开 lastErrors 非致命**：`project member missing: areaRules/gpkg/
  manifest`（project_area.json 未建、gpkg/sqlite 惰性建）——openProject
  返回 true，竞赛口径同样打印后过。
- **沙箱监狱**：子进程 mkproject 手工直跑必须 TEMP/TMP 树内化（否则
  createProject「Unable to write temporary qgs file」）——paleo-dev test
  已内置，ctest 环境继承验证见 tst_mkprojectfixture.twoConsecutiveInstances。
- 残差断言口径（Oracle 3）：mini 生成器 A3 纬度微扰 3.0m → maxResidualM
  == 3.0（精确）+ 逐点 ≤5.0m 上限。

| 3 | layering 三档 + R0 真相勘察 | 三档全绿；发现「R0 全构建成功」是管道假绿（tail 吃退出码，实只出 21 exe）：master tip 本机缺 vendored onnxruntime 时 tst_mapping/tst_versions 等 LNK2019（paleo_ai 空壳，既有面，非本方向引入） | 部分通过 | 按钉版补 vendor/onnxruntime 1.30.0（sha256 校验过）后全量重建 |

## R0 基线口径修正（重要）

- 本 worktree 首次全量构建**实际失败**（`cmake --build | tail` 管道掩码）——
  master f31ee461 在本机 localdeps 无 vendor/onnxruntime 时链接不全：
  `paleo_ai` 退空壳 → `suggestHorizonTracking`/`runTileInference` 未解析 →
  tst_mapping/tst_versions 等 LNK1120。**与本方向改动无关**（diff 不触
  workflow/ai）。
- 处置：按 vendor/manifest.json 钉版下载 onnxruntime-win-x64-1.30.0.zip
  （sha256 c6ba983b…a949 校验一致）解包进 worktree vendor/onnxruntime
  （gitignored，不进提交）。R0/R1 对拍与 ctest 对照都在 ORT 在场的同一
  配置口径下做。
- 主 checkout 的 tst_mapping.exe 是旧混合链时代陈货（其 CMakeCache 同样
  ORT NOTFOUND）——不可作 R0 证据。

| 4 | ORT vendored 后全量构建破三关（paleo_ai 空壳链接断 → 定位 autogen 陈旧态 → 清 build/paleo_ai_autogen 生成侧后 620/620 全绿 357 exe）；新测试首跑 2 败（QgisRuntime 时序 + 中文断言编码）修为自定义 main 先例 + ASCII 口径；well_head 期望 3（多井单文件契约） | tst_mkprojectfixture 6.06s Passed；layering 三档绿 | 通过 | R0 对拍 |
| 5 | R0 对拍（stash 编原版 exe vs 改造版）：A 竞赛形状树全链 / B mini 目录 / C 无参数 三用例 | A: exit 1==1，掩码（目录名自引用+Qt 线程地址）后逐字节一致；B: exit 2==2 缺件报文一致；C: 用法一致 | 通过 | 全量 ctest R1 |

## R0 对拍证据口径（Oracle 1）

- 掩码归一：`sed s/compshape-out-<r0|new>/OUTDIR/g; s/0x[0-9a-f]*/0xADDR/g`
  后 `cmp` 逐字节一致。A 用例树 = make_compshape_tree.py 产物（20 井 xlsx/
  分层 per-井 sheet/21 LAS/头驱动 tops/SEG-Y/岩心薄片粒度岩屑/docx/外委
  xml 全布局），双二进制同树各跑，完整走到 5/5 自校验（「失败 4 项」=
  Ordos 特定期望对合成数据不匹配——两侧完全一致，exit 1==1）。
- B 用例证明「非竞赛树 + --data」错误路径零漂移；C 证明用法面零漂移。

## 覆盖增量清单（Oracle 4：io 解析路径 synthetic-only → 生产路径覆盖）

新测试（tst_mkprojectfixture → QProcess paleo_mkproject --manifest mini）
经 DataImportService 生产链触达的解析器（此前仅单测合成夹具/无覆盖）：

| 解析面 | 文件:行 | mini 驱动数据 |
|--------|---------|---------------|
| 井口文本（位置约定 井名 X Y KB TD） | src/io/wellfileparsers.cpp:169 | well/井位坐标.xml → convertWellHeadXlsx 转换产物 |
| 分层文本（位置约定 + '#' 头驱动列映射） | src/io/wellfileparsers.cpp:203（sniff 列映射 :119） | strat/tops.dat + tops_header.dat（列序打乱） |
| 工作簿（SpreadsheetML + OOXML 分派） | src/io/outsourceworkbook.cpp:714 | well/井位坐标.xml + outsource/wg1_well.xml（外委外链） |
| LAS 2.0 | src/io/lasparser.cpp（全文件） | las/{A1,A2,A3,A3_rerun}.las |
| SEG-Y（道头约定 + 几何冻结） | src/io/segyreader.cpp（SegyReader::open 全文件） | seis/mini.sgy |
| 岩屑表（CSV 方言 → WorkbookSheet） | src/io/cuttingsdoc.cpp:235 | 2.4岩屑录井数据/A{1,2,3}_岩屑录井.csv |
| 井附件角色词表（目录关键词） | src/io/dataimport_geodata.cpp:119-123 | 2.3岩心资料/2.6薄片/2.4岩屑 目录 |
| 井族导入（井建齐 + resolveWell + 主/副 LAS 槽位） | src/io/dataimport_wells.cpp:117 | 全部井数据 |

synthetic 面（PerfFixtures::makeSyntheticCatalogDir——直接灌 catalog，不经
io 解析）仍然服务于分页/性能测试；本方向补的是「生产导入路径」这条链。

| 6 | 第一轮五维自审修复：printf %1 伪格式化（manifest 计数被吞）、convert staging 同 basename 覆盖边界（按序号命名）、expect 循环重复计数、CMake 注册块排版；generate.py 头注同步 | 重建 535/535 零 FAILED；tst_mkprojectfixture 终态 Passed 8.54s；对拍 A/B 掩码后 md5 相同（2b448b8a…） | 通过 | 全量 R1 |
| 7 | 全量 ctest R1（build8 态）：370 项 349 绿 22 红（21 Failed + tst_crossplot_controller SegFault）；红集对照 R0：二进制恒等论证（mkproject.cpp 只进 paleo_mkproject 目标，tests/ 零引用——除新夹具）+ 抽样直跑定性（tst_fspath = sgyVolumeLoadFromChinesePath 本机 locale 面） | 红集 R1∖{新测试} = 红集 R0（按构建图恒等）；tst_mkprojectfixture 在全量中亦 Passed（6.55s） | 通过 | 二轮 review + 提交 |

## Oracle 验收对账（2026-10-08 终态）

1. **manifest 化** ✓：mini manifest 全链跑通（工程文件 + catalog 摘要 +
   expect 全 OK）；无 manifest 对拍三用例（全链树 exit 1==1 / mini 目录
   exit 2==2 / 用法 exit 2==2），掩码后逐字节一致，终态二进制重验
   （md5 2b448b8a…）。
2. **夹具工厂可复用** ✓：tst_mkprojectfixture.twoConsecutiveInstances
   （两连 QTemporaryDir + QProcess 全链均 exit 0）；ctest 沙箱 env 继承
   （XDG/QGIS_PREFIX_PATH），ps1 树内 TEMP 口径下子进程可用。
3. **消费测试** ✓：四断言面全绿（导入逐实体类型/角色计数/版本数 17、
   round-trip 摘要一致、配准 maxResidualM==3.0 + 逐点 ≤5.0m、两连实例）。
   阈值口径：≤5.0m 上限 + 3.0m 精确值（生成器 A3 微扰）。
4. **覆盖增量** ✓：见上表（8 个解析面，文件:行号对照）。
5. **体量** ✓：mini 目录 171 KiB（≤2MB）；夹具测试墙钟 6.06-8.54s
   （含两次 mkproject 全链 + 四断言面）。
6. **全量对照 + layering** ✓：R1 370 项，红 22（=R0 红集，二进制恒等
   论证 + 抽样定性：UI/中文路径/locale 环境面 + 1 个 SegFault 本机既有）；
   layering 三档绿（终态重验）。

## R1 红集清单（22，全部 R0 恒等）

boot, tst_aiassist_perf, tst_cache_las, tst_composeworkflow,
tst_crossplot_controller(SegFault), tst_dockmanager, tst_fspath,
tst_import, tst_mapping, tst_metastore, tst_panels, tst_perfbudget,
tst_runtime, tst_shell_projectlifecycle, tst_startup_trace,
tst_stratigraphicweb, tst_ui, tst_uxtheme, tst_wellcomposite_perf,
tst_wellfaciesconfig, tst_wellsection_ui, tst_workflows
