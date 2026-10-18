# 人工古地理编图流程（2026-10-19）

本次将既有三级分类、共边编辑、证据修订与导出连接为人工编图主流程。

- 基础图：空白新建；单因素按因子分类并分级；地震相、历史古地理图及其他相面。
  有分类的栅格先转面，已具备三级属性的矢量建立工作副本；没有三级归属的图仅作参考。
- 新建图：空白 MultiPolygon GeoPackage，工程米制网格，完整分类与稳定身份字段。
  首次闭合绘制选择微相；首次保存冻结覆盖范围。随后通过分割、面内覆盖、合并和共边节点维护分区。
- 依据：显式读取当前可见参考视图实际显示的版本，过滤其他层位及未保存工作图层。
  每次启用保存版本、观察和修改目的的会话快照；新增说明记为人工支持，图件记为参考。
  可以只填人工说明。版本存在性和层位在启用及写入时检查，参考最多 16 个。
- 原子性：新增图及编图页新建的副本保存 requires_edit_evidence 标记。观察器的每次业务编辑
  在原生事务内附加依据；改类同样使用该路径。无依据或来源失效时拒绝并撤回分区操作。
  依据不会确认解释；原有证据与继承链保持待复核。应用的编图模式同时约束既有副本；独立旧接口保持默认兼容。离开编图模式清空会话依据。
- 显示：保留自动／固定相、亚相、微相，以及选中相面锁定；同一套微相分区负责三级聚合。
- 交付：直接检查拓扑并保存，矢量导出预选准确版本；复用数据导出器提供 GPKG／SHP／GeoJSON
  以及版本、来源和 SHA 清单。图件沿用 QGIS PDF／PNG／SVG 标准版面，可登记工区或输出外部。
  活动编辑缓冲或尚未登记的观察器变更拒绝导出；地图输出还检查所有可见图层的编辑状态。
- UI：多输入综合计算与重算默认折叠；详细逐图斑证据修订默认折叠。主页面规模基线缩小，
  不增加 Qgs 散布文件或放宽源码闸门。

验证：独立 worktree/build，vendor QGIS prefix，Debug 构建，编译 `-j8`、ctest `-j4`。
`tst_mappingworkbench`（90 项）、`tst_faciespartition`（13 项）、
`tst_exportpipeline`（20 项）、`tst_paleomaplibrary`（13 项）全绿。
新增覆盖首绘无依据拒绝、证据与几何同撤销、首次保存覆盖范围、来源失效整体回退、
已有副本在编图模式下强制依据、会话切换清空、准确导出版本及页面门控。
通过 layering strict、i18n、ui invariants、git diff --check；检查了浅／深主题截图。

本地既存静态问题（文件未改动）：file_budget 的 constraintpage_interpolation／folderconfirm／
datalist_tree；ui_size 的 datalist_tree；qgis_spread 的 identifytool.{cpp,h}；
ui_tokens 的 identifypopup.cpp setSpacing(2) 及 compositebody 两条过期例外。
本次没有新增上述违规，规模基线下降：mappingworkbenchpage 1668→1637、构造函数 659→629，
attachWorkbench 321→319。
