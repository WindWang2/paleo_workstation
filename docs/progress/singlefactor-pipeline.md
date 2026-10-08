# 方向 73：单因素工作流账本（2026-10-08）

基线：origin/master 12346066；独立分支 goal/singlefactor-pipeline-20261007。

## R0 定案与弃案

- 比值入口目前仅结构 IDW 消费 FACTOR_MODE；局部方向/Surfer/地统/旧 IDW 直接 FIELD。定案：workflow 统一提取为 DERIVED 点层 factor_value，各同步/异步 prepare 共用该层，保存原口径与父版本。弃案：只补结构 IDW UI（其他方法会读错指标）。
- 实数据：ProjectDataFacade 的主分层 MD + #273 CatalogWellLithologyProvider 的解释/岩屑段；复用 provider 的只读问答，不装载曲线/图片。MD 砂厚为层段内砂岩区间并集，层厚=底 MD−顶 MD。解释覆盖不足/冲突如实缺失，不用 GR 阈值冒充解释；孔渗直读已有字段，净毛比无有效层判据，递延。
- 显式 direct/ratio 严格选字段；保留 wellacquisition 老请求的兜底兼容，但新提取不允许零分母回落到砂厚等异量纲字段。
- 两族仅重组 UI，constraintType/semantic/blockMode 词表不变。方向引导与软边界改正权重；hard_barrier/full_block 进入连通/阻断；contour_stop 终止提线；cartographic_detour 仅解释工作场。局部方向 Kriging 忽略方向/软权重并记 issues；geostat Kriging/SGS 不消费逐线权重，方法说明如实呈现。
- 参数：structural/Surfer/local IDW 幂次；local IDW 井群权重；local Kriging 消费模型/块金/拱高/变程/方位/克里金邻域（与 IDW 邻域分开）；geostat Kriging/SGS 消费球状/指数/高斯、块金/拱高/变程/方位/邻域；SGS 另有次数/种子/集合保留。逐线比值/半径/强度是约束参数，保持保存到线的语义，不能冒充全局 solver 参数。
- 原等值线样式在 QgisStyleService，灰线 0.25mm、7pt、白缓冲 0.6mm，只认 ELEV；结构产物为 level。统一 FactorStyleWriter 出处，兼容两字段，JetBrains Mono 数字、双描边线和白 halo 保证各色带与明暗画布可读；地图符号随主题保持同值（DESIGN 数据符号纪律）。

- 工作台装配会把 ConstraintPage 整页藏在“高级工具”，且重新镜像 ribbon 到旧工作台 factor 按钮。定案：单因素页默认编图流程直接挂 ConstraintPage，原工作台图件操作改名“图件与版本”；本页 ribbon 保持同一方法/生成入口。
- Surfer 的 UI 异步准备未选择 Surfer 引擎，会误跑局部 IDW。按 method 固定选择真实 surfer 引擎，回归准备参数与实际方法一致。
- 原始字段旧 IDW 入口单独收起，主流程/Ribbon 走同一提取入口；历史井点因子版本保留 factor_value，不套用相分类注记。

## 验证与遗留

原五按钮 objectName 保留为族内次级动作；通用 shape 路径收起但可用。

- `cmake --build build -j8` 全量构建成功；收尾修改另将主程序与 34 个相关回归目标增量重建成功。
- 主回归共 45 项（25 项 `tst_singlefactor_*` 全族、全部 `tst_constraint*`、factorworkflow/contourlevels/mappingpages/ui，加 mappingworkbench/wellsection_workflow 与 11 项规范门禁）全部通过。首轮 44/45；历史点版本测试缺少显式 instantiate 的夹具调用已修正，factorworkflow 单独复跑通过（25 passed / 0 failed / 1 skipped）。
- 新回归覆盖严格 direct/ratio、有效零值、零分母不兜底、真实 SMI 分层与解释岩性文件、砂段裁剪/并集与缺失/冲突、样点父版本、各方法统一输入准备及结构/旧 IDW 实际成图、不可变点版本重开、两族捕获参数持久化、方法显隐、默认工作台与两套等值字段样式。
- `tools/check_tidy.py` 22 个 TU 通过；最后修改的 constraintpage/mappingworkbench 两处另跑 clang-tidy 通过。layering/ui_invariants/ui_tokens strict、i18n 与 `git diff --check` 均通过。
- `PALEO_SINGLEFACTOR_QA_DIR="$PWD/build/singlefactor-qa"` 配合页面/工作流回归导出 8 张生产控件与 QGIS 渲染截图；核查 IDW/Kriging 明暗布局、等值面色带与 ELEV/level 的双描边、mono 注记和 halo。
- 既有 `realAreaDoesNotInventWellValues` 因 `PALEO_REAL_PROJECT_AREA` 未配置跳过，不算作 O12 实区通过；本方向新增文件格式/真实 QGIS 栈回归均执行。净毛比有效层判据、LAS 孔渗统计口径、地统逐线屏障已如实登记 TODOS。
