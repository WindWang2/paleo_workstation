# UI 整体审核与工作流改进

日期：2026-10-02。分支：`codex/ui-workflow-polish`。

## 范围与证据

按 `DESIGN.md` 审核启动页、五步工作流、浅深主题、窄停靠面板、数据预览及
预测→上图→复制编辑→保存版本的接线。截图来自本轮原生 Qt/QGIS 控件树，
使用临时工程与合成资料；五页对照均为 1440×900。每张原图均已打开检查。
架构边界沿用 `docs/UI_LAYER_PLAN.md`：页面发意图，工作流及 QGIS 服务执行功能。

修前截图基线为开始审核时的 `4f35600`；提交前已整合主分支 `56d643c`
（catalog SQLite、多文件测井、单因素功能），修后截图与回归来自整合后的代码。
这些功能本身来自主分支，本轮变更集中在下表的界面及操作接线。

保留并确认的优点：工作流链持续可见；参数与高级工具已分开；QGIS 原生图层树、
画布与停靠管理保持专业工具密度；地图数据符号与 UI 主题有明确边界。

## 逐步审核

| 步骤 | 用户任务与修前问题 | 改进与修后状态 | 浅色对照 | 深色对照 |
|---|---|---|---|---|
| 0 启动 | 新建/打开入口在长最近工程列表下方，视觉优先级弱 | 操作移至标题下方；新建使用主按钮与原生图标；最近工程显示名称及目录。入口清晰 | [前](ui-workflow-audit-shots/before/00-startup.png) / [后](ui-workflow-audit-shots/after/00-startup.png) | 五页覆盖双主题 |
| 1 数据管理 | 预览空态只指向列表；属性区提示垂直散布，数据页还指向地图操作 | 空态明确「导入→左侧列表选择」；属性提示靠顶部并指向数据列表。基本流程正常 | [前](ui-workflow-audit-shots/before/01-data-light.png) / [后](ui-workflow-audit-shots/after/01-data-light.png) | [前](ui-workflow-audit-shots/before/01-data-dark.png) / [后](ui-workflow-audit-shots/after/01-data-dark.png) |
| 2 预测编图 | 空输入无下一步提示；地震模式仍写全选井；部分选中后全选会清空；结果操作与样式设置混排 | 输入/结果分别给引导；选择按钮按类型及选择状态变化；运行期输入锁定、可取消；低频设置归入可折叠组，按规范默认展开。选择与刷新回归通过 | [前](ui-workflow-audit-shots/before/02-predict-light.png) / [后](ui-workflow-audit-shots/after/02-predict-light.png) | [前](ui-workflow-audit-shots/before/02-predict-dark.png) / [后](ui-workflow-audit-shots/after/02-predict-dark.png) |
| 3 单因素图 | 等值线间隔放在生成栅格参数之前；没有样点时缺少指引 | 间隔紧邻等值线动作；缺样点给出下一步；主操作与图件/版本分组。流程顺序清晰 | [前](ui-workflow-audit-shots/before/03-constraint-light.png) / [后](ui-workflow-audit-shots/after/03-constraint-light.png) | [前](ui-workflow-audit-shots/before/03-constraint-dark.png) / [后](ui-workflow-audit-shots/after/03-constraint-dark.png) |
| 4 智能编图 | 没有选中项时优先级按钮仍可点；刷新重设列表高度与当前输入；标注选项被其他操作覆盖 | 边界/空选禁用原因可见；列表高度稳定；保留当前项和未应用标注；来源、相分类及标注使用 QGIS 折叠组。状态联动正常 | [前](ui-workflow-audit-shots/before/04-compose-light.png) / [后](ui-workflow-audit-shots/after/04-compose-light.png) | [前](ui-workflow-audit-shots/before/04-compose-dark.png) / [后](ui-workflow-audit-shots/after/04-compose-dark.png) |
| 5 验证 | 未运行提示塞进跨列表格行，窄栏被裁切；只有鼠标双击定位；刷新可能保留旧剖面载荷 | 独立换行提示与问题计数；整行只读选择；回车/双击定位；重跑先清剖面目标。定位与残差联动回归通过 | [前](ui-workflow-audit-shots/before/05-validate-light.png) / [后](ui-workflow-audit-shots/after/05-validate-light.png) | [前](ui-workflow-audit-shots/before/05-validate-dark.png) / [后](ui-workflow-audit-shots/after/05-validate-dark.png) |

