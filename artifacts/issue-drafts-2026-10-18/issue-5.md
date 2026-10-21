# [med] 相面拓扑状态用裸字符串 layer property 跨层传递，isPartition() 门面被视图三处绕过

## 问题

相面拓扑编辑用「字符串键的 QGIS layer property」充当跨层状态通道：键名以裸字面量散落在 QGIS 封装层、功能层、视图层三个层，无任何头文件声明契约；同批新增的 FaciesPartitionEditor::isPartition() 门面被视图层三处绕过，视图仍直读 QGIS 图层内部自定义属性。

## 证据

- 键名字面量（当前 HEAD 逐行读过）：`src/qgis/faciespartitioneditor.cpp:18-19`（schemaKey / footprintKey）、:104-106 与 :109（paleo.evidenceEditFailure / evidenceEditFailures / partitionOperation）；`src/workflow/mappingworkbench.cpp:1005-1009`（paleo.workbenchBinding）、:1047 与 :1062-1079（paleo/requireFaciesTopology、paleo.partitionCommitRejected）。
- 门面被绕过：`src/ui/edittools/editingtoolbar.cpp:547`、:748、:903 仍写 `target->customProperty("paleo/requireFaciesTopology").toBool()`，而同文件 :904 已用 `FaciesPartitionEditor::isPartition(target)`——同文件内两种口径并存。

## 影响

键名改写（任意一处拼写不一致）静默改变拓扑编辑/提交拒绝语义，无编译期保护；三层共写同一份隐式契约，与「数据只问答不管谁来问」的分层相悖。

## 建议

在 src/qgis（或 domain）建具名常量+访问器头（如 FaciesPartitionState::{operation,failure,commitRejected,binding}），功能层与视图层只经访问器读写，禁止裸字符串键跨层传递状态。

## 验证

`grep -rn 'customProperty("paleo\|setProperty("paleo' src/` 在 src/ui 与 src/workflow 零命中；新增单测覆盖「设置-读取」往返经访问器完成。

## 关联

- 同族：#521 / #522 相面编辑链；docs/UI_LAYER_PLAN.md 分层契约。
