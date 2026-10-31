# cmake/paleo-test-infra.cmake — 测试基础设施：沙箱、add_paleo_test、层边界护栏/自检测试
# 由根 CMakeLists.txt 按顺序 include()（纯结构拆分，语义与拆分前逐行一致）。
# 注意：include() 不开新作用域，CMAKE_SOURCE_DIR 仍是仓库根。
# 文件名刻意不用 extra- 前缀——extra-*.cmake 受 _paleo_extra_manifest 防漏对账。


# ---- 测试环境沙箱（devex T2 竞态根治，单点定义）----
# 每测试独立 XDG/HOME 目录：QSettings NativeFormat 在 Linux 走 XDG_CONFIG_HOME，
# tst_panels/tst_webviewpanel 等不再互踩真实 ~/.config/paleo（tst_ui/tst_procdialog
# 曾靠 .cpp 里 setPath 自救，现收归测试布线，零 .cpp 改动）。
# Windows（方向 81）：NativeFormat 走注册表不受 env 控制——每个测试可执行文件
# 链入 tests/support/paleo_test_registry_sandbox.cpp（OBJECT 库，静态初始化期
# 把本进程 HKCU 重映射到 ctest-home/<test>/registry.hiv 私有 hive），由
# PALEO_TEST_REGISTRY_HIVE 指路；tst_test_sandbox 钉契约。决策记录见
# docs/ledger-archive/goal-loop-env-debt2.md「QSettings 三选一」。CI windows leg 的
# 串行 ctest 本方向不改。
if(WIN32)
  add_library(paleo_test_registry_sandbox OBJECT tests/support/paleo_test_registry_sandbox.cpp)
endif()
# 测试可执行文件链入 Windows 注册表沙箱 hook（非 Windows 为空操作）。OBJECT
# 直链进 exe（不是静态库）——静态初始化对象必定被链接、先于 main 运行。
function(paleo_test_link_sandbox target)
  if(WIN32)
    target_sources(${target} PRIVATE $<TARGET_OBJECTS:paleo_test_registry_sandbox>)
  endif()
endfunction()
# 注意 set_tests_properties 的 ENVIRONMENT 是整体覆盖非追加：外部对本函数
# 注入环境做扩展，非路径变量用 set_property(TEST ... APPEND PROPERTY ...)；
# LD_LIBRARY_PATH 等路径变量必须走 ENVIRONMENT_MODIFICATION 的
# path_list_prepend（APPEND 一条整体赋值仍会替换外部注入值，见下方
# tst_onnx 的修复注记）。
# 方向81：PALEO_PYTHON 在 ctest 下可达。configure 时 env PALEO_PYTHON（paleo-dev.ps1
# localdeps 路线指向 deps 根 python.exe）作缺省进 cache；非空即注入每个测试的
# ENVIRONMENT，并把期望路径编进 tst_pythonrepl/tst_scriptrunner 断言——
# 裸 ctest / IDE 跑测试不再依赖调用者 shell 的 PATH（Windows 上 PATH 首个
# python 常是 WindowsApps 商店桩）。留空 = 保持 PATH 发现（Linux/CI 缺省）。
# #398：env→cache 播种要 FORCE——裸 CACHE set 在条目已存在（含 -D 建的
# UNINITIALIZED 档）后静默失效，PALEO_PYTHON 再也进不了测试环境。与
# QGIS_PREFIX/QSCINTILLA_PREFIX 的播种模式同构。
if(NOT PALEO_TEST_PYTHON AND DEFINED ENV{PALEO_PYTHON})
  set(PALEO_TEST_PYTHON "$ENV{PALEO_PYTHON}" CACHE FILEPATH
    "Python interpreter injected as PALEO_PYTHON into every ctest (empty = PATH discovery)" FORCE)
else()
  set(PALEO_TEST_PYTHON "${PALEO_TEST_PYTHON}" CACHE FILEPATH
    "Python interpreter injected as PALEO_PYTHON into every ctest (empty = PATH discovery)")
endif()
set(PALEO_TEST_PYTHON_NORM "")
if(PALEO_TEST_PYTHON)
  file(TO_CMAKE_PATH "${PALEO_TEST_PYTHON}" PALEO_TEST_PYTHON_NORM)
