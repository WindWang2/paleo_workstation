# goal/fault-interpretation — 断层解释数据框架与拾取工具
# 源文件/测试集中此处（根 CMakeLists 模块表并行方向期禁改；README 惯例）。

# 断层解释域模型（数据层）
target_sources(paleo_domain PRIVATE
  src/domain/faultset.cpp)

# FaultSet 工程持久化（metadata，project.sqlite fault_set 表）。
# 依赖补充：FaultSet 值类型在 paleo_domain（store→domain 无环：domain 是叶子）。
target_sources(paleo_store PRIVATE
  src/metadata/faultsetstore.cpp)
target_link_libraries(paleo_store PUBLIC paleo_domain)

# 断层解释编排（功能层）：undo 栈 + SelectionContext 联动 + 切割镜像层。
# 依赖补充：SelectionContext 在 paleo_linkage（DAG 无环：workflow→linkage→services）；
# QUndoStack 属 Qt6::Gui。
target_sources(paleo_workflow PRIVATE
  src/workflow/faultinterpretationcontroller.cpp)
target_link_libraries(paleo_workflow PUBLIC paleo_linkage Qt6::Gui)

# 断层管理面板 + 壳接线（视图层）
target_sources(paleo_ui PRIVATE
  src/ui/faults/faultmanagerpanel.cpp
  src/ui/paleomainwindow_faults.cpp)

# ---- 测试 ----

# 模型：实体/棒/切割 CRUD、命名去重、JSON 往返、同断层多层位切割独立存取
add_paleo_test(tst_faultset LIBS paleo_domain)

# 存储：project.sqlite 往返、写队列注入、只读拒绝、坏 JSON 拒绝
add_paleo_test(tst_faultsetstore LIBS paleo_store)

# 编排：原语逐拍 undo/redo、SelectionContext 载荷/回声、多层位切割独立
# 存取、写队列落盘重开、catalog fault 角色链接、切割镜像层
add_paleo_test(tst_faultinterp LIBS paleo_workflow paleo_linkage paleo_store)

# 剖面 UI：IL 拾取落账+回显、时间切片拒拾、任意线身份稳定、联动高亮、
# 面板树/切割意图、保存重开全闭环还原（offscreen）
add_paleo_test(tst_faultsectionui LIBS paleo_ui paleo_workflow paleo_linkage paleo_store)
