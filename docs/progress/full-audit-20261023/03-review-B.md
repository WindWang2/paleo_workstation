# Review B — 单因素 / 相分类 / 服务契约（full-audit-20261023）

- 基线：`d699694c..HEAD`（#630–#647；相分类与井层单因素的行为变更集中在 #637）
- 角色：只读深审。未改源码、未构建、未跑 ctest。
- 方法：指定增量文件逐行读完，再顺着 compose / 相目录加载 / 阈值校验 / 井点提取 / 综合井图分层显示追到未出现在 diff 里、但行为已变的函数。

#645 的症状只写在文末归属节，不占 Finding 编号。

## Findings

### B-01 / P2 / HIGH / 数据·井层单因素

- File:line：`src/services/projectdatafactors.cpp:14-30`（`ProjectDataFacade::importedWellFactors`）；接合在 `src/workflow/constraintwellfactors.cpp:183-187,226-241`；入库在 `src/io/dataimport_file.cpp:283`
- 函数：`importedWellFactors` → `sourceFor` → `extractWellFactorsImpl`
- 根因：井名只在「规范化后命中多于一口井」时拒绝。命中 0 口时既不写 `error`，也不丢弃该行，调用方用 `QHash::value` 取不到就当成「这口井没有导入行」。
- 触发条件：`*_单因素.csv` / `*_factors.csv` 已解析进版本 `extra.wellFactors`（解析器只拒空井名、重复井层和非法数，不对照目录），其中某井名经 `DataCatalog::normalizeWellName`（去空白/`-`/`_`、转小写）后仍对不上任何井实体。典型是笔误、UWI 与井名不一致、或中文井名差一个字。
- Observed：提取返回成功。对得上的井用文件数值；对不上的井不出现在错误里，`maintainedSand` / `maintainedThickness` 保持 false，继续用顶底分层和岩性词表推断砂厚/层厚。文件里那一行被静默丢掉。
- Expected：关联失败应失败或至少逐井告警，并且不得用推断值填上这份文件本应挡住的井。进度说明写的是「提取按井名/层位关联，拒绝歧义井名及冲突重复输入；空值阻止旧逻辑重新推断」。歧义（>1）和同行冲突确实会 `return {}`；零匹配比留空更危险——留空会挡住推断并带上 `quality`，写错井名却把推断放行。
- 调用链：`readWellFactorFile` 收行 → `dataimport_file.cpp` 写入 `version.extra["wellFactors"]`（不建井链接）→ `importedWellFactors` 只拦 `wellsMatchingName().size() > 1` → `sourceFor` 按规范化井名哈希接合 → `computeExtraction` 对未接合井走岩性/顶底。`tests/tst_factorworkflow.cpp:361` 只覆盖四口都对得上的井，覆盖不到这条。
- 排除比对：不是 #589/#593 的哨兵问题（空单元格在 `wellfactorfile.cpp:47-53` 是省略键，不是写成 0）。不是 #556 的 `qWarning` 吞错——这里根本没有错误对象。不是 #645 的井表行数/等值距症状。
- 修复策略：`wellsMatchingName` 为 0 时与 >1 同一口径拒绝，或返回未关联井名清单并由提取写入非贡献行的 `reason`，禁止这些井再走岩性推断。冲突比较保持整表失败即可。
- 回归测试：目录井 `A1`，CSV 井名 `A1-typo` 且 `sand_thick` 有值。期望 `importedWellFactors` 或 `extractWellFactors` 失败/告警，且 `A1` 的贡献值不得来自岩性推断。再保留一条「`A-1` 对 `A1`」确认规范化匹配仍然成功。

### B-02 / P2 / HIGH / 数据·综合井图分层显示

