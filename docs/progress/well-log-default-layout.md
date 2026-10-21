# 单井测井网格与默认图道（2026-10-21）

对应用户反馈 4–6。

- 曲线白底增加浅灰主次实线网格，默认十等分；横向网格按真实测深定位，随比例尺自适应，滚动和导出复用同一绘制入口。颜色取 Light border / surface-alt，保留网格开关和密度设置。
- LAS、综合 XML 与“恢复标准测井组合”共用标准配置：岩性 GR/SP/CALI，三孔隙 AC/DEN/CNL，深/中/浅电阻率，按常见别名匹配，每个物理量选一根实际存在的曲线。不占空道；无标准曲线时展示首个有效组合。
- 辅助、重复版本、气测、井斜与离散曲线仍保留，默认隐藏。恢复入口超过四项时余项使用原生菜单，避免撑宽窗口；重建时清理旧布局项。
- “维护相”改为“井段相解释”，默认隐藏，属性入口解释其来源（源文件、预测回写、人工编辑），不暗示人工审核完成。预测相和置信度独立保留；刷新/编辑井段属性不覆盖预测道。
- 保存的自定义图道标题、曲线、量程和显隐可从完整源曲线池恢复；重置默认时也使用完整源池。预测置信度在重置时保留身份，后续清除预测不残留僵尸道。

护栏：`layering --strict`、i18n、ui_invariants 通过。file_budget / ui_size / ui_tokens 的失败与基线 master e6837e14 相同：constraintpage_interpolation 的 updateEngineRows、folderconfirm 的 buildFolderConfirmDialog、datalist_tree 的文件与 buildNavTreeSpec 规模；identifypopup 的 setSpacing(2)，以及 compositebody 的两条既有过期颜色例外。docs_drift 另有主分支既有的主窗家族计数 26/27 TU 漂移。未扩张这些基线。

本次 ui-token 例外删除 1 条（曲线网格的两处灰色字面量），条目数 356 → 355；规模基线零增加。

验证：Windows 独立 worktree / build，Qt 6.11.2 + QGIS 4.2，最多 8 个编译任务。相关目标构建通过；以下四组测试通过：`tst_wellcomposite`、`tst_wellcomposite_framework`、`tst_wellfacies`、`tst_wellcomposite_visual`（含既有黄金图抽样）。两个真实井/服务验证入口及一个可选视觉采集入口按原约定跳过，未调用外部预测服务。

新增用例验证：关键曲线选择与完整保留、LAS/XML 一致性、辅助道菜单及布局不累积、网格的真实深度对齐与关闭、自定义标题会话恢复、属性刷新不覆盖预测，以及恢复标准组合后再次预测不遗留旧置信度道。合成井截图 `build/well-default-layout.png` 已目视检查。

参考图片：飞书临时链接在网页工具及内置浏览器中均未能加载；实现依据用户文字要求和 DESIGN.md。