endif()

function(paleo_test_sandbox name)
  set(_home ${CMAKE_BINARY_DIR}/ctest-home/${name})
  file(MAKE_DIRECTORY "${_home}/config" "${_home}/data" "${_home}/cache" "${_home}/tmp")
  set(_env "QT_QPA_PLATFORM=offscreen;PALEO_WELL_FACIES_NO_KEYCHAIN=1;XDG_CONFIG_HOME=${_home}/config;XDG_DATA_HOME=${_home}/data;XDG_CACHE_HOME=${_home}/cache;HOME=${_home};TMPDIR=${_home}/tmp")
  if(NOT WIN32)
    # Windows 不把 TEMP/TMP 改到构建目录：<build>/ctest-home/<test>/tmp 比系统
    # Temp 深 ~40 字符，导入类测试的托管路径（raw/<uuid>/<uuid>/文件）逼近
    # MAX_PATH=260（tst_import 只在 ctest 下红、直跑绿）；QTemporaryDir 本身
    # 已按用例隔离，Windows 上 XDG 沙箱也不生效（QSettings 走注册表）。
    string(APPEND _env ";TEMP=${_home}/tmp;TMP=${_home}/tmp")
  else()
    # 私有注册表 hive（见上方 paleo_test_link_sandbox）：每次运行先删再挂载。
    string(APPEND _env ";PALEO_TEST_REGISTRY_HIVE=${_home}/registry.hiv")
  endif()
  set_tests_properties(${name} PROPERTIES ENVIRONMENT "${_env}")
  if(QGIS_PREFIX)
    # vendor 构建下测试资源/provider 也走构建所链的 prefix（与二进制
    # RUNPATH 一致），不借系统 /usr 栈——裸 ctest 即全 vendored。
    set_property(TEST ${name} APPEND PROPERTY ENVIRONMENT
      "QGIS_PREFIX_PATH=${PALEO_QGIS_RUNTIME_PREFIX}")
  endif()
  if(PALEO_TEST_PYTHON_NORM)
    set_property(TEST ${name} APPEND PROPERTY ENVIRONMENT
      "PALEO_PYTHON=${PALEO_TEST_PYTHON_NORM}")
  endif()
  # review-fix-ci：sanitizer 选项只给「缺省值」，用 ENVIRONMENT_MODIFICATION
  # string_prepend 拼在继承值之前——sanitizer 选项串后出现者胜，CI 工作流级
  # ASAN_OPTIONS=detect_leaks=1 / LSAN_OPTIONS=suppressions=… 不再被逐测试
  # ENVIRONMENT 整串覆盖（旧写法让 PW-030 的 LSAN 实际全程关闭）。本机未导出
  # 时缺省仍 detect_leaks=0（与旧行为一致）。不再写 LSAN_OPTIONS。
  if(PALEO_ENABLE_ASAN)
    set_property(TEST ${name} APPEND PROPERTY ENVIRONMENT_MODIFICATION
      "ASAN_OPTIONS=string_prepend:detect_leaks=0:abort_on_error=1:print_summary=1:")
  endif()
  if(PALEO_ENABLE_UBSAN)
    set_property(TEST ${name} APPEND PROPERTY ENVIRONMENT_MODIFICATION
      "UBSAN_OPTIONS=string_prepend:print_stacktrace=1:halt_on_error=1:")
  endif()
  set_property(TEST ${name} APPEND PROPERTY ENVIRONMENT
    "QTEST_FUNCTION_TIMEOUT=900000")
  # #840：QTEST_FUNCTION_TIMEOUT 只在进程已进入 QtTest 后生效；进程卡在启动前
  # （如 Windows 加载器弹框）时整片吊满 6h job 超时且不点名。给每个测试兜底
  # 进程级 ctest TIMEOUT（30 min），超时即 ***Timeout 判红并点名。个别测试的
  # 显式 TIMEOUT（paleo-test-labels.cmake 等，后设）照常覆盖此默认值。
  get_test_property(${name} TIMEOUT _paleo_t)
  if(NOT _paleo_t)
    set_tests_properties(${name} PROPERTIES TIMEOUT ${PALEO_TEST_TIMEOUT})
  endif()