- File:line：`src/services/previewwelltops.cpp:42-78`（`PreviewDocService::wellTopsForLog` / 局部 `measuredDepth`）；反解在 `src/domain/deviationsurvey.cpp:337-415`（`WellDeviationSurvey::tvdToMd`）；画在 `src/ui/datapreview/datapreviewtabwelllog.cpp:170-178`
- 函数：`wellTopsForLog` → `trajectoryFor` → `tvdToMd` → `FormationInterval.topDepth`
- 根因：显示副本在「只有 TVD、MD 为 NaN」时直接采用 `tvdToMd` 的返回值，并写 `hasMd = true`。`tvdToMd` 对够不着的垂深不返回 NaN，而是返回轨迹上垂深最接近处的 MD（`deviationsurvey.cpp:413-415`）；井口以上还会按首站井斜外推（`:348-349`），轨迹以下按末站姿态外推（`:407-412`）。新函数自己的时深对照路径明确「超出范围不外推、不绘制」，测斜路径没有做对应的命中检查。
- 触发条件：井有测斜链接，分层只有 TVD（`WellTop.md` 为 NaN，`projectdata.cpp:253`），目标 TVD 不落在任一单调片段内。最窄的一种是轨迹垂深几乎不增加（水平井或首站就在井口且后续不再加深），`deepestMd` 停在首站 MD，首站 MD 又是 0，于是缺失 MD 被画成 0。更常见的是画在某个真实但错误的 MD 上。
- Observed：`record.md` 被写成最近轨迹点的 MD，`hasMd = true`。综合柱状图把该值当作顶界深度。告警却是「分层 TVD 已按测斜转换为显示 MD」，与时深对照越界时的「暂不绘制，不自动外推」不是同一标准。原始分层文件未被改写（这一点与头注释一致）。
- Expected：与同函数时深对照分支一致——反解未命中（`tvdAt(md)` 与目标 TVD 不一致，或落在表前/表后外推）则跳过该顶界并单独告警，不把最近 MD 或 0 标成已测 MD。直井等深（无测斜、无对照，`md = tvd`）可以保留，但必须继续带现在这条告警。
- 调用链：`datapreviewtabwelllog.cpp:172` `wellTopsForLog` → `projectdata.cpp:253` 缺 MD 变成 NaN → `previewwelltops.cpp:51` `survey->tvdToMd(tvd)` → `:68-72` 凡有限数都 `hasMd = true` → `:177` `fi.topDepth = record.md`。底界侧 `previewwelltops.cpp:74` 已把 `hasMd == false` 转成 NaN 再送进同一函数，所以底界记录的 0 默认值本身没有被当成深度；问题出在测斜反解的失败返回值。
- 排除比对：不是 #589 的三套空值哨兵，也不是 #593 的空棒 0.0。`WellTop` 缺测深是 NaN，`WellTopRecord` 缺测深是 `md = 0 && hasMd = false`，本函数在使用前做了转换（`:74`）。`pairs.prepend({0,0})`（`:36`）是井口几何原点，只服务对照内插，不是把缺 MD 换成 0。无测斜且无对照时 `md = tvd`（`:63`）并告警（`:48`），TVD 为 0 时 MD 为 0 是井口等深，不是哨兵误用。本条是新显示链把「最近 MD」当成换算成功。
- 修复策略：`tvdToMd` 之后用 `tvdAt` 核对，容差外不收录；表前/表后外推若产品上要保留，必须换成与时深越界相同的告警，且不要把 `hasMd` 设成「原始 MD 存在」。不要改 `tvdToMd` 的既有契约（其它调用方依赖最近 MD）。
- 回归测试：一口有测斜、分层只有 TVD 且 TVD 深于轨迹最深垂深的井，`wellTopsForLog` 不得输出该层，告警不得声称已转换。再加一口水平井（首站 MD = 0、垂深不增加），确认顶界不是 0。无测斜无对照的直井等深用例保持 `md == tvd` 且有告警。

## #645 归属

结论先说：**不是加载器把模板和层级重复计入。** compose 用的是层位已保存的相分类，没有保存时回落到全局默认库 `FaciesCatalog::defaults()` → `FaciesTaxonomy::library()`，只读 `facies-hierarchy.json`。额外模板不进入这条链。测试失败是「新工程默认 27 类」这条已落地的设计变更没有同步到仍按 7 类/6 个阈值写的测试和分相文案；井表三失败是另一条产品回归，不是测试期望过期。

### 相分类阈值 / 图例 / raster()：测试过期，外加产品契约没改完

目录计数（`d699694c` → HEAD）：

| 来源 | 旧 | 新 | 谁在用 |
|------|----|----|--------|
| `facies-hierarchy.json` = `library()` = `defaults()` | 7 | 27 | 无层位分类时的 compose、图例、`saveFacies` 首存 |
| `facies-templates-extra.json` | 36 | 64 | 只在 `templates()` 里追加 |
| `templates()` = library + extra | 43 | 91 | 图件归一化的第二轮匹配、分类维护下拉 |

编码无交集（hierarchy ∩ extra = ∅）。9/11/12/16/17 是从额外模板升进默认库，路径含义没改，只是 9/16/17 换了颜色/纹理。不是把同一条计了两次。`tst_faciescatalog.cpp` 已改成期望 27 和 91，并断言 `templates.mid(0, defaults.size()) == defaults`。进度说明 `docs/progress/unified-geology-import-20261022.md:15` 写明「新工程默认 27 个微相，模板共 91 条」。

