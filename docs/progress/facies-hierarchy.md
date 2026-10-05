# 三级相编图（2026-10-05）

基线：`origin/master`（`0f83e5a`）；分支 `codex/facies-hierarchy`，独立 worktree/build。

用户要求的编辑参考图联动、三级多尺度显示与缺层级填充、三级拓扑一致性、
手工证据与版本关联已接入现有 MappingWorkbench。使用说明见
[`FACIES_MAPPING.md`](../FACIES_MAPPING.md)。

数据层 `domain/facieshierarchy` 定义分类路径、填充与尺度阈值；QGIS 层负责
合并分类渲染、覆盖验证和 canvas 联动；功能层 `workflow/mappinghierarchy`
编排分类修改与证据；视图使用原生控件和 DESIGN.md token 发出操作意图。
没有新增顶层模块，也没有复制参考项目源码或数据。

## 验证

- 独立 build，vendored QGIS_PREFIX，`cmake --build build -j8` 完整构建通过。
- 全量 `ctest --test-dir build -j8 --output-on-failure`：294 项均已运行，
  首次 293/294 通过。补正 1:400 万临界值测试断言后重编译测试，
  `ctest --test-dir build --rerun-failed -j8 --output-on-failure` 1/1 通过。
  产品代码与全量运行时相同，最终所有 294 项通过。
- 初版编图测试 19 个用例通过，包括新增的尺度与缺级填充、合并渲染和标签、
  三级改类与原生撤销、覆盖拓扑拒绝、强制共边节点编辑、证据快照与来源谱系、
  canvas 双向同步及解除联动、面板信号。主窗口集成测试也通过。
- 启动 `./paleo-dev selfcheck` 的 8 项检查通过。
- 分层严格检查、i18n、UI token、UI invariants、自检门禁以及本次修改的
  9 个产品代码翻译单元 clang-tidy 通过；`git diff --check` 通过。
- 全套 QTest 有 40 个依赖数据或额外运行开关的用例跳过，包括真工区、恩平
  golden、WebEngine 图形平台、目录规模及可选截图专项。本次新增测试无跳过。
  未设置真工区数据，因此上述结果不代表真工区验收。

本地日志在独立 build 的 `facies-verified-build.log`、`facies-full-tests.log`、
`facies-rerun.log`、`facies-selfcheck.log`、`facies-tidy.log`，未纳入源码。

## 原生界面截图

以下截图由编图面板集成测试生成，使用合成相面及解释证据，已检查文字布局和
浅／深色 DESIGN token；不是业务真工区图件。

| 面板 | 浅色 | 深色 |
| --- | --- | --- |
| 三级显示与编辑 | [截图](facies-hierarchy-shots/hierarchy-light.png) | [截图](facies-hierarchy-shots/hierarchy-dark.png) |
| 解释证据 | [截图](facies-hierarchy-shots/evidence-light.png) | [截图](facies-hierarchy-shots/evidence-dark.png) |
| 数据管理中的图件与参考入口 | [截图](facies-hierarchy-shots/data-reference-light.png) | [截图](facies-hierarchy-shots/data-reference-dark.png) |

## 数据管理与单因素图联动补充（2026-10-05）

修复高级工具生成单因素图后编图参考列表缺少新版本的问题。目录变更合并到
一次事件循环刷新，恢复每个不可变版本的独立图层；稳定的「当前图」别名不再
影响历史版本查询。编图列表和证据来源自动刷新并保留已有勾选。

新增编图 → 数据管理的保存版本定位，以及数据管理 → 编图的版本参考入口；
后者保持当前主图目标、层位和编辑会话。图层的资产／版本关联在保存后更新。
新增两个回归用例覆盖高级工具真实生成、重算后历史文件／勾选／证据不变、
稳定制图别名的历史版本和重开恢复；主窗口集成测试覆盖两个入口、版本对应
及正在编辑时打开参考的行为。新增测试不需要真工区数据。

补充验证：

- 独立构建程序、启动自检与相关测试目标通过，仍使用 vendored QGIS 和 `-j8`。
- 16 项专项最终通过，覆盖编图、主窗口、数据目录、单因素工作流、编图页面、
  数据预览、编辑工具及分层／翻译／UI 门禁。最终一轮先通过 15/16 项；
  主窗口新增属性断言因停靠布局改变控件父对象而查找失败，修正测试后单独
  重跑主窗口测试 1/1 通过。
- 编图 QTest 21 个用例通过，无跳过；主窗口 QTest 35 个用例通过，
  8 个已有可选专项跳过。新增联动用例无跳过。
- 本次补充的 5 个产品代码翻译单元 clang-tidy 通过，启动自检 8 项通过，
  `git diff --check` 通过。数据管理入口的浅／深色截图已核对 DESIGN token。

补充日志在独立 build 的 `facies-data-link-build.log`、
`facies-data-link-tests.log`、`facies-data-link-ui-final.log`、
`facies-data-link-tidy.log` 和 `facies-data-link-selfcheck.log`，未纳入源码。
