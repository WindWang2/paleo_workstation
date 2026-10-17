# Goal-Loop 账本 — 方向 96：constraintpage 拆分

- 工作树：`.worktrees/constraintpage-split`（分支 `goal/constraintpage-split-20261010`）
- 基线：origin/master `f474069b`（比任务口径 `32851a1e` 新；其间 #316 方向 91 已合，
  constraintpage.cpp 1,830 → **1,893 行**——协变量面（factorCovariateCombo/
  factorCrossCorrelationSpin + updateEngineRows 协变量可见性/清单段）按
  「谁先合谁为准」完整保留）。
- 模式：#312 主窗七域 TU 先例——ctor 按原文行序分解为 **private 构建段成员函数**
  （头文件 private 区增长，公共区 diff 零变更）；文件域助手收敛 internal.h。

## R0 段位图（行号为 1,893 行口径）

| 行段 | 内容 | 去 |
|------|------|-----|
| 1-96 | includes + 匿名 ns 助手（kFactorGenProp/factorIdOfRow/checkedRow + interpolantEngine/useMono/parseLevels） | internal.h |
| 98-116 | ctor 前导（scroll/content/lay/horizons） | 主文件 |
| 118-151 | factorTable 因子清单构建 | 主文件（ctor 内联） |
| 153-207 | wellFactorSection 井点因子面 | _wellfactors |
| 209-227 | factorFieldEdit（隐藏旧契约）+ cellSize 行（cellCaption 无 objectName → 参数传出） | _interpolation |
| 229-277 | 成图方法 + 策略说明 + 覆盖方式 | _interpolation |
| 279-316 | WS-C5 结构 IDW 边界/格网行 | _interpolation |
| 318-530 | factorAdvancedSection 高级参数面（幂次/方向/软边界/变差函数/协变量/SGS + parameterGroup 五组重排） | _interpolation |
| 532-550 | 等厚引擎顶/底构造面行 | _interpolation |
| 552-659 | 生成/取消按钮 + 生成 handler（22 个捕获 → lambda 顶 findChild，事件期解析等价） | _generate |
| 661-718 | 等值线间距/来源/级别 + 生成等值线 | _generate |
| 720-765 | factorTable 勾选互斥 handler | 主文件（wireFactorTableSelection） |
| 767-1041 | 约束线区：列表/右键语义/参数保存/形状/五类型化入口/编辑行 + 两族重排（directionFamily/breakFamily + moveParam 迁移 + cellCaption 重插） | _constraints |
| 1043-1130 | 旧 IDW 兼容段 + statusLabel + 厚度样本段 + realization 段 | _sections |
| 1132-1168 | wf 反馈接线（8 个 connect → status） | _sections |
| 1170-1207 | 输入陈旧接线（纯 findChild 风） | _sections |
| 1208-1216 | 井点因子触发接线 | _wellfactors |
| 1217-1219 | ctor 尾三刷新 | 主文件 |
| 1222-1228 | showEvent | 主文件 |
| 1230-1270 | refreshThicknessSamples | 主文件 |
| 1274-1309 | bindLayerService | 主文件 |
| 1311-1343 | noteFactorLayer | 主文件 |
| 1345-1354 | checkedFactorLayerId | 主文件 |
| 1358-1379 | checkedFactorIsStructural | 主文件 |
| 1381-1558 | updateEngineRows（方法→参数组可见性联动，178 行） | _interpolation |
| 1560-1663 | updateFactorActionStates（动作态机） | _generate |
| 1665-1686 | setRunBusy/noteRunStage | _generate |
| 1688-1709 | markInputsStale | 主文件 |
| 1711-1788 | selectedLineParams/loadSelectedConstraintLine/refreshConstraintList | _constraints |
| 1790-1876 | 井点因子六方法 | _wellfactors |
| 1878-1893 | newConstraintLineParams | _constraints |

## 切分线定案（与任务「预计 5-7 TU」的偏差及理由）

- 任务预估 `_direction/_break` 分立：实测两族入口在同一构建段（L914-1041）一次
  重排成形（typedRow removeWidget→family 迁移 + moveParam 交织），强拆需造人工
  缝 → 合并为单 TU `_constraints`（约束线域整体）。
