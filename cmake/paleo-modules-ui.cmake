# cmake/paleo-modules-ui.cmake — 模块库：功能层 + 视图层 + 组装根 + 兼容伞 + unity 开关
# 由根 CMakeLists.txt 按顺序 include()（纯结构拆分，语义与拆分前逐行一致）。
# 注意：include() 不开新作用域，CMAKE_SOURCE_DIR 仍是仓库根。
# 文件名刻意不用 extra- 前缀——extra-*.cmake 受 _paleo_extra_manifest 防漏对账。

# ---- 功能层 ----
add_library(paleo_linkage STATIC
  src/linkage/selectioncontext.cpp
  src/linkage/wellmaplink.cpp
  src/linkage/seismicmaplink.cpp
  src/linkage/threewaylocator.cpp                  # 问题→三视图联动
)
target_link_libraries(paleo_linkage PUBLIC paleo_deps paleo_services paleo_qgis paleo_domain)

add_library(paleo_workflow STATIC
  src/workflow/paleomapimport.cpp
  src/workflow/paleomaplibrary.cpp
  src/workflow/projectareasummary.cpp
  src/workflow/mappingeditcontext.cpp
  src/workflow/mappingbasemaps.cpp
  src/workflow/geologicalexport.cpp
  src/workflow/dataexportworkflow.cpp
  src/workflow/datagraphworkflow.cpp
  src/workflow/derivationworkflow.cpp
  src/workflow/officepreviewsession.cpp
  src/workflow/projectlayerrefresh.cpp
  src/workflow/xmlpreviewsession.cpp
  src/workflow/stratigraphicwebsession.cpp
  src/workflow/mappingworkbench.cpp
  src/workflow/mappingworkbench_readiness.cpp     # 预测专项：测井批量准备检查（干跑）
  src/workflow/mappingworkbench_predict.cpp       # 预测管线（predict/finish/任务行）——core-deepening A-R2 拆分
  src/workflow/mappingworkbench_edit.cpp          # 编辑副本/资产移除/约束导入——core-deepening A-R2 拆分
  src/workflow/mappingworkbench_compose.cpp       # 融合合成管线——core-deepening A-R2 拆分
  src/workflow/mappingworkbench_polygonize.cpp     # 相栅格转面：同步 + 后台任务（facies-ux 1）
  src/workflow/mappingworkbench_consistency.cpp   # 地震相×测井相一致性报告（facies-ux 4）
  src/workflow/mappingworkbench_impact.cpp       # G05：证据变化空间影响分析（三级分明 + 报告快照）
  src/workflow/evidencerecord.cpp                 # 证据链组装（facies-ux 5）
  src/workflow/wellpredictionservice.cpp
  src/workflow/workflows.cpp
  src/workflow/constraintimport.cpp                 # WS-C：约束 SHP 角色化导入（方向/打断）
  src/workflow/validationworkflow.cpp                # 方向20 轮4：④验证（按类边界析出）
  src/workflow/compositionworkflow.cpp               # 方向20 轮4：③综合编图（按类边界析出）
  src/workflow/predictionworkflow.cpp              # 方向20 轮4：①智能预测（按类边界析出）
  src/workflow/constraintworkflow.cpp              # 方向20 轮4：②约束与单因素（按类边界析出）
  src/workflow/constraintwellfactors.cpp
  src/workflow/constraintfactorjobs.cpp            # 方向20 轮4：②的单因素作业面；方向57 只留统一异步面
  src/workflow/constraintfactorjobs_factor.cpp     # 方向57：基础因子生成族（idw/isopach/welldist/structural/surfer）
  src/workflow/constraintfactorjobs_localdirection.cpp # 方向57：局部方向族三段式
  src/workflow/constraintfactorjobs_geostat.cpp    # 方向57：克里金/SGS 族三段式
  src/workflow/constraintfactorjobs_contours.cpp   # 方向57：分析等值线族 + 制图工作场
  src/workflow/constraintfactorjobs_interpretive.cpp # 方向57：解释性等值线族三段式
  src/workflow/folderimport.cpp                    # W2 文件夹/单文件导入编排
  src/workflow/projectopen.cpp                     # W2 打开/新建工程编排
  src/workflow/registration.cpp                    # W3 临时配准编排
  src/workflow/mappingworkflow.cpp                 # 厚度→约束IDW→相多边形链路
  src/workflow/mapexport.cpp                       # 层位图 PDF 导出
  src/workflow/layouttemplatelibrary.cpp            # 版式模板库编排（qpt→catalog 版本 + 库表）
  src/workflow/horizonbatchexport.cpp               # 按层位组批量出图（快照换图+登记 OUTPUT）
  src/workflow/mapbook.cpp                         # 地图册：AOI 格序列 + 模板变量（纯编排）
  src/workflow/mapbookqueue.cpp                    # 地图册：批量导出队列（任务服务 + 逐版记账）
  src/workflow/mapversioncontroller.cpp            # 保存版本/发布编排
  src/workflow/aiassistworkflow.cpp
  src/workflow/derivedassets.cpp                   # 派生产物 + DERIVED 版本登记
  src/workflow/algorithmpublish.cpp                # algo-atom P1：publishArtifacts 统一发布面
  src/workflow/atomicjob.cpp                       # algo-atom：Processing run+publish 薄助手
  src/workflow/algorithmadapters.cpp               # algo-atom P2：Processing/NativeCpp 适配器
  src/workflow/welltopseditorworkflow.cpp           # 方向 32：分层编辑版本化提交/回滚/合并编排
  src/workflow/wellsitingworkflow.cpp               # 方向34：井网辅助编排（诊断/候选/方案/planned）
  src/workflow/storagegovernancecontroller.cpp
  src/workflow/assetassociation.cpp
  src/workflow/assetops.cpp                        # 方向30：版本回滚/对比 + 物理清理 + 未决归位
  src/workflow/importledger.cpp                    # 方向30：文件夹导入批次台账（sidecar 持久化）
  src/workflow/boundaryeditrules.cpp               # 方向39：相界类型编辑规则（赋类/带域/开放端）
  src/workflow/wellattachmentops.cpp               # 方向79：井附件锚深后补编辑编排（校验/列表面）
)
target_link_libraries(paleo_workflow PUBLIC paleo_deps paleo_qgis paleo_store paleo_io paleo_services paleo_domain paleo_ai)

