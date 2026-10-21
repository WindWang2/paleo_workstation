# cmake/paleo-modules-data.cmake — 模块库：依赖伞 + 数据层 + QGIS 封装层
# 由根 CMakeLists.txt 按顺序 include()（纯结构拆分，语义与拆分前逐行一致）。
# 注意：include() 不开新作用域，CMAKE_SOURCE_DIR 仍是仓库根。
# 文件名刻意不用 extra- 前缀——extra-*.cmake 受 _paleo_extra_manifest 防漏对账。

# ---- 模块化分层库（按 UI_LAYER_PLAN 层契约拆分）----
# DAG（按实测 include 边排序，单向无环）：
#   domain · algorithms · ai · store(catalog+metadata) ── qgis ── io ── services
#     ── linkage · workflow ── ui ── app
# catalog↔metadata 互有 include（datacatalog→atomicfile / mapversionstore→datacatalog），
# 同层环合并为 paleo_store。paleo_core 降为 INTERFACE 伞，旧链接方零改动。
add_library(paleo_deps INTERFACE)
target_link_libraries(paleo_deps INTERFACE paleo_qgis_core_deps ${GDAL_LIB} ${GEOS_C_LIB})
target_include_directories(paleo_deps INTERFACE ${CMAKE_SOURCE_DIR}/src ${GDAL_INCLUDE_DIR} ${GEOS_INCLUDE_DIR})

# ---- 数据层 ----
add_library(paleo_domain STATIC
  src/domain/singlefactorrequest.cpp
  src/domain/types.cpp
  src/domain/litholexicon.cpp
  src/domain/wellcompositemodel.cpp
  src/domain/wellrecords.cpp                        # 井文本记录类型 + XML 嗅探（自 io 下沉）
  src/domain/welltopsedit.cpp                       # 方向 32：分层校验/差异/合并/批量纯函数
  src/domain/projectclassifier.cpp                  # §3 path/extension classifier + xml sniff
  src/domain/arearules.cpp                          # 工程级参数 seam（docs/AREA_PARAMETERS.md）
  src/domain/wellsitingplan.cpp                     # 方向34：布井方案纯值类型（指标快照往返）
  src/domain/seismic/nicestep.cpp
  src/domain/seismic/timedepthmodel.cpp
  src/domain/sequenceframework.cpp                  # 方向28：层序格架模型（单元/标志层 + JSON）
  src/domain/frameworkdiagnostics.cpp               # 方向28：格架一致性诊断（悬空/空洞/重名/无归属）
  src/domain/seismic/sectionwellprojector.cpp
  # sgy*.h 为指向 paleo_sbm 的转发头；实现随 vendor 库编译。
)
target_link_libraries(paleo_domain PUBLIC paleo_deps paleo_sbm paleo_segyio paleo_glm)

add_library(paleo_algorithms STATIC
  src/algorithms/paleoalgorithms.cpp
  src/algorithms/faciespolygonize.cpp
  src/algorithms/faciessliver.cpp                   # A11：小图斑吸收（并查集 + 小顶堆，可取消）
  src/algorithms/seismicattr.cpp
  src/algorithms/singlefactor/curvekernel.cpp
  src/algorithms/singlefactor/support.cpp
  src/algorithms/singlefactor/localidw.cpp
  src/algorithms/singlefactor/samples.cpp
  src/algorithms/singlefactor/localdirectionalgorithm.cpp
  src/algorithms/singlefactor/corridor.cpp
  src/algorithms/singlefactor/partition.cpp
  src/algorithms/singlefactor/faultpath.cpp
  src/algorithms/singlefactor/buffertransition.cpp
  src/algorithms/singlefactor/regionalcontours.cpp
  src/algorithms/singlefactor/contouravoidance.cpp
  src/algorithms/singlefactor/wellacquisition.cpp    # 上游 _extract_current_wells 移植
  src/algorithms/singlefactor/structural.cpp         # 上游 structural_idw 面核（GEOS C API）
  src/algorithms/singlefactor/constraintparse.cpp    # 约束线 params_json 共享解析
  src/algorithms/singlefactor/structuralalgorithm.cpp
  src/algorithms/singlefactor/cartographicsmooth.cpp # 上游制图平滑折线辅助（纯数学）
  src/algorithms/singlefactor/fieldcontours.cpp      # 上游 field_contours/contour_work_field 移植
  src/algorithms/frameworksuggester.cpp              # 方向28：井间格架建议核（只出候选，不写库）
  src/algorithms/singlefactor/contourlevels.cpp      # 上游等值线级别助手（_nice_number 等）
  src/algorithms/wellsiting/wellsiting.cpp           # 方向34：井网覆盖诊断/候选/评估纯核
)
target_link_libraries(paleo_algorithms PUBLIC paleo_deps paleo_domain)

