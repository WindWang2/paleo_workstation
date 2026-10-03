# 方向 28 — 层序地层格架工作台（格架先行）

分支 `goal/sequence-framework-20261004`（基线 `origin/master` `bd1eb0a`），逐条证据见
`.goal-loop-ledger-sequence-framework.md`。

## 为什么要先有格架

编图正确性的前提是「格架先行」：先有层序→体系域二级格架与标志层，再把层位归属钉上去，
最后才谈编图。此前层位序只有 `mappingHorizons()` 一条名单，单元划分与标志层归属全靠人脑，
悬空引用、格架空洞、跨单元重名都无从机械检出。

## 落在哪里

| 目标 | 位置 | 说明 |
|---|---|---|
| 格架树管理 | `src/domain/sequenceframework.{h,cpp}` | 层序→体系域二级模型 + 标志层，纯值类型 + JSON；与 `mappingHorizons()` 双向解析 |
| 落库 | `src/catalog/frameworkstore.{h,cpp}` | 资产类型 `sequence_framework`，`stage=OUTPUT` 受管版本；归属走 `EntityAssetLink`（`sequence_boundary` / `framework_unit`） |
| 角色词表 | `src/catalog/roleregistry.cpp` | 新增 `sequence_boundary` 段 `framework_unit`（建议阶段 OUTPUT）。既有 `sequence_boundary` 实体语义与导入期行为一行未动 |
| 标志层管理 | 同上 + 诊断 | 标志层绑定格架单元，引用 `well_stratification` 分层名，悬空即报 |
| 井间建议 | `src/algorithms/frameworksuggester.{h,cpp}` | 区间最近邻；**只出候选**，唯一写口在 services |
| 编图单元校验 | `src/domain/frameworkdiagnostics.{h,cpp}` | 九种诊断 + 可复现报告（text/markdown） |
| 柱状视图 | `src/ui/sequenceframeworkcolumn.{h,cpp}` | 层级色带，带高按厚度；与树双向高亮 |
| 面板 | `src/ui/sequenceframeworkpanel.{h,cpp}` | 格架树 CRUD/排序 + 标志层/建议/诊断三页签 + 导出 |
| 编排 | `src/services/frameworkservice.{h,cpp}` | 数据层里唯一同时看得见 `paleo_store` 与 `paleo_algorithms` 的位置（护栏禁 `ui→algorithms`） |

## 三条硬约束

1. **`sequence_boundary` 既有语义不破坏**：只在其上新增挂接角色，实体 id 形态（`sb-<NAME>`）、
   `extra["pending"]`、导入期建实体逻辑均未改。`tst_roles` 的「无词表 → 空集」断言因此改为
   钉住新词表段本身（同一 commit 内一并更新）。
2. **格架数据走 catalog 实体 + 版本**：每次保存涨一个受管版本（父版本链 + SHA-256），禁止旁路
   存储。版本没入库时把刚写的受管文件就地删掉，不留「字节在、元数据不在」的半截状态。
3. **建议只出候选**：`suggest()` 是纯函数（不接 catalog、不接文件）；未确认前 catalog 零写入
   由 `tst_sequenceframework` 用 `mutationSeq` + 版本数双断言钉住；全未确认时幂等不落盘。

## 关键裁决：建议器为什么用「区间」而不是「顶深均值」

第一版按成员顶深均值 + 离散度归一化做最近邻，实测 distance ≈ 9.5 ≫ 阈值 1.0，一条候选都出不来：
单元内不同成员本就跨越单元厚度（百米级），而同一层在各井之间的构造起伏只有十米级，按成员离散度
归一化会把单元内的正常成员判成超阈值。改为区间法（落在已确认深度区间内 = 距离 0），并优先用
**本井**区间（该井有该单元成员时），否则退回全工区均值。

## 门禁与测试

```
tst_sequenceframework        7 passed   CRUD round-trip / 版本管线 / 诊断全检出 / 建议零写入 / 报告可复现
tst_sequenceframework_ui     6 passed   面板 CRUD+排序 / 柱状联动 / 建议需确认 / 诊断导出
tst_roles · tst_catalog · tst_constraintstore · tst_arearules · tst_workflows   全绿
layering ×3 · ui_invariants ×3 · i18n ×2 · git diff --check                     全绿
```

## 已知遗留

- 归属链接只补不删（`DataCatalog` 无 removeLink 面）：单元改界后旧链接仍在。诊断以名字（层位序
  权威）为准，不受影响。
- `tst_datapreview` 在本机报 `0xc0000139`——既有环境项（Qt DLL 顺序），fault-surface ledger 与
  `docs/progress/job-framework.md` 已记；本方向未改其断言。
- 体系域只做两级；建议器只用顶深/厚度，未引入井间距离加权。
