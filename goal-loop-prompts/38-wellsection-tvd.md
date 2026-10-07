# Goal-Loop 方向 38：连井剖面 TVD 域 + 真实井距 + 解释岩性道

> **修订注记（2026-10-07）：本方向已修订重发为方向 69
> （`goal-loop-prompts/69-wellsection-tvd.md`），勿按本任务书执行。**
> 史实：方向 38 的 20261004 分支实现已移植进方向 69 分支
> `goal/wellsection-tvd-20261007`；69 修订了三处口径——无测斜如实标注
> （取代 38 的「直井显式语义」）、岩性 provider 形态定案（
> `WellLithologyProvider` + wellfacies 落 DERIVED 资产）、深度域入剖面
> 状态。本文件保留作历史记录。

## 背景（实测事实，勿再勘察）

连井剖面首版只走 MD 深度域（TODOS P3 递延项，数据契约需逐个核实）：

- 现状：`src/domain/wellsection.*`（井序/连线/取数模型）、
  `src/workflow/wellsectionworkflow.*`（世代化取数，currentGeneration
  过滤陈旧结果）、`src/ui/wellsection/`（panel/scene/style/dialogs/fence）。
  分层/LAS/时深表均按 MD 列消费；岩性道是 GR 截断推断的砂/泥二分，
  **不读解释岩性**；井间距等距，不按实际井距；道模板/主题走用户级
  QSettings 不进工程。
- TVD 数据源：`WellLogSet`/`lasdoc` 读面有曲线读取——先勘察
  `readCurveTvd`/时深表资产是否已在 catalog 有角色与版本，
  无则如实标「无 TVD 数据」禁用，不伪造。
- 解释岩性：需 catalog 角色词表勘察（解释岩性资产是否可挂接）；
  GR 推断二分保留为无解释岩性时的回落，不删除。
- 真实井距：井口坐标在 catalog/井位层已有（wellsection band 已按
  customProperty("paleoLayerId")=="wells" 解析坐标）——井距比例模式
  是渲染域改动（等距→按地理投影距），数据零新增。
- 栅状图 fence、断层投绘、剖面-平面 band 为方向 33 已落部件——
  TVD/井距/岩性改动须三处行为一致，ledger 记逐处核对。

## 环境接线

```bash
cd /home/kevin/projects/paleo_workstation && git fetch origin
git worktree add .worktrees/wellsection-tvd -b goal/wellsection-tvd-20261004 origin/master
cd .worktrees/wellsection-tvd
ln -s /home/kevin/projects/paleo_workstation/vendor/superbuild/prefix vendor/superbuild/prefix
ln -s /home/kevin/projects/paleo_workstation/vendor/onnxruntime vendor/onnxruntime
[ -d /home/kevin/projects/paleo_workstation/vendor/prefix ] && \
  ln -s /home/kevin/projects/paleo_workstation/vendor/prefix vendor/prefix
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DQGIS_PREFIX=/home/kevin/projects/paleo_workstation/vendor/superbuild/prefix
cmake --build build -j8
```

## 目标形态（建议按序）

1. **深度域切换**：MD/TVD 域选择（面板控件 + workflow 取数参数）——
   分层/曲线/井间缝全域一致；某井缺 TVD → 该井如实标原因不下拽邻居；
   无 TVD 数据契约 → 切换项禁用带说明。
2. **真实井距**：等距/按距双模式切换；按距模式用井口坐标投影距，
   缺坐标井如实标（列名+原因）不挤进比例轴；剖面图/fence/band 三处一致。
3. **解释岩性道**：catalog 角色勘察后接解释岩性数据源（色板走
   GeoPatterns 词表或工程图式）；无解释岩性 → GR 二分回落并在道上
   标明推断口径（不混充解释成果）。
4. **联动一致性**：fence 栅状图、wellsection band 高亮、断层投绘三处
   随深度域/井距模式同步刷新；世代过滤（currentGeneration）不被
   新参数绕过——域切换作废在途代。

## 通用纪律（方向内全程有效）

- **分层**：`src/` 新文件头三行 `// 层：<词表>`；域转换/取数归
  domain/workflow，渲染归视图层；视图只发信号。
- **诚实面**：无 TVD/无解释岩性/缺坐标如实标，GR 推断与解释成果
  视觉可分（色板或标注可辨）。
- **资源**：构建/测试一律 `-j8`。
- **UI**：对照 `DESIGN.md`；道模板视觉沿用既有 token；i18n 两门。
- **性能断言**：禁绝对毫秒墙钟；剖面重绘用抽样式计时+比率门。
- **ledger**：`.goal-loop-ledger-wellsection-tvd.md`。
- **多轮 review（硬要求）**：每批 → 测试全绿 → diff 自审（分层/世代语义/
  数据诚实/三处一致性/i18n 五维）→ 修复 → 再 review，
  **至少两轮零 High/Medium**；Low 记 PR body。
- **提交**：原子提交，中文 conventional 前缀。

## Oracle 验收（逐条需验证证据）

1. TVD 域 round-trip：带时深表井集的剖面按 TVD 渲染，数值与源表一致；
   缺 TVD 井如实标原因。
2. 真实井距：坐标距悬殊的井集按距模式呈非均匀间距，等距模式还原；
   缺坐标井不参与比例轴且有名录。
3. 解释岩性：带解释岩性资产的井集岩性道按解释着色；无资产井走
   GR 二分且标注「推断」口径。
4. 一致性：剖面/fence/band 三处随域切换同步；域切换后在途结果按
   世代丢弃不错位。
5. 空态：零井/无 TVD/无坐标各场景有文案无异常。