compose 不看单因素自己的类别。`MappingWorkbench::facies`（`mappingworkbench.cpp:445-455`）无 `facies_schema-<层位>` 时返回 `defaults()`。`prepareCompose`（`mappingworkbench_compose.cpp:86`）把这份列表 `normalize` 后交给 `composeRaster`。阈值闸是 `thresholds.size() != codes.size() - 1`（`mappingartifactwriter.cpp:615-621`），`codes` 按 JSON 列表顺序，不是按编码数值排序。测试传入 6 个阈值 `{20,25,30,35,40,45}`，旧默认库 N = 7，N−1 = 6；现在 N = 27，N−1 = 26，于是 `tst_impactrecompute` / `tst_composeattribution` 在进入像元循环之前就被这条文案拒绝。重放侧 `composeattribution.cpp:456` 是同一口径，冻结参数对不上也会拒，但这次 CI 失败发生在首次 compose，不是重放损坏。

`tst_standardelements` 同根：`hierarchicalFaciesLegendFollowsRendererAndLayout` 对 `defaults()` 的每一类造一个面，再断言图例 7 条、符号 7、标题 3。现在 `defaults().size() == 27`，断言 27 vs 7。`rasterLegendAggregatesParentsAndUsesActualSolidColors` 把 27 个编码塞进 `raster(..., cols=7, rows=1)`，`mappingartifactwriter.cpp:114` 要求 `cells.size() == cols * rows`，27 ≠ 7，返回「预测网格为空或尺寸不匹配」。这是测试把宽度写死成 7，不是栅格写入器限 7 列。渲染器按传入 schema 出图例，没有把 27 条合成 7 条的逻辑错误。

已有测试证明引擎仍接受显式 7 类：`tst_mappingworkbench.cpp` / `tst_evidencee2e.cpp` 在 #637 后改成 `saveFacies(..., defaults().mid(0, 7))`，注释写明 six thresholds / seven explicit classes。失败的两套件没有先保存层位分类，所以吃到新的 27 类默认库。

因此：

- 测试过期：是。把期望改成 27，或像已改过的测试那样先 `saveFacies` 钉住前 7 类，compose/图例/raster 断言就会回到旧契约。
- 产品缺陷（同一症状，不另开 Finding）：分相 UI 仍写「例如 10, 25（3 类相）」（`mappingworkbenchpage_flow.cpp:63`），文档仍举三类相填两个阈值。新工程第一次 compose 还会把 27 类 `saveFacies` 进层位（`mappingworkbench_compose.cpp:142`，27 < 32 的上限，存得进）。之后连续单因素必须按这份混合环境列表（三角洲、湖泊、近岸水下扇、浅海）填 26 个严格递增阈值，界面不显示 N，也不显示阈值槽对应哪一类。按界面示例操作的用户会打出和 CI 相同的错误。这不是因子类别被额外模板撑大。

修复应二选一，并同时改测试：要么默认库保持 27，但分相框展示当前层位的 N 和类别顺序，占位符不再写 3 类；要么连续分相使用调用方显式提交的类别列表，不要在无分类时静默套用全部默认微相。不要把 `templates()` 的 91 条并进 `defaults()`。

### 井表 rowCount 0 / 等值距 20 vs 10：产品回归，不是测试过期

`tst_wellattributes` 的 3 个失败是同一条链，对应：

- `modeSwitchAwayAndBackRestoresSavedExtraction`：勾选砂厚后表应为 1 行，实际 0（测试名字是「切回」，但构造期已经把勾选消耗掉，死在切回之前的第一次恢复，`tst_wellattributes.cpp:655`）
- `factorRowSwitchRestoresPerFactorWellTable`：换行后表应为 2 行，实际 0
- `crossFactorSwitchDoesNotLeakContourSuggestion`：`(70-10)/6 = 10`，实际保持构造默认 20（`constraintpage_generate.cpp:240`）

根因在 #637 的页面默认值，不在提取算法：

1. 构造末尾无条件勾上第一行砂厚（`constraintpage.cpp:208`），此时层位下拉还是空的。
2. 勾选触发 `setWellFactorFields`。`changedFactor` 为真，但 `horizon` 为空，保存口径探测被跳过（`constraintpage_wellfactors.cpp:270-276`）。砂厚 `supportsAutoMode == true`，模式被设成「推荐地质口径（自动）」。
3. 测试随后 `setHorizons` / `setCurrentIndex(0)`。因素没变，`changedFactor` 为假，不再探测已保存的 `factorMode=direct`。模式停在 auto。
4. `restoreWellFactors` 用 `extractionParams` 全等匹配（`constraintwellfactors.cpp:577-638`），auto 对不上已保存的 direct，返回 false。表保持 `invalidateWellFactors` 清出的 0 行，提示停在「尚未提取」。
5. 测试再 `setCheckState(Checked)` 是空操作（已经勾着，不发 `itemChanged`），设计中的「勾选后恢复」不再发生。
6. 表里没有 10 和 70，`applySuggestedContourInterval` 的 `count < 2`，步长 0，spin 又不是 0，于是留在初值 20。这就是「20 vs 10」。