endfunction()

# #840：每测试默认超时（秒），可 -DPALEO_TEST_TIMEOUT=… 覆盖。
set(PALEO_TEST_TIMEOUT 1800 CACHE STRING "Default per-test ctest TIMEOUT in seconds (#840)")

set(PALEO_QTEST_FIRST_RUN_DIR "${CMAKE_BINARY_DIR}/Testing/qtest-first-run")
file(MAKE_DIRECTORY "${PALEO_QTEST_FIRST_RUN_DIR}")

# ---- 跨方向契约：add_paleo_test(name [LIBS ...]) ----
# 单参旧式调用零改动可用（回退 paleo_core 伞）——cmake/extra-*.cmake 里
# 四个并行方向都是旧式调用，勿破坏。新式 LIBS 给最小链接集（链接瘦身 T1：
# touch 单模块不再牵连全部测试 relink）；LIBS 按依赖序书写，首模块决定
# ctest LABEL（CI -L 分步用，不拆 job）。
# 注册行保持单行；并行合并冲突时保留双方行。
function(add_paleo_test name)
  cmake_parse_arguments(PARSE_ARGV 1 apt "" "" "LIBS")
  set(_libs ${apt_LIBS})
  if(NOT _libs)
    set(_libs paleo_core)
  endif()
  add_executable(${name} tests/${name}.cpp)
  paleo_test_link_sandbox(${name})
  target_link_libraries(${name} PRIVATE ${_libs} Qt6::Test)
  target_compile_definitions(${name} PRIVATE FIXTURE_GPKG="${FIXTURE_GPKG}")
  add_test(NAME ${name} COMMAND ${name}
           -o "${PALEO_QTEST_FIRST_RUN_DIR}/${name}.txt,txt")
  paleo_test_sandbox(${name})
  set(_label core)
  foreach(_l IN LISTS _libs)
    # #484 P3：带下划线的模块子库（paleo_qgis_attribute_table/paleo_ui_deps 等）
    # 原正则 ^paleo_[a-z]+$ 匹配不到 → 静默回落 core（tst_mappingworkbench 实例）。
    # 取首段（qgis/ui/…）与「首模块决定 LABEL」契约一致；paleo_core 伞仍排除。
    if(_l MATCHES "^paleo_([a-z][a-z0-9_]*)$" AND NOT _l STREQUAL "paleo_core")
      string(REGEX REPLACE "^paleo_([a-z]+).*$" "\\1" _label "${_l}")
      break()
    endif()
  endforeach()
  set_tests_properties(${name} PROPERTIES LABELS "${_label}")
endfunction()

# ---- W6 层边界护栏（docs/UI_LAYER_PLAN.md）：include 归一化正向/反向扫描 +
# QtWidgets 词表禁令 + 层标记硬检查；词表外置 tools/layering_vocab.json
# （scripts/new_module.sh 同步维护）。baseline 已归零，--strict 是防回升闸门
# （非空即红）；selftest 单独成项——扫描器自身坏掉不能静默变绿。 ----
# E-13（full-audit-20261020）：python3 缺失时 ~36 个源码闸门 ctest 项会静默
# 不注册（ctest 全绿假象）——配置错不能静默变绿，显式 FATAL_ERROR 早失败。
find_program(PALEO_PYTHON3 NAMES python3 python)
if(NOT PALEO_PYTHON3)
  message(FATAL_ERROR "未找到 python3——源码闸门（layering/ui/i18n/file_budget 等）"
          "无法注册；安装 python3 后重新 configure（政策见 BUILDING.md）")
