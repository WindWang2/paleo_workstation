# Goal-Loop 方向 73：单因素图工作流补全——井点因子提取、约束线两族明确化、插值方法明面化、等值线/面主题规范

## 背景（实测事实，勿再勘察；行号为 2026-10-07 master `dc26c0eb` 口径）

用户 2026-10-07 对「单因素图」页提出四条要求（原文）：

1. 应该从测井中，把对应的单因素提取出来，生成测井点的单因素
   （砂地比……）属性；
2. 应该把单因素约束线的绘制明确，两种，一种是方向线，一种是打断线；
3. 之前内容插值方法这些要写清楚，不要分为高级工具；
4. 注意等值线和等值面显示的主题。

**现状对照（已核实）**：

- **井点因子提取机制在数据层已存在、视图层未暴露**：
  `src/algorithms/singlefactor/wellacquisition.cpp` 支持
  `factorMode = direct`（直读字段）/ `ratio`（分子÷分母）+
  `valueField`/`numeratorField`/`denominatorField`，含缺失值判别
  （`isMissing`）与诊断 hint。砂地比 = 砂层厚度÷地层厚度正是
  ratio 模式的目标形态。但 `src/ui/pages/constraintpage.cpp`
  全文 grep 无 factorMode/valueField/numerator/denominator 任何
  控件——用户无从选择「提取什么、从哪提」。约束页唯一的样本面是
  `thicknessSection`（CollapsibleSection，~line 877）：每井
  顶/底 TVD + 层间速度四列只读表，是「厚度」单指标的镜像，
  不是通用的井点因子面。
- **约束线入口存在但语义摊散**：`constraintpage.cpp` ~764-800
  有五个并列类型化按钮：画方向线(`direction_line`)/画打断线
  (`break_line`)/画软边界(`interpretive_boundary`)/画等值停止
  (`contour_stop`)/画制图绕行(`cartographic_detour`)；另有通用
  绘制路径（shape 6 型 × semantic 5 值：硬屏障/方向引导/解释软边界/
  等值停止/制图绕行 + blockMode 三档）。五个平级入口无分组、
  无语义说明，用户口径「明确为两种：方向线/打断线」意味着把
  约束线收敛为两族一等入口——方向线族（direction_line/
  direction_guide/制图绕行类趋势约束）与打断线族（break_line/
  hard_barrier/contour_stop 类屏障约束），其余语义归入族内选项
  或折叠为次级。
- **插值方法有下拉但参数藏「高级参数」**：成图方法 combo 在主面板
  （~line 178-192，`surfaceMethodPacks()` 词表驱动 +
  「SGS 实现族」专属项，默认 `local_direction_idw`，每项
  tooltip = geologicalNote）；但方法参数全部在
  `CollapsibleSection("高级参数")`（~line 257）内：
  方向线比值 `factorDirectionRatioSpin`、变差函数模型
  （球状/指数/高斯 ~line 327-329）等。ribbon「插值计算」组
  （`ribbonpanels.cpp:347`）只有「计算单因素」大按钮。
  用户口径「插值方法要写清楚，不要分为高级工具」= 方法与
  其参数升为主流程一等区块，按方法联动显示对应参数与说明。
- **等值线/等值面样式**：`src/qgis/factorstylewriter.cpp` 有按
  因子注册的渐变端点对色带表（地图域数据符号，非 UI token，
  方向正确）；`src/qgis/factorcontour.cpp` 生成等值线。等值线
  图层的线色/线宽/注记字体、分级色带与等值线的协调、明暗主题
  下可读性未见统一规范——R0 需核查现样式来源（QGIS 默认？
  写死值？是否随主题）。

**目标**：单因素图页从「样本表 + 五个平级画线按钮 + 高级参数
折叠区」收敛为一条可读的编图流水：选因子提取口径 → 井点因子
表 → 两族约束线 → 明面化插值方法与参数 → 规范主题的等值线/面。

## 环境接线（Linux 本机口径，BUILDING.md「独立 worktree 开发」）

```bash
cd /home/kevin/projects/paleo_workstation
git fetch origin
git worktree add .worktrees/singlefactor-pipeline -b goal/singlefactor-pipeline-20261007 origin/master
cd .worktrees/singlefactor-pipeline
ln -s /home/kevin/projects/paleo_workstation/vendor/superbuild/prefix vendor/superbuild/prefix
[ -d /home/kevin/projects/paleo_workstation/vendor/onnxruntime ] && \
  ln -s /home/kevin/projects/paleo_workstation/vendor/onnxruntime vendor/onnxruntime
[ -d /home/kevin/projects/paleo_workstation/vendor/prefix ] && \
  ln -s /home/kevin/projects/paleo_workstation/vendor/prefix vendor/prefix
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DQGIS_PREFIX=/home/kevin/projects/paleo_workstation/vendor/superbuild/prefix
cmake --build build -j8
```

主回归面：`tst_singlefactor_*` 全族 + `tst_constraint*` +
`tst_factorworkflow` + `tst_contourlevels` + `tst_mappingpages`
（FactorPageTests 类）+ `tst_ui`（约束页断言段）+ layering 三项。

## 目标形态（建议按序）

