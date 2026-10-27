# 历史任务书

这些任务书保存早期迭代的原始需求与验收约定，不再作为当前排期或执行入口。
文中的“新增”“在飞”、预算、分支规则和环境接线属于发出时的记录。
当前开发遵循 [AGENTS.md](../AGENTS.md) 和 [BUILDING.md](../BUILDING.md)，
未完成工作见 [TODOS.md](../TODOS.md)，交付证据见 [历史账本索引](../docs/ledger-archive/README.md)。

编号任务书份数：94

编号为 01–84 与 91–100；85–90 未立项。份数与目录文件由 `tools/check_docs_drift.py` 校验。

| 编号 | 任务书 |
|---|---|
| 01 | [Goal Loop — goal/ui-experience-polish：UI/UX 全仓深化打磨](01-ui-experience-polish.md) |
| 02 | [Goal Loop — goal/seismic-attributes：地震属性计算引擎（新功能）](02-seismic-attributes.md) |
| 03 | [Goal Loop — goal/seismic-3d-viz：3D 可视化与交互剖切深化（新功能）](03-seismic-3d-viz.md) |
| 04 | [Goal Loop — goal/perf-systematize：性能工程体系化（优化方向）](04-perf-systematize.md) |
| 05 | [Goal Loop — goal/ai-geological-assist：AI 辅助解释生产化（新功能）](05-ai-geological-assist.md) |
| 06 | [Goal Loop — goal/time-depth-velocity：时深转换与速度建模（新功能）](06-time-depth-velocity.md) |
| 07 | [Goal Loop — goal/fault-interpretation：断层解释数据框架与拾取工具（新功能）](07-fault-interpretation.md) |
| 08 | [Goal Loop — goal/petrophysics-logs：测井岩石物理计算与曲线处理（新功能）](08-petrophysics-logs.md) |
| 09 | [Goal Loop — goal/horizon-autotrack：层位自动追踪（新功能）](09-horizon-autotrack.md) |
| 10 | [Goal Loop — goal/gridding-surface-ops：网格化算法与面运算（功能深化）](10-gridding-surface-ops.md) |
| 100 | [Goal-Loop 方向 100：文档漂移与账本入库纪律——PLAN/层计划刷新 + 三份脱队账本收编 + 漂移护栏](100-docs-drift.md) |
| 11 | [Goal Loop — goal/mapbook-reporting：批量制图与报告引擎（新功能）](11-mapbook-reporting.md) |
| 12 | [Goal-Loop 方向 12：合成地震记录与井震标定（Synthetic Seismogram & Well-Tie）](12-synthetic-welltie.md) |
| 13 | [Goal-Loop 方向 13：三维地层格架与属性建模（Stratigraphic Grid & Property Modeling）](13-property-modeling.md) |
| 14 | [Goal-Loop 方向 14：交会分析与无监督相分类（Crossplot-Driven Facies Classification）](14-crossplot-facies.md) |
| 15 | [Goal-Loop 方向 15：多文件井曲线汇聚读面（Well LogSet / 跨文件并集曲线）](15-well-logset.md) |
| 16 | [Goal-Loop 方向 16：catalog 持久层迁 SQLite（存储引擎级替换）](16-catalog-sqlite.md) |
| 17 | [Goal-Loop 方向 17：断层立体化——断棒成面与断距量算（Fault Surface Assembly）](17-fault-surface.md) |
| 18 | [Goal-Loop 方向 18：地质统计学方法包（变差函数 / 克里金 / SGS）](18-geostat-methods.md) |
| 19 | [Goal-Loop 方向 19：井轨迹空间化（测斜模型 / MD↔TVD / 定向井贯穿消费面）](19-well-trajectory.md) |
| 20 | [Goal-Loop 方向 20：任务框架统一 + 巨型文件拆分（重构大方向）](20-job-framework.md) |
| 21 | [Goal-Loop 方向 21：数据路径性能攻坚（索引缓存修复 / LAS 读面 / 冷启动 / catalog 热路径）](21-data-perf.md) |
| 22 | [Goal-Loop 方向 22：地震反演（子波库 / 低频模型 / 带限与稀疏脉冲反演 → 波阻抗体）](22-seismic-inversion.md) |
| 23 | [Goal-Loop 方向 23：单因素语义补完 + 约束线交互绘制（上游全量移植收官）](23-singlefactor-completion.md) |
| 24 | [Goal-Loop 方向 24：交互式约束编辑与地图数字化工具链](24-interactive-editing.md) |
| 25 | [Goal-Loop 方向 25：图件编制与出版输出——布局设计器补全](25-layout-publishing.md) |
| 26 | [Goal-Loop 方向 26：连井剖面深化——编辑交互、基准面与栅状图](26-wellsection-deep.md) |
| 27 | [Goal-Loop 方向 27：沉积相自动编图辅助链——优势相→相界→合成→QA](27-facies-automapping.md) |
| 28 | [Goal-Loop 方向 28：层序地层格架工作台——格架树、标志层与编图单元校验](28-sequence-framework.md) |
| 29 | [Goal-Loop 方向 29：UI 视觉一致性与打磨收口（第二轮）](29-ui-visual-polish.md) |
| 30 | [Goal-Loop 方向 30：工区数据管线与健康管理——导入向导、体检与回收站](30-data-pipeline.md) |
| 31 | [Goal-Loop 方向 31：地质符号库与花纹体系——岩性/相花纹、井符号与图例自动生成](31-geological-symbols.md) |
| 32 | [Goal-Loop 方向 32：井分层数据编辑与质量管理——分层表、批量修正与版本化](32-well-tops-editor.md) |
| 33 | [Goal-Loop 方向 33：批处理与作业队列编排——多层位批量计算、恢复与报告](33-batch-jobqueue.md) |
| 34 | [Goal-Loop 方向 34：新井部署与井网辅助——覆盖诊断、候选点位与方案对比](34-well-siting.md) |
| 35 | [Goal-Loop 方向 35：沉积体系多期演化分析——层位叠置、相带迁移与演化视图](35-sedimentary-evolution.md) |
| 36 | [Goal-Loop 方向 36：版本衍生血缘图谱——parentVersionIds 知识图谱可视化](36-derivation-graph.md) |
| 37 | [Goal-Loop 方向 37：层位自动追踪 3D 化——体传播服务暴露 + 显式倾角引导](37-horizon-autotrack-3d.md) |
| 38 | [Goal-Loop 方向 38：连井剖面 TVD 域 + 真实井距 + 解释岩性道](38-wellsection-tvd.md) |
| 39 | [Goal-Loop 方向 39：相界地质语义类型——整合接触/尖灭/相变的编辑与图面表达](39-boundary-kinds.md) |
| 40 | [Goal-Loop 方向 40：断面 Y 型/分叉成面与自动生长](40-fault-surface-branching.md) |
| 41 | [Goal-Loop 方向 41：单因素原生算法深化——变差函数克里金 + SFPKG/外委格式批量读取](41-singlefactor-kriging.md) |
| 42 | [Goal-Loop 方向 42：数据中心规模治理——资产表虚滚动 + 存储治理台](42-datacenter-scale-governance.md) |
| 43 | [Goal-Loop 方向 43：地震属性体化——时间切片/整体扫描 + 属性图层入层树](43-seismic-attr-volume.md) |
| 44 | [Goal-Loop 方向 44：井曲线多格式读口——DLIS/LIS/BE + MD/TVD 对齐 + 挂接语义收口](44-welllog-multiformat.md) |
| 45 | [Goal-Loop 方向 45：属性建模 V2——断块错位网格 + 序贯高斯/相控建模](45-property-model-v2.md) |
| 46 | [Goal-Loop 方向 46：交会有监督分类——样本标注→训练→推理 + 置信度伴生栅格](46-crossplot-supervised.md) |
| 47 | [Goal-Loop 方向 47：多 realization / 不确定性——预测面集合与置信度派生](47-realization-uncertainty.md) |
| 48 | [Goal-Loop 方向 48：审计清账——AUDIT_ISSUES 未修项修复 + 假绿测试清零](48-audit-closure.md) |
| 49 | [Goal-Loop 方向 49：架构收口——文档漂移修复 + 检查器强化（传递闭包/词表/死代码）](49-arch-closure.md) |
| 50 | [Goal-Loop 方向 50：IO 稳健性攻坚——SEG-Y/井文件解析容错 + 深度基准诚实面](50-io-robustness.md) |
| 51 | [Goal-Loop 方向 51：AI 辅助面升级——Mock 路由接线 + LLM 地质对话助手骨架](51-ai-assist-upgrade.md) |
| 52 | [Goal-Loop 方向 52：测试深化——零测试头清零 + dataChanged 增量通道](52-test-deepening.md) |
| 53 | [Goal-Loop 方向 53：zh_CN 本地化落地——5,196 条翻译清零 + lrelease 产物门禁](53-zh-cn-l10n.md) |
| 54 | [Goal-Loop 方向 54：错误面统一——ErrorHub 错误通道 + 状态栏/通知收敛](54-errorhub.md) |
| 55 | [Goal-Loop 方向 55：seismictaskservice 拆分——5,041 行 God-service 解体](55-seismic-svc-split.md) |
| 56 | [Goal-Loop 方向 56：主窗壳层拆分——attach 3,816 + datalist 3,839 双巨兽解体](56-shell-split.md) |
| 57 | [Goal-Loop 方向 57：功能层与数据层巨兽拆分——constraintfactorjobs 3,693 + dataimportservice 2,437 双文件解体](57-layer-split-dual.md) |
| 58 | [Goal-Loop 方向 58：审计尾巴清零——BIZ-07/RUNTIME-03/BIZ-10/BIZ-11 四项无主债务收口](58-audit-tail.md) |
| 59 | [Goal-Loop 方向 59：头文件卫生——重头出闸收口（previewdoc 扇出 + QWidget 前置声明）](59-header-hygiene.md) |
| 60 | [Goal-Loop 方向 60：高 DPI 适配——3D 视口确定性缺陷修复 + 护栏](60-highdpi.md) |
| 61 | [Goal-Loop 方向 61：AI 工具调用闭环——tools[] 上送 + 领域工具执行回路](61-ai-toolloop.md) |
| 62 | [Goal-Loop 方向 62：AI 助手 UX 补全——会话管理 / 图形化配置 / markdown 渲染](62-ai-ux.md) |
| 63 | [Goal-Loop 方向 63：快捷键体系与帮助面——总表 / 冲突检测 / F1 上下文帮助](63-shortcuts-help.md) |
| 64 | [Goal-Loop 方向 64：错误面统一——ErrorHub 错误通道 + 通知/模态/状态栏三级呈现（方向 54 修订重发）](64-errorhub.md) |
| 65 | [Goal-Loop 方向 65：seismicsection 家族拆分——dockwidget 3,040 + canvas 2,192 新晋头号巨兽解体](65-section-split.md) |
| 66 | [Goal-Loop 方向 66：wellcomposite 家族拆分——track 1,875 + panel 1,640 + canvas 1,549 三文件解体](66-composite-split.md) |
| 67 | [Goal-Loop 方向 67：单因素域补完——协克里金 / 隔断感知变差函数 / SFPKG 写出 / 策略包 UI](67-sf-completion.md) |
| 68 | [Goal-Loop 方向 68：Python 脚本面——脚本运行入口 / 内嵌控制台 / 结果落地通道](68-python-scripting.md) |
| 69 | [Goal-Loop 方向 69：连井剖面 TVD 域 + 解释岩性道（方向 38 修订重发）](69-wellsection-tvd.md) |
| 70 | [Goal-Loop 方向 70：Sanitizer 常态化与构建硬化——ASAN CI 化 + 预算豁免 + Unity 前置清障](70-sanitizer-hardening.md) |
| 71 | [Goal-Loop 方向 71：依赖口径统一——QGIS 三路版本分裂收敛 + #76 CI 政策迁移](71-dep-unify.md) |
| 72 | [Goal-Loop 方向 72：Windows 环境债根治——Qt 6.8/6.11 混链收敛，恢复本机全量回归可信度](72-win-qt-unify.md) |
| 73 | [Goal-Loop 方向 73：单因素图工作流补全——井点因子提取、约束线两族明确化、插值方法明面化、等值线/面主题规范](73-singlefactor-pipeline.md) |
| 74 | [Goal-Loop 方向 74：单因素原生算法收口——约束语义全方法消费 + 协克里金接线 + TODOS P2 对账](74-singlefactor-geostat.md) |
| 75 | [Goal-Loop 方向 75：CI asan 断链修复 + 运行侧验证——观察档真正跑起来](75-ci-asan-fix.md) |
| 76 | [Goal-Loop 方向 76：双 ErrorHub 合流——状态栏徽标接线修正 + 54 系呈现件归并](76-hub-merge.md) |
| 77 | [Goal-Loop 方向 77：AI 工程上下文注入——query_project 工具 + 目录/血缘摘要进对话](77-ai-context.md) |
| 78 | [Goal-Loop 方向 78：mkproject 夹具工厂化——manifest 参数化 + 测试消费闭环](78-mkproject-fixtures.md) |
| 79 | [Goal-Loop 方向 79：图片道深化——锚深后补编辑 + 缩略图 LOD + 管理面板](79-imagetrack-deep.md) |
| 80 | [Goal-Loop 方向 80：快速跟随批——#295 后实战面小修收口（岩性词面配置化 + cuttings 引号转义 + 同域补丁文件清单化）](80-quickfollow.md) |
| 81 | [Goal-Loop 方向 81：环境债二期——16 红清单逐条清零 + QSettings 沙箱决策](81-env-debt2.md) |
| 82 | [Goal-Loop 方向 82：地震窗聚类与测区面文档化补账——零文档新增面立账 + 证据补全](82-basemap-docs.md) |
| 83 | [Goal-Loop 方向 83：paleomainwindow 壳层收尾——本体 2,137 行二次拆分 + 装配根归并审视](83-mainwindow-split2.md) |
| 84 | [Goal-Loop 方向 84：方向 74 执行——克里金约束消费 + 协克里金接线（已有任务书，本方向为发车令）](84-sf-geostat-go.md) |
| 91 | [Goal-Loop 方向 91：#310 处置与方向 84 增量收口——UI 协变量面 + 隔断感知拟合落地](91-cokriging-ui.md) |
| 92 | [Goal-Loop 方向 92：数据树角色词表单源化 + 树增量通道——一批小手术收两个结构债](92-tree-roles-incremental.md) |
| 93 | [Goal-Loop 方向 93：AI 工具结果结构化呈现——血缘图/资产表进对话卡片](93-ai-resultviz.md) |
| 94 | [Goal-Loop 方向 94：Windows vendored LibreOffice + 文档预览跨平台收口](94-lo-win.md) |
| 95 | [Goal-Loop 方向 95：mkproject 夹具扩散——tst_import/tst_aiwiring 等高价值测试改吃 mini 夹具](95-mkproject-spread.md) |
| 96 | [Goal-Loop 方向 96：constraintpage 1,830 行拆分——全仓新头号巨兽解体（域 TU + internal.h）](96-constraintpage-split.md) |
| 97 | [Goal-Loop 方向 97：datapreviewtabs 家族拆分——cpp 1,758 + internal.h 1,607 双体解体（候选清单榜首）](97-previewtabs-split.md) |
| 98 | [Goal-Loop 方向 98：wellsection 家族深化——scene 1,762 行拆分 + 时深对齐消费收口 + 多解释合并策略](98-wellsection-deep.md) |
| 99 | [Goal-Loop 方向 99：catalog 层深化——datacatalog 1,795 + catalogstore 1,605 双巨兽解体 + 词表面收敛](99-catalog-split.md) |