- 任务预估 `_style/_results`：等值线段仅 58 行、结果表在井点因子域内 → 分别
  并入 `_generate`（等值线=生成动作族）与 `_wellfactors`（井点因子结果表）。
- 定案 **6 TU**：主文件 + `_wellfactors`/`_interpolation`/`_generate`/
  `_constraints`/`_sections` + `constraintpage_internal.h`。

## 跨段指针处理约定（行为等价依据）

- objectName 命中控件：事件期捕获一律改 lambda 顶 findChild（与文件内既有
  混用风格一致；控件终身存活不销毁，解析时机等价）。涉及：生成 handler
  22 捕获、saveLine handler 5 个高级区 spin、族段 moveParam 5 spin + adv 布局
  + methodCaption。
- 无 objectName 控件（cellCaption/constraintCaption/fieldCaption）：
  cellCaption 经构建段返回值 → ctor 局部 → buildConstraintArea 参数传递
  （ctor 期解析时机不变）；其余均在同段局部。
- moveParam lambda → internal.h 自由函数（adv 参数显式化）。
- ctor 尾序（L1217-1219 三刷新）与全部 connect 序逐行保持；L1205/L1213 两处
  horizons connect 分属 wireInputStaleness/wireWellFactorTriggers，调用序保持
  先 L1205 后 L1213。

## objectName 守恒清单（101 = 88 显式 + 13 动态）

动态：factorValueFieldCombo(+Caption)/factorNumeratorFieldCombo(+Caption)/
factorDenominatorFieldCombo(+Caption)（6）+ factorIdwParameters/
factorVariogramParameters/factorSgsParameters/factorAnisotropyParameters/
factorNeighborhoodParameters（5）+ directionFamilySection/breakFamilySection（2）。
显式 88 个见 rg 对拍（拆分后逐个验证）。

## 迭代记录

| 轮 | 改动 | 验证 | 结论 | 下一步 |
|----|------|------|------|--------|
| R0 | 勘察+切分线定案 | wc/rg/git log | 通过 | 写 internal.h + 头声明 + 六 TU |
| R1 | 六文件落地（internal.h 90 + 主 338 + _wellfactors 182 + _interpolation 557 + _generate 350 + _constraints 418 + _sections 201）；CMake 登记 5 TU | ①101 objectName rg 全命中；②头公共区 diff 零变更（仅 private+前置声明增长）；③多重集行对拍：28 丢行/95 新行全部对应预期变换（签名改写/22 捕获→findChild/moveParam→自由函数/internal.h inline 化）；④layering --strict 绿 | 部分通过 | 全量构建 + 测试 ×2 |
| R2 | 修正：三行 setSpacing 字节级还原；主 TU 补 <QDoubleSpinBox>；pwsh7 + CMake 3.29 接线（VS CMake 3.27 不够） | 待构建 | 进行中 | 全量构建 → ctest 红集合对拍 |

### 多重集对拍对账明细（R1 证据）

- 原文 8 个 `};` = 新 7 个 + moveParam lambda 定义行（转 internal.h 自由函数后消失）✓
- 原文 `connect( generate, ..., [22 捕获] {` 3 行 = 新 `[this] {` + 22 行 findChild（点击期解析）✓
- 原文 saveLine/族段 5 spin 捕获 = buildConstraintArea 顶 7 行 findChild（ctor 期解析，含 adv/methodCaption/cell）✓
- internal.h `inline` 化 6 助手 + moveParamWidget 展开（原文单行 if 复合句 → 多行，语义同）✓
- 无未对账丢行/新行 → 无死代码引入、无语句丢失

### R2-R4 环境事件（记录，非拆分问题）