1. **R0 勘察定案**：
   - 井点因子数据路径：`wellacquisition` 的请求谁构造
     （`src/workflow/constraintfactorjobs*.cpp` /
      `constraintworkflow.cpp`）？井点属性字段源是什么（测井曲线
      统计？解释岩性段？分层厚度？——#273 已落地连井剖面解释岩性
      provider，`wellsectionworkflow` 有岩性段取用先例）。定出
      「测井→井点单因素属性」的最小真实链路：哪些单因素可从现有
      数据算（砂地比=解释砂厚÷层厚、层厚、净毛比等），哪些只能
      直读已有字段。
   - 约束线语义账本：ConstraintStore 词表 +
     `singlefactorrequest`/`singlefactorstrategy` 里五个
     constraintType 与 semantic 值的算法消费面——哪些真的影响
     插值（打断/屏障必须进入 solver 的阻断逻辑），哪些只是标注。
   - 插值方法清单：`surfaceMethodPacks()` 词表全项 + SGS + 各自
     参数面（哪些参数属于哪个方法，从 `setSpin`/`lineParams`
     消费点反查）。
   - 等值线/面样式现值来源：factorcontour 输出图层的
     renderer/symbol 是谁设的、色带与等值线颜色是否冲突、
     注记字体是否走 mono 数字面约定。
   - 每项定案与弃案记 ledger。

2. **井点单因素提取面**：约束页增加「井点因子」区块（建议接替
   或扩展 `thicknessSection` 的位置与视觉权重）：因子口径选择
   （直读字段 / 比值，比值给分子分母字段选择）、字段源下拉由
   实数据驱动、提取结果表 = 每井 因子值（替代或并列现有厚度
   四列，按因子口径动态列头）。提取动作走 workflow 层信号，
   视图只发信号不干活；缺失井在行内注明原因（沿用
   「层间速度或原因」列的既有模式）。`objectName` 稳定命名供
   测试查找。

3. **约束线两族收敛**：「画约束」区重构为两族入口：
   - 方向线族：绘制入口 + 族内语义（方向引导/趋势/绕行类）
     与比值参数（沿用 `factorDirectionRatioSpin`）；
   - 打断线族：绘制入口 + 族内语义（硬屏障/等值停止类）与
     阻断档（沿用 blockMode 三档）。
   词表 id 不变（ConstraintStore/算法消费面不动），变化限于
   UI 组织与文案；每个入口带一句自解释说明（地质语义，
   沿用 muted caption 样式）。原五按钮若保留需归入族内
   次级选择；多边形/矩形/点等 shape 与 semantic 通用路径
   维持可用但不再与两族入口平级抢视觉权重。

4. **插值方法明面化**：成图方法及其参数从「高级参数」折叠区
   升为主面板区块：方法下拉旁给方法说明（现有 geologicalNote
   tooltip 升级为常驻说明文字），按选中方法联动显示该方法的
   参数组（变差函数模型只跟 Kriging 族走，方向比值只跟方向
   IDW 族走，无参方法显示「无参数」）。「高级参数」折叠区只
   留真正的次级项；ribbon「插值计算」组文案同步实际方法名。

5. **等值线/面主题规范**：等值线图层与充填面样式收口为统一
   规范——色带继续走 factorstylewriter 语义表（不动地图域
   配色权），等值线色/线宽/注记字体（mono 数字面）与注记
   密度进统一常量或主题桥接点；明暗主题切换下等值线与注记
   可读性要保证（注记 halo/缓冲按 QGIS 惯例）。规范写进
   DESIGN.md 或对应工作流文档一节，非散落在 cpp 注释里。

6. **测试**：每个目标配回归——井点因子提取（factorMode
   direct/ratio 的 UI→workflow 请求断言）、两族入口
   （objectName + 信号参数断言）、方法-参数联动（切方法
   显隐参数组）、等值线样式断言（线宽/注记存在性）。
   存量 `tst_singlefactor_*`/`tst_constraint*`/`FactorPageTests`
   全绿不可回退；改动 objectName 必须同步测试与 ribbon
   镜像查找。

7. **文档**：`docs/workflows/MAPPING_WORKBENCH.md` 单因素章节
   更新为交付后真实行为；DESIGN.md 若新增地图域样式规范记
   决策日志；TODOS.md 勾选/新增递延项如实登记。

8. **收尾**：工作分支提交（可分逻辑提交），推远端并开 PR——
   按 AGENTS.md 纪律**提 PR 不自行合并**，PR 描述带
   「改动摘要 + 测试面 + 遗留项」。

## 通用纪律（方向内全程有效）

- **层界**：提取/比值计算在 algorithms 或 workflow 层（现有
  `wellacquisition`/`constraintfactorjobs_factor` 落点优先复用），
  UI 只发信号；视图层不得直接 include `algorithms/*`——新通路
  走 workflow/信号编排。
- **词表稳定**：ConstraintStore 的 constraintType、semantic、
  blockMode 取值、`singlefactorrequest` 字段名是持久化与算法
  契约——UI 重组不改词表值；确需新增词表值时在 PR 里单列
  兼容性说明。
- **主题纪律**：UI 面颜色/间距/字体一律走 PaleoTheme token；
  地图域符号（色带、等值线色）属数据语义不进 UI token，
  但要有统一出处与文档口径。
- **范围边界**：本方向只动单因素图工作流面（约束页 +
  其 workflow/算法接线 + 等值线面样式 + 相关 ribbon 组）。
  若勘察发现相邻缺陷（如提取链路本身算错），修复范围在
  本方向内；属独立大改的记 TODOS 移交。
- **构建/测试**：`-j8` 上限；分层头三行注释；新增 UI 控件
  `objectName` 稳定命名；`tools/check_layering.py` 必须绿。
- **进度文档**：重要发现与决策写 `docs/progress/` 或账本，
  不在口头。