## 跨页面问题与修复

- **P1 保存一次增加两个版本**：`QgisEditingService` 同步发送提交回调，工具栏在
  回调和返回路径各发一次 `editingStopped`。会话结束统一经幂等入口消费，保存、
  取消及外部提交/回滚各通知一次；完整编图测试确认一次保存只递增一个版本。
- **P2 窄图层栏裁切**：长工具栏最小宽度撑宽图层树。工具栏独立横向滚动；
  树使用停靠栏剩余宽度，空态按宿主尺寸约束并随文本变化重排。
- **P2 嵌套滚动**：完整编图参数展开后，页面滚动区与停靠栏滚动区会同时出现。
  编图页统一交给停靠栏视口处理溢出，滚轮只有一个页面滚动范围；页面栈只按
  当前页测量，隐藏的长表单不会撑高验证页或数据属性页。长/短页切换回归通过。
- **P2 主题/规范偏离**：旧 caption 只在创建时取浅色；属性标题滥用交互蓝，
  字号为规范外的 10pt；折叠段 hover 硬编码规范外颜色。改为活体主题、
  中性标题 12pt 与既有 token。失败/降级提示使用有足够文字对比度的深色语义 token。
- **P2 胶囊未渲染**：Qt QSS 不会把 `9999px` 自动夹到控件半高，层位按钮呈矩形。
  按字体、内边距与边框计算半高圆角；两主题下选中层位均呈胶囊。
- 操作反馈现在说明实际结果：显示、参考窗口、标注、取消、分类及绘制分别提示；
  不再把所有动作都称作「图件与版本已更新」。动态图件说明使用纯文本显示。

## 验证

- 构建应用及 10 个相关测试目标成功：Qt 6.11.2、GCC 16、vendored QGIS prefix，
  Ninja / RelWithDebInfo。
- CTest **18/18 通过**：主窗口、页面、主题、编图工作台、编辑工具、预览、
  多文件测井 UI、图层树、三视图联动，以及 layering / UI invariants / i18n 的严格检查与自测。
- `check_tidy.py --base origin/master --build-dir build`：10 个改动的产品翻译单元通过。
  本地 clang-tidy 23.1.1；CI 继续使用仓库钉定的 clang-tidy 20。
- 启动及五页的 11 组同尺寸截图已逐张比较；另用完整主窗口测试运行
  预测→参考窗口→相面→编辑→保存，确认图层、画布、ribbon 与版本同步。
  预测结果对照为 1600×1000：[前](ui-workflow-audit-shots/before/prediction-result.png) /
  [后](ui-workflow-audit-shots/after/prediction-result.png)。其中结果为模拟输入的真实程序输出。
- `git diff --check` 通过；中文翻译骨架只补本轮相关上下文，保留其他条目。

复现截图：配置并构建 `tst_ui` 后，使用同一 vendored QGIS prefix：

```bash
QT_QPA_PLATFORM=offscreen QGIS_PREFIX_PATH="$QGIS_PREFIX" \
  LD_LIBRARY_PATH="$QGIS_PREFIX/lib" \
  XDG_CONFIG_HOME=/tmp/paleo-audit-home/config \
  XDG_DATA_HOME=/tmp/paleo-audit-home/data \
  PALEO_WORKFLOW_CAPTURE=/tmp/paleo-workflow-audit \
  ./build/tst_ui workflowAuditSnapshots
```

`QGIS_PREFIX` 指向本机已构建的 QGIS 安装前缀；截图用例使用临时工程，
运行普通测试时不设置截图变量即可跳过取证。

## 证据边界

本轮覆盖主窗口及五页，并对相关编辑、数据预览、验证联动做自动化回归。
截图可证明布局、文案与主题状态；未声称完整 WCAG 合规。屏幕阅读器、真实窗口
系统的拖放、操作系统高 DPI、Windows 与 OpenGL 三维地震视口仍需相应平台验收。
预测截图使用明确标示的模拟数据，不能据此判断地质算法或真实数据质量。