# ---- 视图层 ----
add_library(paleo_ui STATIC
  src/ui/paleostartupscreen.cpp
  src/ui/paleomainwindow_paleomaps.cpp
  src/ui/pages/datalist_paleomaps.cpp
  src/ui/pages/datalist_auxnav.cpp
  src/ui/pages/datalist_importbuttons.cpp
  src/ui/pages/mappingeditbasis.cpp
  src/ui/pages/mappingbasemapselector.cpp
  src/ui/datapreview/projectstatisticspage.cpp
  src/ui/datapreview/datapreviewtabdocument.cpp
  src/ui/pages/datagraphsceneitems.cpp
  src/ui/pages/datagraphpage.cpp
  src/ui/datapreview/officepreviewwidget.cpp
  src/ui/datapreview/datapreviewtabxml.cpp
  src/ui/notifications/notificationcenter.cpp      # 方向64：错误呈现（通知卡/severe 模态/状态栏）
  src/ui/notifications/toastcard.cpp               # 方向64：通知卡（预建池复用）
  src/ui/notifications/paleonotify.cpp             # 方向64：视图层错误/确认统一出口
  src/ui/notifications/errorhistorypanel.cpp       # 方向64：错误历史面板（过滤/复制/清空）
  src/ui/pages/stratigraphicwebpage.cpp
  src/ui/symbols/symbolpickerpanel.cpp             # 方向31：符号选择器面板（花纹/线型浏览 + 语义过滤）
  src/ui/pages/mappingworkbenchpage.cpp
  src/ui/pages/mappingworkbenchpage_flow.cpp
  src/ui/pages/mappingworkbenchpage_references.cpp
  src/ui/pages/mappingworkbenchpage_schema.cpp
  src/ui/pages/mappingreferencepanel.cpp
  src/ui/paleomainwindow_references.cpp
  src/ui/paleomainwindow_workbench.cpp
  src/ui/paleomainwindow_workbench_editflow.cpp
  src/ui/paleomainwindow_workbench_commands.cpp   # #473：命令分发链——attachWorkbench 行为守恒拆分
  src/ui/paleomainwindow_workbench_evidence.cpp   # #473：证据面板/图谱联动接线
  src/ui/paleomainwindow_workbench_wells.cpp      # #473：井面板 dock/自动保存/修订链
  src/ui/paleomainwindow.cpp
  src/ui/paleomainwindow_docks.cpp               # 壳构建：中央面与 dock 族（方向83）
  src/ui/paleomainwindow_status.cpp              # 状态栏族（方向83）
  src/ui/paleomainwindow_errorhub.cpp            # 错误呈现域（方向64/83）
  src/ui/paleomainwindow_ribbon.cpp              # ribbon 骨架与面板菜单（方向83）
  src/ui/paleomainwindow_shortcuts.cpp           # 页快捷键注册（方向63/83）
  src/ui/paleomainwindow_pages.cpp               # 页切换域（方向83）
  src/ui/paleomainwindow_project.cpp             # 工程打开/导入编排入口（方向83）
  src/ui/paleomainwindow_attach.cpp                # 壳接线聚合胶水层
  src/ui/paleomainwindow_attach_data.cpp           # 数据管理页接线
  src/ui/paleomainwindow_attach_predict.cpp        # 智能预测页接线
  src/ui/paleomainwindow_attach_constraint.cpp     # 约束与单因素页接线
  src/ui/paleomainwindow_attach_compose.cpp        # 综合编图页接线
  src/ui/paleomainwindow_attach_validate.cpp       # 验证与布井辅助接线
  src/ui/paleomainwindow_attach_shell.cpp          # 壳面与全局组件接线
  src/ui/paleomainwindow_attach_mapping.cpp        # 编图流水线与版本接线
  src/ui/paleomainwindow_attach_models.cpp         # 属性建模/相图/时深转换接线
  src/ui/ribbonpanels.cpp                          # W4 ribbon 命令组构建迁出
  src/ui/faciesdisplaycontrol.cpp
  src/ui/pages/datalist.cpp                        # W5 数据页·列表侧（胶水层）
  src/ui/pages/datalistops.h                       # 数据页·辅助类型与持久化工具
  src/ui/pages/datalist_table.cpp                  # 数据页·表格展现与行内动作
  src/ui/pages/datalist_tree.cpp                   # 数据页·树形层级与分组
  src/ui/pages/datalist_select.cpp                 # 数据页·选中集管理与联动
  src/ui/pages/datalist_filter.cpp                 # 数据页·多维组合过滤与搜索
  src/ui/dialogs/geologicalexportdialog.cpp
  src/ui/dialogs/dataexportdialog.cpp
  src/ui/pages/datalist_batch.cpp                  # 数据页·批量操作与上下文菜单
  src/ui/pages/datalist_association.cpp
  src/ui/pages/datalist_dnd.cpp                    # 数据页·拖拽与外部文件处理
  src/ui/pages/datalist_undo.cpp                   # 数据页·撤销重做与治理对话框
  src/ui/pages/entitypanel.cpp                     # W5 数据页·实体视图侧
  src/ui/pages/wellsitingpanel.cpp                 # 方向34：布井辅助面板（验证页页签）
  src/ui/pages/datapage.cpp                        # W5 DataPage 兼容薄壳
  src/ui/pages/wellreadinesspanel.cpp               # 预测专项：测井批量准备检查面板
  src/ui/pages/predictpage.cpp                     # W5b ①智能预测
  src/ui/pages/constraintpage.cpp                  # W5b ②约束与单因素（方向96：ctor 编排 + 因子清单面）
  src/ui/pages/constraintpage_wellfactors.cpp      # 方向96：井点因子域 TU
  src/ui/pages/constraintpage_interpolation.cpp    # 方向96：插值方法参数域 TU
  src/ui/pages/constraintpage_generate.cpp         # 方向96：生成动作/等值线域 TU
  src/ui/pages/constraintpage_constraints.cpp      # 方向96：约束线域 TU
  src/ui/pages/constraintpage_sections.cpp         # 方向96：页尾段/反馈接线域 TU
  src/ui/pages/composepage.cpp                     # W5b ③综合编图
  src/ui/pages/validatepage.cpp                    # W5b ④验证
  src/ui/typedconstraintdrawcontroller.cpp         # m2(B)：物源线/展布线/控制点捕获
  src/ui/locator/paleolocatorfilters.cpp
  src/ui/layoutdesignershell.cpp
  src/ui/maptools/paleomaptools.cpp
  src/ui/maptools/sitingpicktool.cpp               # 方向34：地图布点拾取工具
  src/ui/maptools/identifypopup.cpp
  src/ui/maptools/paleoshapetools.cpp              # wave/shapetools
  src/ui/correlationpanel.cpp
  src/ui/correlation/correlationtrack.cpp          # wave/correlation
  src/ui/correlation/correlationwellcolumn.cpp
  src/ui/correlation/curvebrowser.cpp
  src/ui/correlation/depthruler.cpp
  src/ui/correlation/horizonmarkers.cpp
  src/ui/decorations/paleodecorations.cpp
  src/ui/edittools/editingtoolbar_partition.cpp
  src/ui/edittools/editingtools.cpp                # wave/edit-tools
  src/ui/edittools/editingundostack.cpp
  src/ui/edittools/vertexpartitionvalidation.cpp
  src/ui/edittools/vertexeditortools.cpp
  src/ui/edittools/editingtoolbar.cpp
  src/ui/vertexeditorshim.cpp
  src/ui/layout/layoutitempalette.cpp              # wave/layout-designer
  src/ui/layout/layoutitempanel.cpp
  src/ui/layout/layoutexportactions.cpp
  src/ui/layout/layouttemplates.cpp
  src/ui/layout/layoutundostack.cpp
  src/ui/layout/layoutitemtree.cpp                 # 方向25：元素树（原生 QgsLayoutModel 视图）
  src/ui/layout/mapbookcontroller.cpp               # #148：地图册面板 ↔ 批量导出队列接线
  src/ui/layout/mapbookpanel.cpp                    # goal/mapbook-reporting：批量参数面板（只发信号）
  src/ui/datapreview/datapreviewtabs_content.cpp    # 方向97：buildContent 总分发析出
  src/ui/datapreview/datapreviewtabs_survey.cpp     # 方向97：测区全景（openSurveyArea）析出
  src/ui/datapreview/datapreviewtabs_basemap.cpp    # 测区全景在线底图切换（街道/卫星/地形）
  src/ui/datapreview/datapreviewtabs_well.cpp       # 方向97：井过滤族正文 + LAS 挂起应用析出
  src/ui/datapreview/datapreviewtabs_overlay.cpp    # 方向97：D2.10 同目录组图析出
  src/ui/datapreview/datapreviewtabs.cpp           # §4 data-page preview tabs
  src/ui/datapreview/datapreviewtabseismic.cpp     # 方向20 轮4：seismic 预览分支析出
  src/ui/datapreview/datapreviewtabhorizon.cpp      # 方向20 轮4：horizon 预览分支析出
  src/ui/datapreview/datapreviewtabwelllog.cpp      # 方向20 轮4：well_log 预览分支析出
  src/ui/datapreview/datapreviewtabimage.cpp         # 方向20 轮4：image_reference 分支析出
  src/ui/datapreview/datapreviewtabgeojson.cpp        # 方向20 轮4：geojson 分支析出
  src/ui/datapreview/datapreviewtabworkbook.cpp       # outsource_workbook 原生表格预览 + LO 打开
  src/ui/datapreview/datapreviewtabmapproduct.cpp     # 成果图件（相栅格/相面矢量/单因素）分支析出
  src/ui/datapreview/previewmappage.cpp            # P2 D1.x/D3.x 预览地图组合页（工具条/状态条/侧栏/鹰眼/书签）
  src/ui/datapreview/previewtocpanel.cpp           # P2 D4.x 迷你 TOC + 符号快调 + 图例
  src/ui/datapreview/previewidentifypanel.cpp      # P2 D7.x identify 结果面板 + 全表模式
  src/ui/datapreview/previewprofilepanel.cpp       # P2 D5.1-5.4 层位剖面图面板
  src/ui/datapreview/previewhistogramwidget.cpp    # P2 D2.3/D5.8 直方图小图
  src/ui/datapreview/previewmapstates.cpp          # P2 D1.7/D2.x 状态页 + 会话记忆
  src/ui/dialogs/folderconfirm.cpp                 # W2 文件夹导入确认表
  src/ui/dialogs/projectmapsettingsdialog.cpp
  src/ui/welltops/welltopseditordialog.cpp          # 方向 32：分层表编辑器
  src/ui/welltops/welltopsmergedialog.cpp           # 方向 32：导入融合逐行取舍
  src/ui/dialogs/storagegovernancedialog.cpp
  src/ui/dialogs/cataloghealthdialog.cpp           # 方向30：工区体检对话框（分类计数 + 跳转意图）
  src/ui/dialogs/depthanchordialog.cpp            # 方向79：井附件锚深编辑（预览+输入+清锚）
  src/ui/pages/wellattachmentpanel.cpp           # 方向79：井附件管理面板（清单/行内编辑/软删意图）
  src/ui/webviewpanel.cpp                          # embedded web shell (lazy, degradable)
  src/ui/wellcomposite/wellcompositetrack.cpp      # ResFormStar 多道综合柱状图（D1.11 道头基类）
  src/ui/wellcomposite/trackpatterns.cpp           # 方向 66：道图案工厂（岩性/相纹理）
  src/ui/wellcomposite/trackcolumns.cpp            # 方向 66：基础六道（标尺/文本/地层/岩性/取芯/图片）
  src/ui/wellcomposite/trackcurves.cpp             # 方向 66：曲线充填与符号道
  src/ui/wellcomposite/trackcompound.cpp           # 方向 66：地层系统组/沉积相复合道
  src/ui/wellcomposite/wellcompositecanvas.cpp     # 画布主控件（视口/缩放/深度协调）
  src/ui/wellcomposite/compositeheader.cpp         # 方向 66：置顶道头（拖拽换位/分隔线/焦点环）
  src/ui/wellcomposite/compositebody.cpp           # 方向 66：道体渲染与手势
  src/ui/wellcomposite/wellcompositepanel.cpp      # 综合图总装面板（编排根）
  src/ui/wellcomposite/panelsetup.cpp              # 方向 66：面板 UI 搭建
  src/ui/wellcomposite/paneldata.cpp               # 方向 66：数据装配（XML/LAS→道布局）
  src/ui/wellcomposite/paneltracks.cpp             # 方向 66：道编排与会话同步
  src/ui/wellcomposite/paneldepth.cpp              # 方向 66：深度交互（书签/pin/TWT/读数）
  src/ui/wellcomposite/paneledit.cpp               # 方向 66：编辑会话与导出
  src/ui/wellcomposite/curveconfigdialog.cpp
  src/ui/wellcomposite/wellpositionlegendwidget.cpp
  src/ui/sequenceframeworkcolumn.cpp               # 方向28：格架柱状视图（层级色带）
  src/ui/sequenceframeworkpanel.cpp                # 方向28：格架树/标志层/诊断/建议面板
  src/ui/horizonchipbar.cpp                        # 8层位chip条（阶段E）
  src/ui/releasepanel.cpp
  src/ui/taskpanel.cpp
  src/ui/attributetablepanel.cpp
  src/ui/constraintdrawcontroller.cpp
  src/ui/layers/layertreepanel.cpp                 # 图层树面板
  src/ui/layers/layerpropertiesdialog.cpp          # 原生属性壳 + 业务页
  src/ui/seismic3d/seismiccameracontroller.cpp     # 3D 视口相机/渲染器
  src/ui/seismic3d/seismicslicerenderer.cpp
  src/ui/seismic3d/volumeframerenderer.cpp
  src/ui/seismic3d/seismic3dviewportwidget.cpp
  src/ui/seismic3d/seismic3dviewpanel.cpp
  src/ui/seismic3d/seismic3dviewpanel_volume.cpp            # 换体入口 TU（full-audit-20261020 拆分）
  src/ui/seismicsection/seismicsectioncanvas.cpp   # 2D 任意线/井剖面 dock
  src/ui/seismicsection/seismicsectiondockwidget.cpp
  src/ui/paleotheme.cpp                            # DESIGN.md 字体/色 token
  src/ui/paleoicons.cpp                            # ribbon 图标出口
  src/ui/paleoribbon.cpp                           # SARibbon 壳公用件
  resources/seismic_shaders.qrc
  resources/paleo_fonts.qrc
)
target_link_libraries(paleo_ui PUBLIC paleo_deps paleo_ui_deps paleo_qgis paleo_workflow paleo_services
  paleo_store paleo_linkage paleo_domain paleo_io paleo_ai paleo_saribbon paleo_sbm paleo_qgis_attribute_table)

