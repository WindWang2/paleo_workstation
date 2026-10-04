# Goal-Loop 方向 27 ledger：沉积相自动编图辅助链（facies-automapping）

分支 `goal/facies-automapping-20261004`（基于 origin/master bd1eb0a）。

## 批次 1：算法四核 + workflow 编排 + UI 面板 + 壳接线

### 交付物

| 阶段 | 文件 | 说明 |
|---|---|---|
| 1 优势相 | `src/algorithms/faciesmapping/dominantfacies.{h,cpp}` | 井相柱 → 层位×井优势相/频率/覆盖率；最上优先重叠吸收；置信加权得分；tie 如实标记 |
| 2 相界 | `src/algorithms/faciesmapping/candidateboundaries.{h,cpp}` | 域∪等值线 unaryUnion 节点化→polygonize→约束区求交；证据标记 `level:<L>#<seg>` + zone id；面积守恒残量（未定相）保留 |
| 3 合成 | `src/algorithms/faciesmapping/evidencesynthesis.{h,cpp}` | 多源加权点票；阈值拦截不赋相；无票约束继承（confidence=0 如实） |
| 4 QA | `src/algorithms/faciesmapping/faciesqa.{h,cpp}` | 五检测器：不闭合/重叠/孤岛（阈值可配）/穿越硬约束/缺井覆盖；逐条 regionIds+metric+定位点；同输入同输出 |
| 5 编排 | `src/workflow/faciesmappingworkflow.{h,cpp}` | JobRunner 三段式（prepare/compute/commit）；草稿 GPKG + QA 报告 JSON 双 DERIVED 资产（报告父版本=草稿版本）；权重/阈值+param_hash 持久化进版本 extra；declare `facies_draft.<h>` 进 05_PaleoMap；约束库装配（polygon→相区、break_line→硬线、域缺省=相区并集） |
| 3/4/5 壳 | `src/ui/faciesmapping/faciesmappingpanel.{h,cpp}` | 权重/阈值配置 + 进度 + QA 报告表（点击定位）+ 一键生成；只发意图 |
| 组装 | `paleomainwindow.{h,_attach.cpp}` / `appcontext.{h,cpp}` / `main.cpp` | attachFaciesMapping（幂等 dock）；assemble→startJob/同步兜底；rebind 同 catalog 实例纪律 |

### 测试

- `tst_faciesmapping_algorithms`（20 用例）：oracle 1 逐格频率/覆盖率断言；oracle 2 面积/顶点/证据标记断言；oracle 3 三类人工缺陷全检出可定位；合成权重翻转/阈值/继承。
- `tst_faciesmapping_workflow`（5 用例真实栈）：oracle 4 catalog 版本/SHA/受管路径/父版本链 + declare + organizer 落 `paleoHorizon=T1` 组 + GPKG 相码对账；权重持久化断言（同资产递增版本）；JobRunner 三段式；QA JSON 可解析；失败路径零副作用（不产游离图层）。

### 自审记录

- 轮 1（六维）：
  - 修复：qgs include 块重复粘贴（编辑事故）；`control.cancelled` 伪进度通道移除（取消走框架 CancelFn）；QA 类型 token 面板侧中文映射（i18n）。
  - 开发中修复的 GEOS 生命周期缺陷（自审重点项验证有效）：①`domainRingLines` 返回临时 vector 的 range-for 悬垂；②`noded/faces` if 块作用域过早析构致 `faceGeoms` 悬垂——均已改为具名持有至使用结束。
  - Low（记 PR body）：`DerivedAssetRegistrar::stage` 失败路径不清理预建 staging 目录（与既有 workflow 现状一致）；`geosutil makeCoordSeq` 在 createLinearRing 失败时的 seq 泄漏（既有 helper 形态）。
- 轮 2（六维）：见提交信息/PR。

### 遗留项（未做，交后续方向）

- 阶段 6 版本对比视图（两版相图叠加差异高亮）——oracle 未覆盖。
- 等值线输入的声明层自动装配（contours.<h>.<factor> → request.contours）：现仅约束库装配 + 调用方显式给。
- 井相柱从 wellcomposite 数据自动装配（现由调用方构造 WellFaciesColumn）。
- 优势相统计表作为独立属性表资产落库（现随 QA 报告 diagnostics/extra 计数落库）。

### Oracle 验收对照

1. ✅ 优势相统计逐格断言：`tst_faciesmapping_algorithms::dominantFrequenciesAndCoverage`（0.8 覆盖率/0.625-0.375 频率逐格）等 5 用例。
2. ✅ 相界几何正确性：`candidateRegionsSplitByContourAndZone`（面积 50/50、顶点 5、证据 `level:5#0` 完整）等 5 用例。
3. ✅ QA 召回：`qaDetectsUnclosedOverlapIsland`（三类人工缺陷全检出且 regionIds+metric+location 可定位）等 4 用例。
4. ✅ 草稿资产 catalog 版本/SHA + organizer 落组：`draftRegistersCatalogVersionsAndDeclaresLayer`（版本行+SHA+`paleoHorizon=T1` 组内图层断言）。
5. ✅ 全量绿：见批次测试记录（14+4 目标集 + 全量 ctest）。
