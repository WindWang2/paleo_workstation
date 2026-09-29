# 解释工具（INTERPRETATION）

> wave/seismic-chain-deep · Phase 4 交付（D4.1–D4.10）+ Phase 5 井震
> 组件：`services/seismictaskservice`（解释模型与算法）、
> `ui/seismicsection/seismicpickpanel`（列表面板）、画布解释模式（SECTION.md §交互）

## 1. 数据模型（服务层）

```cpp
SeismicPick        { id, inlineNo, xlineNo, twtMs, sampleIndex,
                     confidence, interpreter, horizonName }
SeismicFaultSegment{ id, sectionType, sectionIndex, points[(traceFrac,twtMs)],
                     interpreter, name }
SeismicInterpretationSession { name, sourceSgyPath, interpreters[],
                               picks[], faults[], nextId }
```

**落位裁决**：解释模型放 `seismictaskservice`（数据层）——视图层 `io/*`
include 白名单仅 `lasdoc.h`，解释模型必须从画布/面板（视图）可达，而
seismictaskservice 是视图可达的唯一本包数据层文件。

## 2. 工作流

```
画布解释模式（● 拾取 / ✂ 断层，互斥单选）
  ├─ 拾取：pickPlaced(列,TWT) → dock 解析 (IL,XL)（SectionRef 剖面身份）
  │        → AddPicksCommand（QUndoStack）→ 会话 + 画布叠加 + 自动保存
  ├─ 断层：拖画折线（草稿虚线）→ faultDrawn(归一化点列) → 会话
  └─ 面板：列表/定位/删除/重命名/导出 CSV/追踪/资产登记
```

## 3. 各交付项落点

| 项 | 行为 |
|---|---|
| D4.1 种子拾取 | 画布点击放点；`SectionRef{type,index,colMin,colMax}` 把列号换算成测线号（IL 剖面列=XL 轴，反之亦然） |
| D4.2 互相关追踪 | `trackHorizon(slice, type, index, colMin, colMax, seedCol, seedSample, opts,…)`：种子道波形 × 滑动窗 Pearson 相关，双向逐道追踪；**低于阈值即停**（不硬凑）；窗参数/阈值 UI（面板行） |
| D4.3 层位资产 | `registerHorizonAsset`：拾取网格化 → CSV（inline,xline,twt_ms,confidence）→ `CatalogVersion{stage=DERIVED, parentVersionIds=[地震RAW], managed=false, sha256}` 外链登记 |
| D4.4 断层资产 | `registerFaultAsset`：折线 CSV（segment,section,traceFrac,twtMs）→ 同上 DERIVED 登记 |
| D4.5 列表面板 | 7 列表格（ID/IL/XL/TWT/置信度/解释者/层位）；置信度红黄绿着色；定位（跳剖面+线号）/删除/重命名/导出 |
| D4.6 undo/redo | `QUndoStack` 原子命令：AddPicks/RemovePick/RenamePick（redo 分配新 id 语义） |
| D4.7 网格化 | `gridPicks`：IDW（power=2）→ 规则测网栅格（步长=同轴最小间隔；角点直取、内部插值） |
| D4.8 会话持久化 | `<sgy>.seispicks.json` 伴生文件（picks+faults+名册+nextId）；`setVolume` 自动恢复；每次变更自动保存（崩溃恢复点，D6.7） |
| D4.9 多解释者 | 名册 + 每拾取解释者字段；活动解释者下拉 |
| D4.10 置信度 | 追踪相关系数即置信度（手动拾取=1.0）；画布叠加绿(≥0.75)/橙(≥0.5)/红 |

## 4. 追踪算法细节（D4.2）

```
seedWave = 种子道 [seedTop, seedTop+win) 列跨步取数   ← 行主序布局：道=跨行！
for col ← seed±1 → 双向：
  搜索窗 = [prevSample-maxSearch, prevSample+maxSearch] 钳体积界   ← 不钳种子窗
  candidate = col 列同窗跨步取数
  best = argmax Pearson(seedWave, candidate)
  bestCorr < threshold → break（同相轴丢失）
  拾取 = 窗口中心；confidence = bestCorr
```

两个曾修的算法 bug（测试锁定）：候选窗必须**按列跨步**取数（行内连续指针
是横向跨道的噪声条——假相关 1.0）；搜索窗必须围绕**上一道位置**且钳到
体积界（否则漂移超一个窗长后永远断链）。合成倾斜同相轴 120 道全追踪、
中断即停（`trackingOnSynthetic`/`trackingStopsAtCorrelationLoss`）。

## 5. 井震（Phase 5，D5.1–D5.7）

| 项 | 行为 |
|---|---|
| D5.1 任意线编辑器 | 「井与分层」菜单 → 折线节点表（il xl 每行）→ `extractSectionFromVolumeAsync` + 候选井投影开关 |
| D5.2 提取缓存 | 服务层任意线 LRU≤4（体指纹×路径 FNV）；同路径重提 51ms→0ms |
| D5.3 井轨迹投影 | 顶/底 XY 独立投影到剖面折线（斜井轨迹线）；时深无检查点 → 标题栏原因注记（仍均速投影，不静默） |
| D5.4 合成记录 | `computeSyntheticSeismogram`：AC+DEN→阻抗→反射系数→25Hz Ricker 褶积（2ms 网格，归一[-1,1]）；缺曲线→井位降级原因注记（橙字）；剖面内红波形叠加 |
| D5.5 分层标注 | 井筒沿线 tops 十字+标签（既有） |
| D5.6 井旁道小图 | 最近井 IL/XL 吸附 → 道 wiggle（变面积填充）+分层刻度 |
| D5.7 多井开关 | 全部 / 最近 1 口 / 最近 3 口（\|偏距\| 排序过滤） |

## 6. catalog 登记契约（D4.3/D7.4）

- 资产 id：`seis_horizon_<seisAssetId>_<horizon>` / `seis_fault_…`；
  重复登记幂等（同 sha 命中即返回既有路径）。
- 版本：`stage=DERIVED`、`parentVersionIds=[源地震 RAW 版本]`、
  `managed=false`（解释产物在工程 `interpretation/` 目录，外链托管）、
  `sha256` 真算、`extra.origin="seismic-interpretation"`。
- app 层接线点：`SeismicSectionDockWidget::setInterpretationCatalog(catalog,
  assetId, versionId, outputDir)`——主窗口持有 catalog（TODOS 登记）。

## 7. 复现

```bash
QT_QPA_PLATFORM=offscreen ./build/tst_seismic_interpret  # 11 用例
QT_QPA_PLATFORM=offscreen ./build/tst_seismic_welltie    # 7 用例
```