# ---- 组装根 ----
add_library(paleo_app STATIC src/app/appcontext.cpp src/app/appcontext_layers.cpp src/app/onnxwiring.cpp)  # appcontext_layers：图层刷新簇析出（core-deepening A-R2）；onnxwiring：模型注册表装配（方向83 装配根瘦身）
target_link_libraries(paleo_app PUBLIC paleo_ui paleo_workflow paleo_linkage paleo_services
  paleo_io paleo_qgis paleo_store paleo_domain paleo_ai)

# ---- 兼容伞：旧链接方零改动，实际分派各模块 ----
add_library(paleo_core INTERFACE)
target_link_libraries(paleo_core INTERFACE
  paleo_app paleo_ui paleo_workflow paleo_linkage paleo_services paleo_io
  paleo_qgis paleo_store paleo_algorithms paleo_domain paleo_ai
  paleo_saribbon paleo_sbm paleo_segyio paleo_glm ${GDAL_LIB} paleo_ui_deps)
target_include_directories(paleo_core INTERFACE ${CMAKE_SOURCE_DIR}/src ${GDAL_INCLUDE_DIR})

# ---- 实验性 unity build（devex T6）：PALEO_UNITY_BUILD=ON 时仅逐产品 target ----
# vendor 三家（sbm/segyio/saribbon）显式排除——见顶部 option 注释。
# 实测结论（方向70，2026-10-07，MSVC/Ninja 10 目标全编过）：早期同名匿名
# helper（setError×35 / connectionNameFor×8 / ensureOpen×7 / kNan / cluster
# stopped·report·failure / geosArea / ensureGdalRegistered / addIssue /
# sha256Hex / sfpkg 常量 / GridSpec·readGrid / seismic3d Normalize 族 /
# windows 宏撞名 slots·small 等）已按「同构→共享 internal 头（*_internal.h
# ×15）/语义不同→逐文件改名」收拢，UNITY_BUILD=ON 在 BATCH_SIZE 8 下全目标
# 编译+链接通过。已知余量（mega-TU 超集分析，当前分批未触发、源序重排可
# 复暴露，见 devex.md「Unity 实测」）：cluster valid/distance、PALEO_NODATA
# 宏、datapreview qssHeight 重载歧义、previewhistogramwidget QFont helper。
# 默认仍 OFF：未做全量 ctest 长跑对照，收益（链接期 TU 数）待量化后再定。
if(PALEO_UNITY_BUILD)
  foreach(_t paleo_domain paleo_algorithms paleo_store paleo_qgis paleo_io
             paleo_services paleo_linkage paleo_workflow paleo_ui paleo_app)
    set_target_properties(${_t} PROPERTIES UNITY_BUILD TRUE UNITY_BUILD_BATCH_SIZE 8)
  endforeach()
endif()