1. VS 自带 PowerShell 5.1 解析不了 paleo-dev.ps1（PS7 语法）→ 统一 pwsh 7。
2. VS CMake 3.27 < 3.28 → PATH 前置 `C:/Qt/Tools/CMake_64`（3.29.3）。
3. **基线预存问题**：worktree 无 gitignored `vendor/onnxruntime` → paleo_ai 走
   无 ORT 空壳分支 → `aiassistworkflow.cpp:502/545` 无守卫调用
   suggestHorizonTracking/runTileInference → tst_background_open 链接失败。
   按 manifest.json 钉哈希手工 fetch ORT 1.30.0（sha256 校验过）+
   清 paleo_ai 陈旧 autogen（分支翻转后 mocs_compilation 未重生成）后
   826/826 全绿。**与拆分零相关**（我的 diff 不触 ai/workflow）。
4. 编译期修复：internal.h QTableWidget 完整类型（C2027）；_wellfactors/
   _sections 补 <QHeaderView>（C2027）；主 TU 补 <QDoubleSpinBox>。

### Review 轮 1（五维，机械+深读证据）

| 维度 | 证据 | 结论 |
|------|------|------|
| API 等价 | 头公共区 diff 零变更（git diff 证据）；消费方 8 文件零改动（git status） | 过 |
| objectName 守恒 | 101/101 rg 全命中（88 显式+13 动态）；无新增/删除 | 过 |
| 信号序/初始化序 | ctor 14 调用 = 原文行段 1:1；connect 51=51；16 信号 emit 位点逐个对拍相等 | 过 |
| TU 边界 | 6 TU 全 ≤600（最大 _interpolation 557）；主 338 ≤800 | 过 |
| 无死代码 | 多重集对拍无未对账行；六文件零编译警告（build5.log 精滤） | 过 |
| 深读复核 | ctor 编排/互斥 handler/族段迁移三处高风险段逐行比对原文 | 过 |

### R5 全量验证（Oracle 主证据）

- 全量构建：826/826 步绿；constraintpage 六文件零编译警告（build5.log 精滤）。
- 主测试面 ×2 遍（同 build）：tst_factorworkflow/tst_mappingpages/tst_
  constraint_draw/tst_ui 两轮 4/4 全绿（第二遍 217s/33s/4.4s/1.4s）。
- 全量 ctest：381 项，360 绿/21 红。
- **基线红集合对拍（stash → 重建 master 基线 → 同 21 项重跑）**：
  - 基线红 = 16，全部 ⊂ 我的 21 红（每项吻合，含同型崩溃
    tst_seismic_baseline 0xc0000409）。
  - 差集 5 项处置：
    - tst_perf_segyindex / tst_seismicattrperf / tst_errorhub_storm：
      build.ninja 链接行**零 paleo_ui 引用**——构建产物不含拆分代码，
      定义上不可能受影响（负载敏感 perf/压力测试）。
    - tst_officepreview / tst_seismic_3dviz：链接 paleo_ui 但拆分 build
      隔离复跑即绿（全量串行跑机时负载抖动；officepreview 全量日志见
      GDAL sqlite「unable to open database file」环境症状）。
    - tst_perf_segyindex 隔离复跑仍红但其不链接 paleo_ui（同上排除）。
  - 结论：**红集合 diff 中无任何可归因于拆分的项**。
- 基线对拍途中两笔环境账（非源码问题）：stash 回滚后 paleo_ui.lib 残留
  split 版 obj（LNK1236，清 obj+lib 重建即绿）；恢复重建时 MSVC 并行
  cl 无诊断崩溃 8 例（重跑即绿——连编数小时资源瞬态）。
- layering 三档 ctest（layering/layering_strict/layering_selftest）3/3 绿。

### Review 轮 2（零 High/Medium 复审）

轮 1 后无修复项（编译期 4 处 include/完整类型修复在建链阶段已清）。
轮 2 复核：ctor 14 段调用序、互斥单选 handler、族段迁移+断开重连、
saveLine 参数装配、wireInputStaleness 六 connect 序——逐段与原文比对
无行为偏移；无死代码（多重集对拍零未对账行）。Low 3 条记 PR body：
L1 cellCaption 参数传递与 cell findChild 混用（同控件对取法不对称，
无行为差）；L2 生成 handler 22 捕获→23 行 findChild（可读性换 TU 解耦）；
L3 direction/breakLine 先连后断再连（原文原样保留的既有模式，非本次引入）。