endif()
if(PALEO_PYTHON3)
  add_test(NAME well_facies_input_audit COMMAND ${PALEO_PYTHON3}
           -m unittest discover -s ${CMAKE_SOURCE_DIR}/tests -p test_audit_well_facies_inputs.py)
  add_test(NAME layering COMMAND ${PALEO_PYTHON3}
           ${CMAKE_SOURCE_DIR}/tools/check_layering.py)
  add_test(NAME layering_strict COMMAND ${PALEO_PYTHON3}
           ${CMAKE_SOURCE_DIR}/tools/check_layering.py --strict)
  add_test(NAME layering_selftest COMMAND ${PALEO_PYTHON3}
           ${CMAKE_SOURCE_DIR}/tools/check_layering.py --selftest)
  # #474：两个 opt-in 闸门档接入 ctest（此前 --symbol-audit 只有 CI lint 跑、
  # --transitive 完全无闸——「有工具未接线」）：
  #   layering_symbol_audit：零基线（任何非视图层 ui 类构造即红）；
  #   layering_transitive：四指标对 tools/transitive-baseline.txt 只缩不涨。
  # heavy_* 两指标仅依赖 repo include 边恒可闸；func/ui 两指标需 QGIS 头目录
  #（探测序 --qgis-include > $QGIS_PREFIX/include/qgis > vendor/superbuild 默认）
  # ——本测试不显式传参：superbuild 本地全闸，CI deb 闭包构建自动降为只闸
  # heavy_*（CI lint 车道显式给 QGIS_PREFIX 全闸，见 .github/workflows/ci.yml）。
  # #533 口径裁决（2026-10-18）：func/ui 两指标的权威闸口在 CI lint 车道
  #（Source gates 步显式给 QGIS_PREFIX）；本 ctest 项**不注入** QGIS_PREFIX，
  # 否则 deb 闭包/OSGeo4W 构建下该指标的红会复制进 linux/windows 两个必过
  # 车道（同一信号 lint 已有，重复红只稀释门禁语义——#497 的教训）。本地
  # 要与 CI 同口径全闸，显式跑：
  #   QGIS_PREFIX=<vendor/superbuild/prefix 或 deb 闭包前缀> \
  #     python tools/check_layering.py --transitive
  # 当前树该口径实测 ui_qtwidgets_notes 7 > baseline 6（2026-10-20 复测：
  # func 0 / ui 7 / heavy_edges 52 / heavy_tus 50，仅 ui 差 1）。差额边的
  # 主体已两度换人未归零——原 #531 的 geologicalexportdialog.cpp →
  # src/qgis/layoutpreview.h 边已被删除，现差额为
  # src/ui/maptools/identifytool.h:6 → qgis/maptoolviewapi.h ->
  # [qgs] qgsmaptool.h -> <QGestureEvent>@26（#552 引入、晚于 #518 基线
  # 更新）。差额已在 docs/ci/known-reds.json 登记（issue #533，test-scope
  # 条目 layering-transitive-ui-notes-20261020 + lint job-scope 条目）。
  # **后续动作**：该差额经 src/qgis 薄壳与视图层边界的计数口径裁决收敛后，
  # 把本测试改为注入 QGIS_PREFIX 的真同口径（本地/CI 全闸同一指标）。
  add_test(NAME layering_symbol_audit COMMAND ${PALEO_PYTHON3}
           ${CMAKE_SOURCE_DIR}/tools/check_layering.py --symbol-audit)
  add_test(NAME layering_transitive COMMAND ${PALEO_PYTHON3}
           ${CMAKE_SOURCE_DIR}/tools/check_layering.py --transitive)
  # #532：--qgs-audit（PALEO_QGIS_PLAN §2「QGIS 迁移成本只许落在 src/qgis」
  # 在 include 级的唯一机械执行者）接入 ctest——此前全仓零接线，include 级
  # 直连可以无声增长。接线时 116 个直连文件全部在册（#523 导出链封装
  # 收敛后新增 0、可收缩 0）；full-audit-20261021 E-2 扫描面扩到全部
  # 非视图模块后冻结为 137（见基线头注），--strict 口径与
  # qgis_spread_strict 一致（可收缩即红，收敛一个删一行）。
  add_test(NAME layering_qgs_audit COMMAND ${PALEO_PYTHON3}
           ${CMAKE_SOURCE_DIR}/tools/check_layering.py --qgs-audit --strict)
  # #497：CI 红项分诊名单（docs/ci/known-reds.json）结构校验 + 分诊器夹具
  add_test(NAME qgis_spread COMMAND ${PALEO_PYTHON3}
         ${CMAKE_SOURCE_DIR}/tools/check_qgis_spread.py)
  add_test(NAME qgis_spread_strict COMMAND ${PALEO_PYTHON3}
         ${CMAKE_SOURCE_DIR}/tools/check_qgis_spread.py --strict)
  add_test(NAME qgis_spread_selftest COMMAND ${PALEO_PYTHON3}
         ${CMAKE_SOURCE_DIR}/tools/check_qgis_spread.py --selftest)
  # E-12（full-audit-20261020）：与其余源码闸门同口径——无标签 ctest 项 +
  # WORKING_DIRECTORY 双保险（AGENTS.md「构建与测试布线」节声明）。
  set_tests_properties(qgis_spread qgis_spread_strict qgis_spread_selftest
                     PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  #（triage job 消费前，本地/ctest 先过一遍名单合法性与解析器回归）。
  add_test(NAME ci_known_reds COMMAND ${PALEO_PYTHON3}
           ${CMAKE_SOURCE_DIR}/tools/ci_triage_reds.py --validate)
  add_test(NAME ci_triage_reds_selftest COMMAND ${PALEO_PYTHON3}
           ${CMAKE_SOURCE_DIR}/tools/ci_triage_reds.py --selftest)
  # E-11（full-audit-20261020）部分收口：flaky 台账检查器 selftest 接闸——
  # 此前零注册（检查器自身坏掉不能静默变绿；跨 run 聚合接线见 #501 后续）。
  add_test(NAME flaky_ledger_selftest COMMAND ${PALEO_PYTHON3}
           ${CMAKE_SOURCE_DIR}/tools/check_flaky_ledger.py --selftest)
  # #760：Windows/Linux 测试门禁共享规则（tools/ci-gate-rules.json 单一真源）——
  # consistency 逐轮核对两端脚本的内联回落副本与 JSON 一致、且都从共享规则取
  # 结果行口径，防两端门禁语义悄悄漂移；selftest 验证渲染/转义/Timeout 入库。
  add_test(NAME ci_gate_rules COMMAND ${PALEO_PYTHON3}
           ${CMAKE_SOURCE_DIR}/tools/ci_gate_rules.py --check-consistency)
  add_test(NAME ci_gate_rules_selftest COMMAND ${PALEO_PYTHON3}
           ${CMAKE_SOURCE_DIR}/tools/ci_gate_rules.py --selftest)
  # goal/ui-experience-polish：UI 不变量闸门——DESIGN.md Motion 禁编排动画
  #（src/ui 零 QPropertyAnimation/QEasingCurve/setAnimated(true)）+ QSS 裸
  # hex 零容忍（token 单一真源 = paleotheme；baseline 只缩不涨，strict 防回升）。
  add_test(NAME ui_invariants COMMAND ${PALEO_PYTHON3}
           ${CMAKE_SOURCE_DIR}/tools/check_ui_invariants.py)
  add_test(NAME ui_invariants_strict COMMAND ${PALEO_PYTHON3}
           ${CMAKE_SOURCE_DIR}/tools/check_ui_invariants.py --strict)
  add_test(NAME ui_invariants_selftest COMMAND ${PALEO_PYTHON3}
           ${CMAKE_SOURCE_DIR}/tools/check_ui_invariants.py --selftest)
  # #502：视图层文件规模闸（src/ui 单文件 ≤1000 行，baseline 只缩不涨）。
  # 已闸 include 层（layering）与动画/色值/DPR（ui_invariants），体量侧此前无闸。
  add_test(NAME ui_size COMMAND ${PALEO_PYTHON3}
           ${CMAKE_SOURCE_DIR}/tools/check_ui_size.py)
  add_test(NAME ui_size_strict COMMAND ${PALEO_PYTHON3}
           ${CMAKE_SOURCE_DIR}/tools/check_ui_size.py --strict)
  add_test(NAME ui_size_selftest COMMAND ${PALEO_PYTHON3}
           ${CMAKE_SOURCE_DIR}/tools/check_ui_size.py --selftest)
  # #538：函数体规模闸（src/ui 单函数 ≤200 行 + 单文件 ≤1000 行，baseline
  # 只缩不涨）接入 ctest——与 ui_size（只闸 >1000 行文件）互补：本闸管函数体
  # 与冻结值回升/可收紧/可收缩四态。接线时已做只缩不涨对账（删 3 行回到阈值
  # 内 + 下调 5 个冻结值，见 file-budget-baseline.txt 头注）；残余 5 个回升
  # 条目在 4 个不属本岗可改范围的文件（editingtoolbar / mappingworkbenchpage
  # ×2 / attach_compose / compositebody），已在 docs/ci/known-reds.json 登记
  # （issue #538）——闸门红着接线，不靠上调基线换绿。
  add_test(NAME file_budget COMMAND ${PALEO_PYTHON3}
           ${CMAKE_SOURCE_DIR}/tools/check_file_budget.py)
  add_test(NAME file_budget_strict COMMAND ${PALEO_PYTHON3}
           ${CMAKE_SOURCE_DIR}/tools/check_file_budget.py --strict)
  add_test(NAME file_budget_selftest COMMAND ${PALEO_PYTHON3}
           ${CMAKE_SOURCE_DIR}/tools/check_file_budget.py --selftest)
  # #77：i18n 裸字面量门禁（#44「无 CI 门禁」收口）。脚本按自身位置定位
  # src/，WORKING_DIRECTORY 显式钉源码根作双保险。
  add_test(NAME i18n COMMAND ${PALEO_PYTHON3}
           ${CMAKE_SOURCE_DIR}/tools/check_i18n.py)
  add_test(NAME i18n_selftest COMMAND ${PALEO_PYTHON3}
           ${CMAKE_SOURCE_DIR}/tools/check_i18n.py --selftest)
  add_test(NAME ui_tokens COMMAND ${PALEO_PYTHON3}
           ${CMAKE_SOURCE_DIR}/tools/check_ui_tokens.py)
  add_test(NAME ui_tokens_strict COMMAND ${PALEO_PYTHON3}
           ${CMAKE_SOURCE_DIR}/tools/check_ui_tokens.py --strict)
  add_test(NAME ui_tokens_selftest COMMAND ${PALEO_PYTHON3}
           ${CMAKE_SOURCE_DIR}/tools/check_ui_tokens.py --selftest)
  # 方向 71（#76 依赖口径统一）：三路 QGIS 版本口径一致性——superbuild 精确
  # == deb 闭包上游 == manifest qgis_family 家族，补丁名随 superbuild 版本。
  # 防 4.2.2/4.2.3/4.2.x 三口径再分裂；selftest 合成树验证各分裂形态能打红。
  add_test(NAME qgis_versions COMMAND ${PALEO_PYTHON3}
           ${CMAKE_SOURCE_DIR}/tools/check_qgis_versions.py)
  add_test(NAME qgis_versions_selftest COMMAND ${PALEO_PYTHON3}
           ${CMAKE_SOURCE_DIR}/tools/check_qgis_versions.py --selftest)
  # 方向 81 环境债二期：QGIS 资源补齐工具（deb-closure.lock 同源取 srs.db）、
  # 环境债红名单检查器、OSGeo4W pin 生成器（#297 回归：[prev] 段覆盖当前版本）
  # 的合成夹具自检；env_redset_validate 钉名单结构 + 测试名已注册。
  add_test(NAME qgis_resources_selftest COMMAND ${PALEO_PYTHON3}
           ${CMAKE_SOURCE_DIR}/tools/ensure_qgis_resources.py --selftest)
  add_test(NAME env_redset_validate COMMAND ${PALEO_PYTHON3}
           ${CMAKE_SOURCE_DIR}/tools/check_env_redset.py --validate)
  add_test(NAME env_redset_selftest COMMAND ${PALEO_PYTHON3}
           ${CMAKE_SOURCE_DIR}/tools/check_env_redset.py --selftest)
  add_test(NAME pin_osgeo4w_selftest COMMAND ${PALEO_PYTHON3}
           ${CMAKE_SOURCE_DIR}/tools/pin_osgeo4w.py --selftest)
  # 方向 100：文档漂移护栏——任务书份数（期望值只读 goal-loop-prompts/
  # README 的「编号任务书份数：」行，不写死）/ UI_LAYER_PLAN 主窗行数落点
  # （日期戳现状行 ±20% 容差 + 家族 TU 精确数）/ AUDIT_ISSUES 头部计数句
  # 必带日期戳。selftest 合成夹具含「README=100 且实数=100 应绿」反写死项。
  add_test(NAME docs_drift COMMAND ${PALEO_PYTHON3}
           ${CMAKE_SOURCE_DIR}/tools/check_docs_drift.py)
  add_test(NAME docs_drift_selftest COMMAND ${PALEO_PYTHON3}
           ${CMAKE_SOURCE_DIR}/tools/check_docs_drift.py --selftest)
  # 方向 94：LibreOffice vendored pin 双平台（libreoffice/libreoffice_win）
  # 一致性——版本对齐 + url/sha/layout 口径 + fetch 脚本引用面；selftest
  # 合成 manifest 验证各漂移形态能打红。
  add_test(NAME lo_pin COMMAND ${PALEO_PYTHON3}
           ${CMAKE_SOURCE_DIR}/tools/check_lo_pin.py)
  add_test(NAME lo_pin_selftest COMMAND ${PALEO_PYTHON3}
           ${CMAKE_SOURCE_DIR}/tools/check_lo_pin.py --selftest)
  # #642：角色双表一致性——catalogroles（UI 树面）与 roleregistry（链接
  # 词表）的 key 集合必须相等；selftest 合成夹具验证漏登形态能打红。
  add_test(NAME role_tables COMMAND ${PALEO_PYTHON3}
           ${CMAKE_SOURCE_DIR}/tools/check_role_tables.py)
  add_test(NAME role_tables_selftest COMMAND ${PALEO_PYTHON3}
           ${CMAKE_SOURCE_DIR}/tools/check_role_tables.py --selftest)
  set_tests_properties(layering layering_strict layering_selftest
                       layering_symbol_audit layering_transitive
                       layering_qgs_audit
                       ci_known_reds ci_triage_reds_selftest flaky_ledger_selftest
                       ci_gate_rules ci_gate_rules_selftest
                       ui_invariants ui_invariants_strict ui_invariants_selftest
                       i18n i18n_selftest ui_tokens ui_tokens_strict ui_tokens_selftest
                       ui_size ui_size_strict ui_size_selftest
                       file_budget file_budget_strict file_budget_selftest
                       qgis_versions qgis_versions_selftest
                       qgis_resources_selftest env_redset_validate env_redset_selftest
                       pin_osgeo4w_selftest docs_drift docs_drift_selftest
                       lo_pin lo_pin_selftest
                       role_tables role_tables_selftest
                       PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  set_tests_properties(qgis_resources_selftest env_redset_validate env_redset_selftest
                       pin_osgeo4w_selftest docs_drift docs_drift_selftest
                       lo_pin lo_pin_selftest
                       role_tables role_tables_selftest
                       PROPERTIES ENVIRONMENT "PYTHONUTF8=1")
  # #767：Python 闸门项此前会在源码树留下字节码（tools/__pycache__/…、
  # tests/__pycache__/…）——ctest 跑完工作树即脏（git status 噪音），且跨平台
  # 大小写敏感文件系统上陈旧 .pyc 有隐性风险。全部 Python 闸门项统一注入
  # PYTHONDONTWRITEBYTECODE=1；与 PYTHONUTF8 合并设置——ENVIRONMENT 是整体
  # 覆盖非追加，两个值必须在同一串里，否则后一次 set_tests_properties 会把
  # 前一次注入的 PYTHONUTF8 顶掉（Windows cp1252 控制台下中文报告行即假红）。
  set(_paleo_py_gates
      well_facies_input_audit
      layering layering_strict layering_selftest
      layering_symbol_audit layering_transitive layering_qgs_audit
      qgis_spread qgis_spread_strict qgis_spread_selftest
      ci_known_reds ci_triage_reds_selftest flaky_ledger_selftest
      ci_gate_rules ci_gate_rules_selftest
      ui_invariants ui_invariants_strict ui_invariants_selftest
      i18n i18n_selftest
      ui_tokens ui_tokens_strict ui_tokens_selftest
      ui_size ui_size_strict ui_size_selftest
      file_budget file_budget_strict file_budget_selftest
      qgis_versions qgis_versions_selftest
      qgis_resources_selftest env_redset_validate env_redset_selftest
      pin_osgeo4w_selftest docs_drift docs_drift_selftest
      lo_pin lo_pin_selftest
      role_tables role_tables_selftest)
  set_tests_properties(${_paleo_py_gates}
                       PROPERTIES ENVIRONMENT "PYTHONUTF8=1;PYTHONDONTWRITEBYTECODE=1")
endif()
# ---- C++ 主线边界护栏（goal/cpp-primary-boundary-20261025）----
# cmake/verify-cpp-mainline.cmake（cmake -P，零 Python 依赖）：src/ 零 Python
# 应用源码、构建/安装面不引用历史 Python 归档目录、必备 C++ 模块与合法
# Python 扩展（pythonenv/scriptrunner/pythonrepl/控制台）注册不失踪、
# superbuild 保持 WITH_BINDINGS=OFF。selftest 在合成夹具上正反向验证——
# 检查器自身坏掉不能静默变绿。规则全文与动机见
# docs/CPP_PRIMARY_BOUNDARY.md；与上方 Python 闸门同口径：无标签 ctest 项
# + WORKING_DIRECTORY 双保险（AGENTS.md「构建与测试布线」）。
add_test(NAME cpp_mainline COMMAND ${CMAKE_COMMAND}
         -DCPP_MAINLINE_SOURCE_DIR=${CMAKE_SOURCE_DIR}
         -P ${CMAKE_SOURCE_DIR}/cmake/verify-cpp-mainline.cmake)
add_test(NAME cpp_mainline_selftest COMMAND ${CMAKE_COMMAND}
         -DCPP_MAINLINE_SOURCE_DIR=${CMAKE_SOURCE_DIR}
         -DCPP_MAINLINE_SELFTEST=ON
         -DCPP_MAINLINE_SELFTEST_DIR=${CMAKE_BINARY_DIR}/cpp-mainline-selftest
         -P ${CMAKE_SOURCE_DIR}/cmake/verify-cpp-mainline.cmake)
set_tests_properties(cpp_mainline cpp_mainline_selftest
                     PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
# ---- 新 worktree 首次 configure 早失败护栏（goal/perf-maintenance-20261027）----
# cmake/verify-first-configure.cmake（cmake -P）：依赖存在（stub prefix 全链
# 首配 + 四条 vendor include 落进 compile_commands——TODOS P2 原症状断言）/
# 缺失（显式与 env 两路悬空 prefix 必须 configure 期 FATAL 且报文含修复
# 入口）/ 幂等（同一命令重跑行为一致，不生成错误栈构建系统）。仅 UNIX
# 注册：stub 库文件名/分隔符按 POSIX 写死，Windows 首配行为不在本护栏
# 验收面（PR 如实标注，待 Windows 车道实跑后再放开）。
if(UNIX)
  add_test(NAME first_configure COMMAND ${CMAKE_COMMAND}
           -DFIRSTCFG_SOURCE_DIR=${CMAKE_SOURCE_DIR}
           -DFIRSTCFG_BINARY_DIR=${CMAKE_BINARY_DIR}/first-configure-check
           -P ${CMAKE_SOURCE_DIR}/cmake/verify-first-configure.cmake)
  set_tests_properties(first_configure PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()
# 方向 81：paleo-dev.ps1 单元自检（AST 抽函数 + 合成夹具；-Mutate 换回 #297
# 旧 Read-InstalledDbMap 必须被抓到）。找到 pwsh/powershell 才注册——Windows
# CI 必跑，Linux 装了 pwsh 也跑。
find_program(PALEO_PWSH NAMES pwsh powershell)
if(PALEO_PWSH)
  add_test(NAME paleo_dev_ps1_selftest COMMAND ${PALEO_PWSH} -NoProfile -ExecutionPolicy Bypass
           -File ${CMAKE_SOURCE_DIR}/tools/paleo_dev_ps1_selftest.ps1)
  add_test(NAME paleo_dev_ps1_selftest_mutation COMMAND ${PALEO_PWSH} -NoProfile -ExecutionPolicy Bypass
           -File ${CMAKE_SOURCE_DIR}/tools/paleo_dev_ps1_selftest.ps1 -Mutate)
endif()