注释写明意图：「已有按字段口径保存的提取版本时沿用该口径（不让默认推荐口径把已保存成果藏起来）」（`constraintpage_wellfactors.cpp:267-268`）。实现只在因素变化时探测，而新构造在层位到达之前就把这次变化用掉了。生产路径同样是先构造页面、再 `setHorizons`，不是测试专有。用户不手动从「自动」改回「直接使用字段值」，已保存的 direct 提取就显示成尚未提取，等值距停在 20 m。

测试期望仍然是正确契约，不应放宽成 rowCount 0 或等值距 20。修复：层位从空变为有值时重新做一次 `savedFieldMode` 探测；或把构造期勾选挪到层位就绪之后。`isImplemented` 隐藏置信度不改变砂厚/砂地比的行号，不是这 3 个失败的原因。

## 必查结论（不另立 Finding）

### 单位、空值：没有静默成功

- 单位：交换格式没有单位列（`wellfactorfile.h:5-6`）。数值列要求有限、非负，`sand_ratio` / `coverage_ratio` ≤ 1。空单元格不写入键，不写成 0。文件里如果出现「m」「ft」这类文字，`toDouble` 失败，整文件拒绝。没有「读了单位列再忽略」的路径。英尺/厘米写成纯数字时会被当成米收下——这是格式本身不携带单位，不是解析器把单位转换失败后报成功。不单独立项。
- 空值：`tst_factorworkflow.cpp:367-393` 已锁住契约。`A1` 的砂厚 0 仍是贡献值 0；`A4` 砂厚/砂地比空、`quality=incomplete-source`，提取行无 `value`，原因包含该质量说明。`maintainedSand = !imported.isEmpty()` 会挡住岩性回填，与进度说明「空值阻止旧逻辑重新推断」一致。质量说明为空时原因串会被清掉，随后 `computeExtraction` 用「比值不是有限数值」兜底，井仍不贡献，不是静默成功。
- 井名：歧义与冲突会失败；零匹配静默，见 B-01。

### 深度哨兵：缺 MD 没有被当成 0，除 B-02 那条反解

- `WellTop` 缺 MD/TVD 是 NaN（`projectdata.h:62-63`，`projectdata.cpp:253-254`）。`wellTopsForLog` 用 `isfinite` 判断，不会把 NaN 当成 0。
- `WellTopRecord` 的 0 + `hasMd=false` 是既有哨兵（#589/#593 族）。底界进入换算前已改成 NaN（`previewwelltops.cpp:74`）。`previewWellBases` 用 `hasMd`/`hasTvd` 与当前顶界的有限性对照（`previewwellbases.cpp:52-54`），不会把缺 MD 的 0 拿去和当前顶界比。
- 无测斜、无时深对照：`md = tvd` 并告警，不是赋 0。时深对照越界且非等深：不绘制、不外推。
- 末层没有底界时，井道用下一顶界，最后一层底界为 NaN（`datapreviewtabwelllog.cpp:178`）。绘制端用 `bottomDepth < topDepth` 一类比较，NaN 比较为假，不会把底界画成 0。
- 受管 `.dat` 当前版本直接采用文件内底界，不再走 RAW 的「顶界是否被编辑过」守卫。顶界和底界读的是同一当前版本，这一支本身不制造 MD=0。父链走不到 RAW 电子表格时返回空且不告警；无底界是合法分层，不据此立项。

### 相目录

见 #645 节。`resolve("浊积水道")` 在 27 类默认库里因四条不同父级而失败，是故意的（`tst_faciescatalog` 已断言）。图件归一化先匹配工程/默认库，默认库里唯一的裸名（如「水下分流河道」）不会被额外模板里的扇/曲流河/辫状河变体撑成歧义。

## 覆盖清单

### 逐行