# catalog + metadata 同库（互依环）
add_library(paleo_store STATIC
  src/catalog/purgelease.cpp
  src/catalog/datacatalog.cpp                       # 生命周期/open/落盘面（方向 99 拆分主 TU）
  src/catalog/datacatalog_entities.cpp              # 实体族：add/查询/井名匹配/前缀 id
  src/catalog/datacatalog_assets.cpp                # 资产族：add/查询/物理删除/ast-N id
  src/catalog/datacatalog_links.cpp                 # 链接族：mutator×4/查询/role 诊断面
  src/catalog/datacatalog_versions.cpp              # 版本族：add(supersede)/extra 就地更新/sha/路径
  src/catalog/datacatalog_derivation.cpp            # 派生族：下游闭包/staleness-lite 标记
  src/catalog/datacatalog_export.cpp                # 导出族：catalog.json 快照/井位/测区 GeoJSON
  src/catalog/roleregistry.cpp                      # RoleDef/RoleRegistry 词表 + project_area.json 覆盖
  src/catalog/catalogroles.cpp                      # 方向92：树/AI 角色词表单源（分组/树序/显示名）
  src/catalog/entityview.cpp                        # RoleSlot/EntityView 纯查询门面
  src/metadata/paleoprojectstore.cpp
  src/metadata/paleoprojectfile.cpp
  src/metadata/layermanifest.cpp
  src/metadata/releasestore.cpp
  src/metadata/mapversionstore.cpp                  # 版本/发布状态机落表（meta sqlite）
  src/metadata/layouttemplatestore.cpp              # 版式模板库语义索引（方向 25；catalog 存内容）
  src/metadata/metastore.cpp                        # project.sqlite user_version 门
  src/catalog/frameworkstore.cpp                    # 方向28：格架落库（catalog 资产 + 受管版本）
  src/metadata/wellsitingstore.cpp                  # 方向34：布井方案集单行文档表
  src/metadata/projectlock.cpp                      # 两实例同开一工程写互斥（QLockFile）
  src/metadata/processprobe.cpp                     # 方向81：锁持有者 pid 存活探测 + 同机判据（Qt 陈旧锁回收兜底）
)
target_link_libraries(paleo_store PUBLIC paleo_deps paleo_domain)

