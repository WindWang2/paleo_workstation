# 方向 05 ledger：QGIS 图层体系、样式制图与既有地图版式能力深化

- 分支：`goal7/qgis-layers-layout-20261014`；worktree：`../paleo-wt-goal7-qgis-layers`
- Base SHA：`b0520429c21f858a1c053b876f511680088564a9`（2026-10-14 执行时 origin/master 实测）
- 环境：Linux x86_64（16C/62G），cmake 4.4.4 + Ninja + ccache 4.14，Debug，QGIS_PREFIX=主仓 superbuild prefix（symlink 共享，只读不写），QT_QPA_PLATFORM=offscreen
- 全局构建锁：`/tmp/paleo_workstation_global_build_j4.lock`，构建/测试一律 flock -j4
- 实时边界复核：Open PR 仅 #437（80 文件只读，清单见 05-baseline.md §1）；Open Issues #352–#421 全归 Issue Agent

## R0（2026-10-14）基线与消重 —— 完成

- 阶段 0 盘点：fetch 后 origin/master = b0520429 与任务书核查基线一致；主 checkout 干净；无同名 worktree/分支；新建 worktree + vendor symlink 接线（superbuild/prefix + onnxruntime；本机无 vendor/prefix deb 闭包）；flock 内 configure 一次通过（QScintilla warning 属 BUILDING.md 允许降级）。
- 四路只读审计（S1 图层生命周期 / S2 符号样式 / S3 版式输出 / S4 渲染性能与 QA 基建）完成，主 Agent 对全部候选发现做了代码级抽查复核（qgislayerprofile.h:26-29 契约 vs applyPrunedTheme:283-323；geopatterns.cpp:43-85 NOOP vs :138-302 QObject::tr；layouttemplates.cpp:122-152 outcome 丢弃；mapbooklayout.cpp:38-71 硬编码字体；layertreepanel.cpp:683-759 循环内 declared()；qgislayerorganizer.cpp:153,217-232 双重复制——全部属实）。
- 产物：`docs/progress/parallel-7/05-baseline.md`（主账：写权限边界、已有能力、候选工作项 W1–W10、留档项、性能方案、风险）+ `05-baseline-{layers,symbols,layout,perf}.md`（四份分包审计全文）。
- 候选工作项（全部白名单内、与 #437/Issues 零重叠）：W1 page:* 主题重放保留无声明临时图层可见性；W2 refreshIndicators 声明回查提升出循环；W3 organizer declared() 复制消除；W4 i18n 上下文错配（NOOP vs QObject::tr）；W5 模板失败静默→ErrorHub+信号；W6 mapbooklayout 字体走 figureFontFamily；W7 地图册回读校验补 PDF 尾/PNG 尺寸/SVG 头；W8 exportLayoutAsync 启动失败补 exportFinished；W9 相花纹词表「三角洲」缺口；W10 守恒 Oracle 补强。
- 留档不做（超白名单或 #437 保护）：S1 A-1 双激活层位分叉（paleomainwindow_workbench.cpp，#437 文件）、S3 A4 mapbookqueue failed 计数（workflow）、S4 A2/A3/A4/A7（datapreview/decorations/paleomainwindow）——见 05-baseline.md §4。
- 基线构建/测试结果：见下轮记录（全量构建完成后补）。

## 微循环记录

（后续每轮：目标 → 改动文件/函数 → Oracle → 失败/成功日志要点 → 耗时/RSS → commit ID → 排除事项）