| 文件 | 方式 | 结论 |
|------|------|------|
| `src/services/paleomapfiles.cpp`（102）+ `.h` | 全文；对照 `projectsetupmaps.cpp` 的哈希键和 `paleomapimport.cpp:94` | 通过。预览与拷贝都走 `members()` 的 `absoluteFilePath`，当前调用方键一致。`expectedHashes` 缺件/变哈希会拒绝并 `removeRecursively`，目标目录是该版本暂存目录，不是工程根。`mkpath` 失败仍不写 `error`，与改前相同，不新开。非 shp 用 `rasterFamily` 是随主件收样式，预览与拷贝同一词表。 |
| `src/services/previewdoc.h:135-138` | 新声明及上下契约注释 | 头注释与「无资料则等深并告警、不改原始深度」一致；与 B-02 的测斜失败返回值不一致。其余 `previewdoc` 分片未逐行。 |
| `src/services/previewwellbases.cpp` + `.h` | 全文 | 见必查结论。SHA 双读、编辑过的顶界不套旧底界，只在 RAW 电子表格支。受管 `.dat` 支信任当前文件。 |
| `src/services/previewwelltops.cpp` | 全文 | B-02。等深与时深内插分支核对过，无除零（单调闸在前）。 |
| `src/services/projectdata.h` | 新声明 + `WellTop` 哨兵注释；其余方法接口级 | 缺测深是 NaN，与 B-02 的输入一致。 |
| `src/services/projectdatafactors.cpp` | 全文 | B-01。>1 与同行内容冲突会整表失败；零匹配不会。 |
| `src/services/singlefactordef.cpp` + `.h` | 全文 | `isImplemented` 只藏 `confidence`，与进度说明一致。不改变砂厚/砂地比行号，不是井表 CI 的根因。孔渗单位仍是「小数或 %」的既有文案，本窗未改语义。 |

### 调用链（行为已变、diff 里没有函数体）

| 符号 | 方式 | 结论 |
|------|------|------|
| `FaciesTaxonomy::library/templates` | `faciestaxonomy.cpp:49-60` 全文相关函数 | 只拼接两个 JSON，不交叉去重；编码无重叠，拼接正确。 |
| `FaciesCatalog::defaults` | `faciescatalog.cpp:112` | 等于 `library()`，27，不是 91。 |
| `MappingWorkbench::facies/saveFacies` | `mappingworkbench.cpp:437-503` | 无版本则默认库；首存 1–32。27 能存进。 |
| `prepareCompose` / `composeRaster` 阈值闸 | `mappingworkbench_compose.cpp:69-191`，`mappingartifactwriter.cpp:599-624` | N 来自层位分类，不是因子。见 #645。 |
| `thresholdsValid` | `composeattribution.cpp:447-461` | 与 compose 同口径，本次 CI 不是重放损坏。 |
| `readWellFactorFile` | 全文 | 空值省略，0 保留，无单位列。 |
| `sourceFor` 导入接合 | `constraintwellfactors.cpp:151-315` | B-01；空值挡推断与测试一致。 |
| `restoreWellFactors` / `extractionParams` | `constraintwellfactors.cpp:576-666` | 模式必须全等。见 #645 井表。 |
| `ConstraintPage` 构造勾选与模式探测 | `constraintpage.cpp:205-208`，`constraintpage_wellfactors.cpp:216-278,435-448`，`constraintpage_generate.cpp:234-246` | #645 产品回归。 |
| `tvdToMd` | `deviationsurvey.cpp:337-415` | 不可达返回最近 MD。B-02。 |
| `topsFromFile` | `projectdata.cpp:245-262` | 缺 MD → NaN。 |
| `datapreviewtabwelllog` 分层装载 | `:159-183` | 显示副本直接当顶界。 |
| `PaleoMapFacies::resolve` | `paleomapfacies.cpp:41-114` | 先工程/默认库，再模板。默认库内唯一裸名不被额外模板撑歧义。 |
| `facies-hierarchy.json` / `facies-templates-extra.json` | 计数、编码、路径对照旧版 | 见 #645。不是重复计入。 |

## 未核查区

- `composeRaster` 像元循环、`polygonize`、`replay` 在阈值闸之后的网格重放（本次 CI 在闸前失败，未逐像元复核）。
- `constraintwellfactors.cpp` 插值发布、距离变换、等厚分支（导入接合和恢复匹配之外）。
- 相图例渲染器实现（只核对了测试为何从 7 变成 27，没有逐节点核对 QGIS 图例模型）。
- `prepare_comp_data.py` 的区间并集/时深内插数值（进度说明声称已测，本轮未复算）。
- `previewdoc.cpp` / `previewdoc_xml.cpp` / `previewdoc_survey.cpp` 全文。
- Windows 下 `copyDataset` 路径大小写与 `removeRecursively`（本机无 Windows；Linux 调用方键一致）。
- #645 其余套件（导入版本计数、岩性道标题、assetpaging、onlinebasemap、wellcomposite）不在本任务文件面内，未归因。
