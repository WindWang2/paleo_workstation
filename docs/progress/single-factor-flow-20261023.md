# 单因素四步流程（2026-10-23）

用户要求：先获取井中单因素，计算结果保存供下次读取；间隔线与方向线可绘制或导入，
均可选；按各因素合理默认参数生成等值图；等值线单独可选。用户确认地层厚度也默认
从井读取或计算。

## 实现

- 页面与 ribbon 按获取、约束线、等值图、等值线四步组织。获取按钮直接可见，
  逐井结果与算法参数保留在折叠段，固定主按钮为“生成等值图”。
- 获取操作优先恢复同层位、因素、提取口径且来源仍有效的井值版本；恢复缺少的
  点图层声明，工程重开后不重复计算。父版本或来源文件 size/mtime 变化则重新读取。
  详细设置提供“重新读取并计算”，强制创建新版本。井分层和解释原件在后台读取前
  冻结文件状态，就地修改分层原件也会使旧井值失效；保护路径不冒充实际使用的父来源。
- 地层厚度的页面默认来源为井分层 MD，自动取同井顶底 MD 差；缺少有效厚度的井
  展示原因并排除。构造面差值保留显式来源选择，成果记录 `factor_source` 和
  `depth_domain`。旧调用未指定来源时保留原构造面契约，避免改变历史 API 语义。
- 砂地比渲染推荐范围为 0–1，等值距 0.1；砂厚/层厚从 0 起，按实际最大值向上
  取 1/2/5 级值域，以米显示，不限制为固定几十米。推荐不改写原始井值和栅格值。
- 间隔/方向线与等值线均可选。导入沿用现有线矢量校验与约束契约；默认 Surfer IDW
  可在无约束线时运行。等值图保存后可直接进入后续编图。

## 验证

- Windows / RelWithDebInfo：`paleo` 与相关测试目标构建通过，编译并行度 `-j8`。
- 同步主分支 `c45f089a` 后，Qt 测试串行执行，8 个相关套件通过：`tst_mappingpages`、`tst_factorworkflow`、
  `tst_singlefactor_acquisition`、`tst_singlefactor_strategy`、`tst_singlefactor_structural`、
  `tst_singlefactor_constraintuse`、`tst_singlefactor_contours`、`tst_shell_chrome`。工作流 49 项通过，
  外部真实工区用例因 `PALEO_REAL_PROJECT_AREA` 未设置而跳过，不作为真实工区验收。
- 新增回归覆盖：重建工作流与图层清单后复用保存结果；强制重算与井点文件变化；
  比值/厚度默认值域及实际 QGIS 渲染器范围；读取 100 m 井层厚、无约束线成图并记录
  MD 口径；分层原件底界从 200 改为 250 后自动得到 150 m 新厚度。
- 完整 `tst_shell_chrome` 20 项通过，包含四组顺序、导入入口、重复成图动作归入更多
  菜单与命令帮助检查。补齐两个线矢量导入入口的帮助目录条目，沿用镜像同步。
- 12 项源码闸门通过：layering strict / symbol / transitive / qgs audit、QGIS spread、
  UI invariants / size / file budget、i18n、UI tokens、QGIS versions、docs drift。
  所有基线保持原值。
- 通过 Qt 真实控件截图检查亮色、暗色、280 px 窄栏与主窗口 ribbon。缩短折叠标题，
  成果操作纵向排列；测试明确断言窄栏默认流程水平滚动范围为 0，固定成图操作可见。
  本地证据位于 `artifacts/single-factor-flow/`，不作为程序资源打包。
