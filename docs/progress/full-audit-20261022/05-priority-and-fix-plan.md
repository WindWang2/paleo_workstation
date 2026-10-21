# Full Audit 2026-10-22 — 05 修复优先级与计划

## 基线测试分类（Phase 2）

- 基线构建：首跑失败（ENVIRONMENT_FAILURE：共享 prefix 中 libarrow/parquet 五个
  悬空符号链接指向已删除的 appimage 暂存目录，ld 无法闭合 libgdal DT_NEEDED；
  从 pacman 缓存 arrow-25.0.1-7 包提取真实库替换后恢复）。
  修复动作记录于 00-baseline.md 与 09-loop-ledger.md；非本仓代码问题。
- 基线 ctest：构建完成后执行，结果回填 06-baseline-quality.md。

## Fix 1 簇划分（每簇独立 commit，红绿测试优先）

| 簇 | Findings | 主题 | 预期 commit |
|----|----------|------|------------|
| 1 | C-7、B-02、B-03、C-9 | 深度单位/基准诚实性（英尺 LAS 归一、未知单位拒绝、MD 闸序、拒收路径测试） | fix(workflow/services) |
| 2 | A-02、A-03 | 文本曲线生态一致性（GB18030 解码、分类器/读取器词表同源） | fix(io) |
| 3 | B-01 | 批量优先级让位交互（JobRunner 优先级参数 + BatchQueue 传后台级） | fix(workflow) |
| 4 | B-05≡D-03、B-06 | request* 失败通道 + 平坦面极值契约 | fix(qgis) |
| 5 | A-01、A-06、A-08 | 导入 IO 小修（文案区分、列数早退、行序契约） | fix(io) |
| 6 | D-01、E-01、E-02、E-03 | 道数据源身份路由 + vendor 补丁登记 + 基线文档对齐 | fix(ui)+docs |
| 7 | C-8 | WellLogRead::parse 格式参数重载（消双嗅探，随簇1 同文件收尾） | perf(io) |

## 结转项（本轮不修，理由）

| ID | 理由 |
|----|------|
| A-05 | 需要新增 plan 期井名快照通道（io 契约扩展）+ 三处调用面协同，体量与回归面
  超出本轮「最小根因修复」边界；64MB/4M 格预算使上界可控。建议独立 io 打磨批次
  与 A-04（dedup 流式化）合并处理。 |
| A-04 | 同上（#561 同款流式化改造，三族路径）。 |
| B-04 | SUSPECTED 级、依赖「受管文件被外部越轨编辑」前提；需 catalog extra 指纹
  字段（schema 面）。待证伪/证实后独立批次。 |
| D-02 | SUSPECTED 级窄窗竞态（需事件循环精确交错）；D-01 的数据源身份落地后
  天然获得按身份恢复的通道，届时一并收殓。 |

## 修复纪律

- 每簇：读源 → 红测（能红则红）→ 最小修改 → 定向测试 → 同域回归 → commit。
- 不放宽断言、不改 golden 迎合输出、不加吞错兜底。
- 基线文件只缩不涨；闸门本地全跑。