# ---- QGIS 封装层 ----
add_library(paleo_qgis STATIC
  src/qgis/paleomapfacies.cpp
  src/qgis/paleomapsource.cpp
  src/qgis/wellpointnavigation.cpp
  src/qgis/mappingartifactwriter.cpp
  src/qgis/seismicwellcalibration.cpp
  src/qgis/qgisruntime.cpp
  src/qgis/qgisprojectservice.cpp
  src/qgis/qgisprojectservice_async.cpp
  src/qgis/qgisprojectservice_map.cpp
  src/qgis/projectmapreference.cpp
  src/qgis/onlinebasemap.cpp
  src/qgis/basemaplayer.cpp
  src/qgis/qgislayerservice.cpp
  src/qgis/qgiscanvascontroller.cpp
  src/qgis/qgiseditingservice.cpp
  src/qgis/qgisconstrainteditsession.cpp
  src/qgis/qgisprocessingservice.cpp
  src/qgis/qgisstyleservice.cpp
  src/qgis/geopatterns.cpp                           # 方向31：地质符号语义注册表（花纹/线型词表 + 符号构造）
  src/qgis/semanticlegend.cpp                        # 方向31：语义图例自动生成（渲染器类目→swatch+词面，增删/renderer 联动）
  src/qgis/qgislayoutservice.cpp
  src/qgis/geologicalmaplayout.cpp
  src/qgis/dataexportwriter.cpp
  src/qgis/layoutexport.cpp                         # 层位图/设计器导出核心（QgsLayoutExporter 封装）
  src/qgis/standardelements.cpp                     # 标准图件元素工厂 + 三类内置内容模板骨架（方向 25）
  src/qgis/mapbooklayout.cpp                        # 地图册：逐格/蒙太奇/索引页版面构建 + 落盘回读校验
  src/qgis/manifestprojection.cpp
  src/qgis/faciespartitioneditor.cpp
  src/qgis/faciespartitiontool.cpp
  src/qgis/factorstylewriter.cpp                    # m2(B)：单因素色带 .qml 落盘
  src/qgis/factorcontour.cpp                        # m2(B)：等值线 GDAL C API
  src/qgis/qgislayerprofile.cpp                     # 页面档案：QgsMapThemeCollection 封装 + 组档案表
  src/qgis/qgislabelzorder.cpp                      # 标注随图层 z 序：给每层钉 rendering/labelsWithLayer（vendored QGIS 补丁）
  src/qgis/qgislayerorganizer.cpp                   # 图层树布局器：00_Data 置顶共享 + 层位组 + 组秩归位
  src/qgis/previewmapcanvas.cpp                     # P2 D1.1 预览画布封装（私有层容器+CRS+历史栈+渐进渲染）
  src/qgis/mappingreferencecanvas.cpp
  src/qgis/previewmaptools.cpp                      # P2 D1.2 预览工具注册表（量测/identify/剖面线）
  src/qgis/previewrasteranalysis.cpp                # P2 D2.2/D5.x 预览栅格分析（统计/拉伸/直方图/剖面/极值）
  src/qgis/identifytool.cpp                         # 主画布识别浏览（经 IdentifyPresenter 驱动视图弹层）
  src/qgis/previewidentify.cpp                      # P2 D7 identify 查询核心（空间索引缓存+栅格插值）
  src/qgis/previewrendercache.cpp                   # P2 D6.2/D6.6 渲染结果缓存（内存 LRU + QTemporaryDir PNG）
  src/qgis/seismicsectiontool.cpp
  src/qgis/wellattributestore.cpp
  src/qgis/geostatrasterio.cpp                      # algo-atom P2：geostat 适配器 CRS 句柄 + Float32 写口
)
target_link_libraries(paleo_qgis PUBLIC paleo_deps paleo_ui_deps paleo_store paleo_algorithms paleo_io paleo_services) # #483 qgislayerservice→ErrorHub

# QGIS 4.2.2 original attribute table dialog, embedded with a small app host.
# App-only source is vendored because libqgis_app requires a QgisApp singleton.
# Core5Compat 已在 cmake/paleo-dependencies.cmake 顶部 find_package(Qt6 REQUIRED ...) 里一并 REQUIRED，此处不再重复探测。
add_library(paleo_qgis_attribute_table STATIC
  vendor/qgis_attribute_table/qgsattributetabledialog.cpp
  vendor/qgis_attribute_table/qgsattributetabledialog.h
  vendor/qgis_attribute_table/qgsclipboard.cpp
  vendor/qgis_attribute_table/qgsclipboard.h
  vendor/qgis_attribute_table/qgsdelattrdialog.cpp
  vendor/qgis_attribute_table/qgsdelattrdialog.h
  src/qgis/nativeattributetablehost.cpp
  src/qgis/nativeattributetablehost.h)
target_include_directories(paleo_qgis_attribute_table PUBLIC
  ${CMAKE_SOURCE_DIR}/vendor/qgis_attribute_table
  ${CMAKE_SOURCE_DIR}/src/qgis)
target_link_libraries(paleo_qgis_attribute_table PUBLIC paleo_qgis Qt6::Core5Compat)

