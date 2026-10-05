# Ledger — goal/realization-20261004（方向 47：多 realization / 不确定性）

工作树：`.worktrees/realization`，基线 origin/master。
协议：每轮原子提交（中文 conventional 前缀）；oracle 每条给证据；
未跑完的不记通过；五维自审（分层/schema 契约/统计口径/provenance/i18n）。

## 轮0 勘察定案（2026-10-04）

- realization 与 version 正交关系决议：**成员 = 同一集合资产名下的
  DERIVED 版本**，不是版本序列。`realization_id` 预留列不启用——契约
  物化为 `versions.extra_json` 键族（`realizationSetId` /
  `realizationIndex` / `realizationMemberCount` / `realizationSeed`），
  `schema_epoch` 维持 1 无迁移。「缺键 = 非成员」前后向兼容。
  契约本文：`src/catalog/realizationset.h` 头注；迁移注记落
  `docs/SCHEMA_MIGRATION.md` §8 末条。
- 首个填充源 = 既有 `geostat/sgs.h` 种子化多实现（共享随机流，
  成员间靠 index 区分取样序——`realizationSeed` 记基种子不逐成员记）。
- 统计面/差值面 = 独立 `realization_stat` / `realization_diff` 资产的
  DERIVED 版本；统计面 parents 锚全部在场成员，差值面 parents 锚两侧
  均值面版本。
- 分层：契约/查询归 `src/catalog`（paleo_store），逐像元统计核归
  `src/algorithms`（纯数值无 Qt），编排归 `src/workflow`，面板/图签/
  树归 `src/ui`，壳接线在 `paleomainwindow_attach.cpp`。

## 批1 catalog 契约 + 批2 统计核（提交 75e4466 / 6cde0d0）

- `src/catalog/realizationset.{h,cpp}`：enumerateSets / setById /
  memberVersionId / statSurfaces / meanSurfaceVersionId /
  memberRasterPaths / statisticDisplayLabel。
- `src/algorithms/ensemblestats.{h,cpp}`：均值 + 总体标准差（÷N）+
  P10/P90（type-7 线性插值 h=q(n-1)）；NaN 逐像元跳过；0 成员 → NaN；
  StreamingMoments O(cells) 内存；quantilesBanded 按行带处理不全体驻留。
- 测试：`tst_realizationset`（10 例）/`tst_ensemblestats`（9 例）。
- 自审修复：集合标题成员号与缺号报告的 0 基口径统一（首轮 1 基标题
  与 missingIndices 的 0 基不一致——低severity，已修）；统计/差值
  图层声明补 horizon 继承（首轮漏传——低severity，已修）。

## 批3 workflow 编排 + SGS 持久化（提交 cc08416）

- `src/workflow/realizationworkflow.{h,cpp}`：publishSet（成员栅格收编
  为集合 DERIVED 版本 + 图层声明 `realset.<setId>.m<index>`）/
  deriveStatistics（≥2 成员才做，逐口径落 realization_stat 版本 +
  `realset.<setId>.stat.<token>` 图层）/ differenceOfMeans
  （realization_diff，`realsetdiff.<A>.<B>`）。
- `src/workflow/constraintfactorjobs.cpp`：SGS 计算段把各实现写成员
  栅格，发布段经 RealizationWorkflow 收编为集合；`persistRealizations`
  参数关闭时保持均值+标准差旁路旧形态。
- 派生产物全走 DerivedAssetRegistrar 受管路径（artifacts/derived/…），
  SHA-256 记档、只读提交、无 QDir::temp 兜底。
- 测试：`tst_realizationworkflow`（12 例）——发布 round-trip / 空集拒绝
  / 单成员拒派生 / 像素真值统计 / provenance 覆盖 / 重派生幂等 /
  损坏成员诚实失败 / 网格不一致拒绝 / 差值正确性 / 帧序 / 重启存活。

## 批4 UI（本批）

- `src/ui/realization/realizationpanel.{h,cpp}`：集合选择 / 成员切换
  （下拉+滑块同一份当前态）/ 统计面列表 / 派生按钮 / 两集合对比 /
  成员动画（QTimer tick，帧序 = index 升序、缺号跳过）。全 intent
  信号；面板对 catalog 只读查询。
- `src/ui/decorations/paleodecorations.{h,cpp}`：不确定性图签（右上卡，
  DESIGN 图例位；title=口径词、subtitle=在场/缺号）。相图签同位时
  管理器逐帧下压让位（topMargin）。
- `src/ui/pages/datalist.cpp`：「不确定性集合」树分支——父集合→成员
  （缺号位禁选如名列）+ 统计面叶；版本行 memberNote 标签
  （versiondialog.h 表列内联显示）。
- `src/ui/pages/constraintpage.cpp`：SGS「保留实现集合」勾选
  （params.persistRealizations）；集合查看段默认收起挂页尾。
- 壳接线（paleomainwindow_attach.cpp）：RealizationPanel catalog 换绑
  （projectOpened）；member/stat/diff/derive intent →
  showRealizationMember/Stat/Layer（同集合成员互斥、同集合统计互斥、
  差值独立可叠）；图签随集合面上图/清除；datalist 树信号同通道。
- 测试：`tst_realizationpanel`（7 例）——空态 / 切换 intent /
  缺号诚实行 / 统计 intent / 单成员禁派生 / 动画帧序不重复发射 /
  对比守卫。
- 自审修复：不确定性图签与相图签同占右上 y=16 冲突 → 图签在场时
  逐帧让位（已修）。

## Oracle 验收映射

1. 契约寻址 round-trip + 旧库兼容 → `tst_realizationset`
   （membersRoundTripAndAddressing / legacyCatalogWithoutKeysOpensClean）。
2. 派生面逐像素收敛真值 → `tst_ensemblestats` syntheticTruth* +
   `tst_realizationworkflow` 像素真值用例。
3. 成员切换/缺号如实标/分组树 → `tst_realizationpanel` +
   datalist 集合分支（树节点 = enumerateSets 直读）。
4. 差值逐像素一致 + 帧序 → `tst_realizationworkflow`
   （differenceOfMeans 直算对照 / memberFrameOrder）+
   `tst_realizationpanel::animationTicksInFrameOrder`。
5. provenance：统计面 parents = 全部在场成员（workflow 测试断言）；
   口径词 statisticDisplayLabel 单源（图签/版本表/树/面板逐字一致，
   `tst_realizationset::statisticLabelsStable` 钉死文案）。
6. 空态/退化：N=1「单成员·无不确定性」禁派生；零成员集合不入枚举
   不显示假面（两个测试文件均覆盖）。

## 验证命令与结果

- `cmake --build build -j8` → 全绿。
- `ctest -R "realization|ensemblestats|layering|decorations|datalist|
   panels|catalog|geostat" -j8` → 22/22 通过
   （含 layering / layering_strict / layering_selftest）。
- 相邻回归：tst_panels、tst_decorations、tst_catalog*、geostat 全套绿。

## 已知 Low（入 PR body）

- 差值面（realization_diff）不进集合树分支——两集合共有物，挂任一
  单侧都会误导归属；列表视图仍可见，版本表可查 provenance。
- 成员号显示与 index 同基（0 起）——地质用户预期 1 起，但契约的
  missingIndices/版本表全用 index，0 基保证「缺 #2」与缺号报告逐字
  对应；待用户反馈再评估展示层 +1。
