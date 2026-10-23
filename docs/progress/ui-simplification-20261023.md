# 常用工作流与界面简化修复（2026-10-23）

基线：`f3998bd4`，工作分支 `codex/simplify-workflows`，独立 worktree/build。
本批完成常用界面减负、已确认的误操作/草稿问题与局部刷新优化。
修改前先核对 DESIGN.md；默认折叠、预测入口、参考视图等行为已同步进规范。
QGIS 继续负责 GIS，未改地质算法或数据格式。

## 交付内容

| 审查项 | 本批修复及边界 |
| --- | --- |
| R01 | 图层 Delete 改为图层树自身的 WidgetWithChildrenShortcut；搜索框/其他页面不再消费图层删除。 |
| R02 | 剖面定位要求唯一匹配的地震资产；支持 payload 的 asset_id；无匹配或多匹配时提示，取消“第一份地震资料”兜底。 |
| R03 | 按层位保留分类草稿；外部分类更新不覆盖 dirty 表单，显示核对提示；工程重绑清空旧工程草稿；保存成功清 dirty。草稿只在当前页面会话中保留，未做重启恢复或自动冲突合并。 |
| R06 | 图件默认筛当前层位，显式勾选“查看全部层位的图件”；共享资料仍可显示。 |
| R07（部分） | 初始预测类型改测井；Mock 保留显式入口。完整的 Mock 前置检查引导尚待统一。 |
| R08 / V06 | 样式、来源与参数、分类维护默认折叠，参考帮助收进工具提示；常用任务输入和操作保留。 |
| R10 | 视图切换统一经过筛选/分页逻辑；树和虚拟表不显示外部分页。 |
| R11 | 分类树改为单次事件循环合并更新；批量加载只重建一次。 |
| R12 | 不再随每次筛选重建隐藏的图标、分组和虚拟模型；隐藏选区不能把已筛掉的资产带进批量命令。 |
| R13 | 低分辨率预览优先复用当前身份/视野缓存，未命中直接启动 QGIS 正式异步渲染；去掉 GUI 同步 renderSnapshot。 |
| R14（部分） | “设为主文件”接入既有共享 SetPrimaryCmd，支持撤销/重做，延迟执行避免拆除发信号的按钮；数据操作命令整体移出 ui 仍待架构收口。 |
| R15 | 第一份通知方案标为历史提案，现行方向 64 为唯一契约，保留历史而消除实现歧义。 |
| V01 / V02 | 等宽数字增加中文字体回退；深色图标只提亮暗色中性描线，保留彩色图标像素，修复逐通道加亮饱和。 |
| V03 | 参考图按两列伸缩，第三张进入下一行，超出区域滚动；保留主图工具、参考版本与联动关系。 |
| V04 / V05 | 还原 dock 意愿后进入首页，收起空工程面板；编辑工具/编辑会话随编辑目标显示，图层选择入口保留。未强制重置用户保存的窄屏布局。 |
| V07 | 资产批量动作归入“选项”；虚拟工区节点及多选概要隐藏实体 CRUD。 |
| V08 | 验证清单显示级别和可换行的信息；代码/图层保留在工具提示及可复制详情；空残差表隐藏。 |
| V09 / V10 | 剖面低频项进入“显示与解释…”；属性建模仅显示当前方法参数，对象参数随勾选显示，切换保留值。 |
| V11 / V12 | 删除版面底部重复缩放按钮；Web 失败面增加“重试连接”，消费既有连接 QAction 的可用状态。 |

## 运行验证与截图

- Windows 本机 Qt 6.11.2 / QGIS 前缀，Ninja RelWithDebInfo，构建并行度最多 8；Qt 测试串行。
- 完整清理重编译主程序及 11 个相关测试目标；10 组 CTest 全部通过：mappingpages、shortcuthelp、uxtheme、panels_dataops、previewmap_page、seismic_sectionui、layoutshell、webviewpanel、stratigraphicweb、shortcuts_shell。
- 新增回归覆盖：改名/删行后的分层位草稿、外部更新和工程重绑；树刷新次数；算法参数显隐和值保留；筛选排除隐藏选区；实体面板“设主”撤销/重做；中文 glyph 覆盖和彩色图标像素；重试动作禁用/启用和触发。
- Qt/QGIS 截图集成：workflowAuditSnapshots、secondarySurfaceAuditSnapshots、mappingWorkbenchCanvasRibbonAndReferences、faciesPartitionRibbonMirrorsTargetAndAvailability 全部通过，包含实际合成栅格/相面、参考视图、编辑与保存链。Mock 资料明确标记，不作为生产算法质量证据。
- Windows 原生平台再跑 workflow/secondary 两场景；2x 离屏再跑 workflow，全部通过。原生截图为 2160×1350，基础 workflow 为 1440×900，2x 为 2880×1800。图像直接保存，未修改像素。
- 本地证据：`artifacts/fixes-20261023/` 下 build、ctest、各场景 QtTest 文本及原图；`comparison/index.html` 提供 10 组前后对照和参考图新状态。前次半渲染的参考图未作为前图。
- 没有进行真实大工程的响应延迟/RSS 基准；局部性能结论限于去除重复工作和同步渲染路径，不能推导整体加速倍数。

## Windows 增量构建根因修复

截图流程曾在析构时触发 0xC0000374。CDB 定位到 MappingWorkbenchPage 的新旧类布局混用；对象文件早于新头文件，Ninja 没有登记该头依赖。仅靠独立组件测试通过不足以排除这一问题。

根因是本地化 MSVC /showIncludes 与 CMake 3.29 生成的 msvc_deps_prefix 字节不一致；缺英文 clui.dll 时 VSLANG=1033 仍输出中文。paleo-dev.ps1 现用实际编译器和真实 CMake→Ninja 夹具验证头文件确实进入 deps 数据库，再修正生成文件的前缀。首次修复清理旧对象，后续前缀相同保留增量构建；清理失败撤销记录，下一次必须重试。

已完整重建并再次通过截图集成。真实 paleomainwindow_workbench.cpp.obj 的 Ninja deps 为 VALID，包含 mappingworkbenchpage.h；PowerShell 7 与 Windows PowerShell 5.1 自检通过，5.1 的真实编译器/Ninja 夹具也验证了头依赖与二次不清库。脚本增加 UTF-8 BOM，修复 5.1 把中文脚本当系统代码页解析的既有问题。构建记录同时保留错误前缀诊断，未把失败尝试当成成功证据。

## 架构闸门与待完成项

layering strict、symbol audit、带 QGIS_PREFIX 的 transitive、qgs audit、qgis spread、UI invariants、UI size、file budget、i18n、UI tokens、QGIS versions、docs drift 均通过。所有基线只下调：file-budget 六个冻结值下调；ui-size 五个值下调（其中两项是旧代码已收敛的记录对账），条目计数头注 11→8。Qgs 直连/散布冻结基线分别维持 137/172，未补登违规。

以下审查项仍未完成，明确进入 TODOS.md：R04 影响分析/版本对比后台任务化；R05 旧新工作流能力迁移与统一；R09 保存作用域模型；R07 完整前置条件引导；R14 数据操作编排从视图目录迁出。它们涉及任务生命周期、跨入口业务状态或持久化契约，本批没有用隐藏控件或后台直接调用 live QObject 冒充解决。