add_library(paleo_io STATIC
  src/io/dataimportservice.cpp                      # 方向57：生命周期 + 入口 wrapper
  src/io/dataimport_session.cpp                     # 方向57：会话/登记基座（plan/RAW/commit 面）
  src/io/dataimport_file.cpp                        # 方向57：单文件导入编排（importOneFile 壳+分派）
  src/io/dataimport_wells.cpp                       # 方向57：井族分支（well_head/log/tops/deviation）
  src/io/dataimport_geodata.cpp                     # 方向57：地物族分支（horizon/seismic/辅助参考）
  src/io/dataimport_folder.cpp                      # 方向57：文件夹导入/预览/plan 执行面
  src/io/dataimport_pyramids.cpp                    # 方向57：栅格金字塔/概览族
  src/io/dataimport_document.cpp                    # 方向57：文档 PDF 转换 + 外链重定位
  src/io/lasparser.cpp
  src/io/dlisparser.cpp                            # 方向44：DLIS (RP66 v1) 读口
  src/io/lisparser.cpp                             # 方向44：LIS79 读口（含 TIF）
  src/io/welllogread.cpp                           # 方向44：井曲线格式分派门面
  src/io/segyreader.cpp                             # wave/segy
  src/io/constraintstore.cpp                        # wave/constraint-gpkg
  src/io/wellfileparsers.cpp                        # well head / tops / TD text parsers
  src/io/horizonbinner.cpp                          # horizon scatter -> binned time raster
  src/io/sattrio.cpp                                # goal/attr-volume：SATR/SATV 属性容器读写
  src/io/attrgridout.cpp                            # goal/attr-volume：时间切片属性网格 → 地理参考 GeoTIFF
  src/io/timedeptool.cpp                            # TVD/MD -> TIME(ms) interpolation
  src/io/geojsonaffine.cpp                          # D11 临时配准（手工仿射）
  src/io/wellcompositexml.cpp
  src/io/ingestplan.cpp                             # C 包：纯 plan + 幂等执行器
)
target_link_libraries(paleo_io PUBLIC paleo_deps paleo_domain paleo_store)

add_library(paleo_services STATIC
  src/services/paleomapfiles.cpp
  src/services/datagraph_layout.cpp
  src/services/dataexport.cpp
  src/services/datagraph.cpp
  src/services/toolavailability.cpp
  src/services/previewdoc.cpp                       # W1 数据页唯一数据门面
  src/services/previewdoc_xml.cpp
  src/services/previewdoc_survey.cpp                # 完整测区范围后台恢复
  src/services/singlefactordef.cpp                  # m2：单因素定义注册表
  src/services/faciesdisplaysettings.cpp
  src/services/algoparamschema.cpp                  # m2：算法参数 schema 注册表
  src/services/algorithmspec.cpp                    # algo-atom P0：AlgorithmSpec 静态契约表
  src/services/nativerunners.cpp                    # algo-atom：hztrack 等 services 侧 native 包装
  src/services/projectdata.cpp                      # 读侧门面（catalog.json + LayerManifest）
  src/services/paleotaskservice.cpp                 # D1/D2 异步任务框架
  src/services/jobrunner.cpp                        # 方向20：统一三段式 JobRunner（作业框架）
  src/services/seismicmapping.cpp                   # 测网几何↔inline/xline/CDP
  src/services/seismichorizoncluster.cpp             # 层位窗反射特征聚类（Mock 相解释）
  src/services/seismicclusterlabels.cpp              # 测井多数票标定聚类类别
  src/services/seismictaskservice.cpp               # sdk::Dataset 门面 + 任务编排 (Base Core)
  src/services/seismictaskservice_transcode.cpp     # 地震任务：索引与转码
  src/services/seismictaskservice_slice.cpp         # 地震任务：切片与剖面
  src/services/seismictaskservice_attr.cpp          # 地震任务：属性计算与体生成
  src/services/seismictaskservice_interp.cpp        # 地震任务：层位追踪与解释
  src/services/seismictaskservice_viz.cpp           # 地震任务：可视化与预览
  src/services/crashreport.cpp                      # 致命信号 fd 落盘 + .running 旗标
  src/services/frameworkservice.cpp                 # 方向28：格架读侧门面 + 建议/诊断编排
  src/services/startuptrace.cpp                     # goal/perf-systematize 簇1：启动分段仪表
  src/services/storagegovernance.cpp
  src/services/cataloghealth.cpp                    # 方向30：工区资产体检（快速面 + 外链 SHA 复验）
  src/services/errorhub.cpp                         # 方向64（方向76 合流后唯一实现）：统一错误通道（去重/聚合/环形历史）
  src/services/imagelod.cpp                         # 方向79：图片道装载 LOD（缩略两级 + EXIF + 原图 LRU）
  src/services/evidenceindex.cpp                    # G08/G12：证据反向索引（可重建检索结构，非第二事实源）
)
target_link_libraries(paleo_services PUBLIC paleo_deps paleo_io paleo_domain paleo_store paleo_algorithms)
