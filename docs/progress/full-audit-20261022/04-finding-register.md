# Full Audit 2026-10-22 — 04 Finding Register（问题立案册）

来源：5 份只读深审报告（03-review-{A,B,C,D,E}.md，均在本目录）+ 主代理交叉复核。
基线 b301f2e8；增量窗 093346e6..b301f2e8（20 PR）。全部 Finding 证据与复现路径
见各分报告，本册只做汇总、去重与状态跟踪。

## 汇总

- 新立案：**22 项**（原始 23，B-05≡D-03 合并），**全部 P3，零 P0/P1/P2**。
- 置信度：VERIFIED 14 / HIGH 3 / SUSPECTED 3 / VERIFIED(机制)+HIGH(整体) 1 / 文档项 1（E-01 计入 VERIFIED）。
- 排除登记（不修）：D-04~D-08（#590/#592 同类新面/存量，5 项）、A 观察 O-1（#587 族）。
- 上轮（20261021）在册项复核：C-1..C-6 全部修复确认；B-1/B-4 修复确认；B-3 维持登记不修；
  A-01'21（ShaCache）在册未修（issue 化归属外部批次）；A-03'21 解冻即本轮 A-04。

## 立项表

| ID | Sev | Conf | 模块 | 一句话根因 | 状态 |
|----|-----|------|------|-----------|------|
| C-7 | P3 | VERIFIED(机制)/HIGH | workflow/wellpredictionreview | predictionLog 对英尺 LAS 不做米制归一，验证页曲线×相带错轴 | FIXED(fc7e2b08) |
| B-02 | P3 | VERIFIED | services/welllogset | mdAxisInMetres 对 legacy 未知非空单位当米直通，与 readCurveTvd 双口径 | FIXED(fc7e2b08) |
| B-03 | P3 | VERIFIED | services/petrophyscomputeservice | MD 闸先于 parse ok 检查，解析失败误报「校正索引元数据」 | FIXED(fc7e2b08) |
| C-9 | P3 | VERIFIED | workflow × tests | 五条 MD/单位硬拒绝路径零测试覆盖 | PARTIAL→结转测试债（fc7e2b08 三面新测 + 既有 wellsection/mappingworkbench 基准面两处；propertymodelworkflow/sectionworkbench 建独立测试批次——R2-S1 裁定二选一取结转） |
| A-02 | P3 | VERIFIED | io/welllogtext | 文本曲线只认严格 UTF-8，GB18030 中文文件被拒（生态割裂） | FIXED(d7424bde) |
| A-03 | P3 | VERIFIED | domain/projectclassifier × io/welllogtext | 分类器词表（井深/分号）与读取器词表不一致，预览提议→执行拒绝 | FIXED(d7424bde) |
| B-01 | P3 | HIGH | workflow/batchjobqueue + services/jobrunner | 批量作业硬编码 High 优先级挤占 4 线程池，「让位交互」闸结构性不可达 | FIXED(0235c343) |
| B-05≡D-03 | P3 | VERIFIED+HIGH | qgis/previewrasteranalysis | requestSummary/Histogram/Extrema 失败/取消永不回调，面板静默保留旧图统计 | FIXED(b29301ae) |
| B-06 | P3 | SUSPECTED | qgis/previewrasteranalysis | 平坦面 MAD=0 时极值阈值退化 0，标出 100 个零显著度「峰」（头注契约矛盾） | FALSE_POSITIVE（neighbourhood 严格比较使平坦面候选为空；既有 tst_previewmap_assets:1164 用例钉住；b29301ae 留验证注释） |
| A-01 | P3 | VERIFIED | io/dataimport_log | 单模糊井名命中被文案谎报「对应多口井」 | FIXED(9b231b0d) |
| A-06 | P3 | VERIFIED | io/welllogtext | 空白分隔分支无 kMaxColumns 逐格早退（内存放大后总量闸兜底） | FIXED(d7424bde) |
| A-08 | P3 | VERIFIED | io/dataimport_folder | F1 迟到 Skipped 行排在结果行首，违反行序契约 | FIXED(9b231b0d) |
| C-8 | P3 | VERIFIED | workflow × io/welllogread | 六个调用点外层 detect + parse 内部再 detect，双趟嗅探 | FIXED(3af0e299) |
| D-01 | P3 | HIGH | ui/wellcomposite/paneltracks | FormationTrack 数据源靠道标题字符串区分，改名后同步错路由（图面数据混淆） | FIXED(49bd3723) |
| E-01 | P3 | VERIFIED | vendor/sbm PATCHES.md | columnDistancesM 分叉漏记 P14，上游同步重放契约会破 | FIXED(80bc61ac) |
| E-02 | P3 | VERIFIED | AGENTS.md + cmake 注释 | qgs/spread 基线计数文档漂移（149/116 → 实 172/137） | FIXED(80bc61ac) |
| E-03 | P3 | VERIFIED | cmake/paleo-test-infra.cmake | qgis_spread 注册块缩进错位（维护陷阱） | FIXED(80bc61ac) |
| A-05 | P3 | VERIFIED | io/ingestplan+dataimport_log+file | 文本/XML 曲线一轮预览-确认 ≥6 趟全文件 IO（plan 期全量解析取井名） | CARRY-OVER（结转，见 05） |
| A-04 | P3 | VERIFIED | io/dataimport_file | dedup 三族补挂路径 readAll 整读（#561 流式化未覆盖） | CARRY-OVER（结转，见 05） |
| B-04 | P3 | SUSPECTED | services/welllogset+previewdoc | storedHeader 元数据优先无 mtime/指纹失效闸 | CARRY-OVER（结转，见 05） |
| D-02 | P3 | SUSPECTED | ui/wellcomposite/hiddentrackbar | 隐藏道恢复走陈旧 specIndex（窄窗竞态） | CARRY-OVER（结转，见 05；D-01 落地后收敛路径已明） |

## 排除与登记（不修，防复查）

| 项 | 归属 |
|----|------|
| D-04 井位页每次构建同步重放 savedChoice（大栅格底图开页卡顿） | EXCLUDED_EXISTING_ISSUE（#590 同类；#616 新面登记） |
| D-05 Quantile/HistEq GUI 线程全带直方图 | EXCLUDED_EXISTING_ISSUE（#590 同类，存量） |
| D-06 井照片缩略同步解码 | EXCLUDED_EXISTING_ISSUE（#590 同类，存量） |
| D-07 retryRow GUI 线程 copy+SHA | EXCLUDED_EXISTING_ISSUE（#590 同类，存量） |
| D-08 wellcomposite 存量非 tr 文案 4 处 | EXCLUDED_EXISTING_ISSUE（#592 族） |
| A/O-1 storeManagedRaw flush 无 fsync | EXCLUDED_EXISTING_ISSUE（#587 族） |
| C/观察1 文件夹导入取消语义翻转（整批丢弃） | 有意设计（有测试背书），产品侧过目项 |
| C/观察2 aiwiring.cpp 注释过期 | P4 级，随手修窗口 |
| C/观察6 三个深度换算函数三体并存 | 收敛建议（wellnumeric 权威化），独立批次 |

## 严重度为空档的说明

零 P0/P1/P2：五模块独立深审 + 主代理接缝复核后，20 个新 PR 的增量面在本轮
审查深度下未发现可触发的崩溃、数据损坏、数值错误或主线阻塞；上轮 4 项 P2
修复经 C/B 复核全部落地。剩余问题全部为一致性、体验、性能与维护性 P3。
