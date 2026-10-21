# cmake/paleo-test-labels.cmake — 测试标签：perf/golden/env 红名单/heavy（须在所有 extra-*.cmake 之后）
# 由根 CMakeLists.txt 按顺序 include()（纯结构拆分，语义与拆分前逐行一致）。
# 注意：include() 不开新作用域，CMAKE_SOURCE_DIR 仍是仓库根。
# 文件名刻意不用 extra- 前缀——extra-*.cmake 受 _paleo_extra_manifest 防漏对账。

# ---- 审计 01 M6：计时/黄金图类测试打标签（放在所有 extra-*.cmake 之后统一登记）----
# perf   = 墙钟预算/比率门/基准类：本机负载敏感。`ctest -LE perf` 可整体排除，
#          `ctest -L perf` 单独串行重跑。阈值一律不动；统一 RUN_SERIAL 防并行抖动。
# golden = 渲染像素比对类（字体/优化级别敏感）。
# add_paleo_test 已按首模块设了 LABELS——这里 APPEND，不覆盖（CI 的 -L 分步不受影响）。
# tst_panels / tst_correlation_full 只有个别计时断言、主体是功能覆盖：保持既有
# RUN_SERIAL，不打 perf（-LE perf 不应丢掉这些功能测试）。
set(_paleo_perf_tests
  tst_perf tst_previewmap_perf tst_aiassist_perf tst_startup_trace tst_perfbudget
  tst_mapsnapshot_scale
  tst_singlefactor_perf tst_singlefactor_contourbudget tst_singlefactor_guilatency
  tst_ui_blocking tst_mem_budget tst_perf_las tst_perf_segyindex tst_perf_catalog
  tst_perf_regress tst_petrophysperf tst_seismic_perf tst_seismic_baseline
  tst_seismic_budgets tst_seismicattrperf tst_horizontrackperf tst_timedepthperf
  tst_wellcomposite_perf tst_cache_las tst_welllogset_perf tst_geostat_perf
  tst_lasperf tst_dataperf_index tst_coldstart tst_inversion_perf
  # full-audit F10：吞吐比率断言（largeFieldThroughputRatio）无预热/无最小时
  # 长闸，CI linux 并行轮实证红、串行重跑洗白（run 38835970978）——挂 perf
  # 标签 + RUN_SERIAL 与同类比率门同一纪律。
  tst_wellsiting_kernel)
foreach(_t IN LISTS _paleo_perf_tests)
  if(TEST ${_t})
    set_property(TEST ${_t} APPEND PROPERTY LABELS perf)
    set_tests_properties(${_t} PROPERTIES RUN_SERIAL TRUE)
  endif()
endforeach()
if(TEST tst_singlefactor_perf)
  set_tests_properties(tst_singlefactor_perf PROPERTIES TIMEOUT 1200)
endif()
if(TEST tst_mapsnapshot_scale)
  set_tests_properties(tst_mapsnapshot_scale PROPERTIES TIMEOUT 1800)
endif()
if(TEST tst_singlefactor_kriging)
  set_tests_properties(tst_singlefactor_kriging PROPERTIES TIMEOUT 600)
endif()
if(TEST tst_seismic_3dui)
  set_tests_properties(tst_seismic_3dui PROPERTIES RUN_SERIAL TRUE)
endif()

# ---- 方向 81：环境债红名单（tools/env_redset.json，单一真源）→ ctest 标签 ----
# envdebt / envdebt_<category> / envdebt_<fixed|exempt>：`ctest -L envdebt` 一键
# 重跑方向 72 的 16 红清单；判定由 tools/check_env_redset.py 做（豁免只认
# 指定预算断言、只限指定路线）。APPEND，不覆盖既有标签。
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${CMAKE_SOURCE_DIR}/tools/env_redset.json")
file(READ "${CMAKE_SOURCE_DIR}/tools/env_redset.json" _paleo_redset)
string(JSON _paleo_redset_n LENGTH "${_paleo_redset}" entries)
if(_paleo_redset_n GREATER 0)
  math(EXPR _paleo_redset_last "${_paleo_redset_n} - 1")
  foreach(_i RANGE ${_paleo_redset_last})
    string(JSON _t GET "${_paleo_redset}" entries ${_i} test)
    string(JSON _c GET "${_paleo_redset}" entries ${_i} category)
    string(JSON _s GET "${_paleo_redset}" entries ${_i} status)
    if(TEST ${_t})
      set_property(TEST ${_t} APPEND PROPERTY LABELS envdebt envdebt_${_c} envdebt_${_s})
    endif()
  endforeach()
endif()
if(TEST tst_singlefactor_contourbudget)
  set_tests_properties(tst_singlefactor_contourbudget PROPERTIES TIMEOUT 600)
endif()
if(TEST tst_singlefactor_guilatency)
  set_tests_properties(tst_singlefactor_guilatency PROPERTIES TIMEOUT 180)
endif()
foreach(_t IN ITEMS tst_wellcomposite_visual)
  if(TEST ${_t})
    set_property(TEST ${_t} APPEND PROPERTY LABELS golden)
  endif()
endforeach()

# ---- #503：重测试分组标签 heavy（APPEND，不覆盖模块标签）----
# 反馈周期：Windows 全量 ctest ~44min、巨型全栈测试单跑分钟级——本地迭代
# 可用 `ctest -LE 'heavy|perf'` 走快速道（BUILDING.md「测试车道与快速道」）。
# 入选口径 = 全栈/独占类功能巨测（#503 实测体量：万行级测试码或 RUN_SERIAL
# 全栈伞式；perf 预算类已有 perf 标签，不重复挂）。CI 不用该标签（-LE 语义
# 不变），仅本地快速道与 `-L heavy` 定位用；随测试拆分/瘦身只缩不涨。
set(_paleo_heavy_tests
  tst_panels tst_panels_dataops tst_edittools tst_import tst_mappingworkbench
  tst_correlation_full)
foreach(_t IN LISTS _paleo_heavy_tests)
  if(TEST ${_t})
    set_property(TEST ${_t} APPEND PROPERTY LABELS heavy)
  endif()
endforeach()
