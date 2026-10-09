# Goal-Loop 方向 98：wellsection 家族深化——scene 1,762 行拆分 + 时深对齐消费收口 + 多解释合并策略

## 背景（实测事实，勿再勘察；行号为 2026-10-09 master `32851a1e` 口径）

连井剖面是功能最密集的面（方向 38/69 交付 TVD 域/解释岩性/
真实井距/图片道后），三个结构性缺口：

1. **`wellsectionscene.cpp` 1,762 行**（全仓第六）——剖面
   场景：ColumnItem/GapItem/HeaderWidget/FaultOverlayItem/
   图片道（#311 加入）+ 地震缝 + 深度域渲染。方向 65 拆了
   seismicsection 家族、#312 拆了主窗——wellsectionscene
   是下一座（TODOS 方向 80 候选清单同族）。
2. **时深对齐消费半面**：`WellDeviationSurvey::tvdToMd` 每调用
   O(站数)+百次二分且 pointAt 线性扫段——wellsection 地震缝
   侧已用行级 LUT 绕开（scene 内注释），**治本（站点二分
   查找 + 段内缓存）留 deviationsurvey 专项**（TODOS:69-70
   原话）；fence 读回只取井序、深度域不回读（fencewidget
   loadFromStore 恢复节井集，域/间距跟主面板状态——读回
   语义半面，TODOS:71-73）。
3. **多解释源仲裁**：同井多份解释资产取最新版本号消费、
   无合并语义（TODOS:73-75「多解释者并行工作需显式合并
   策略再立」）；completed→publishLithoAsset 生产者接线
   无端到端测试（TODOS:76-78）。

## 环境接线（Windows 本机实测口径；方向 72 后统一链）

```powershell
cd C:\Users\wangj.KEVIN\projects\paleo_workstation
git fetch origin
git worktree add .worktrees\wellsection-deep -b goal/wellsection-deep-20261010 origin/master
cd .worktrees\wellsection-deep
./paleo-dev.ps1 build
./paleo-dev.ps1 test
```

等价手工接线见 BUILDING.md:140-152。主回归面：tst_wellsection_*
族（workflow/imagetrack/ui）+ tst_deviationsurvey（若有）。

## 目标形态（建议按序）

1. **tvdToMd 治本**：`src/domain/` deviationsurvey——站点二分
   查找 + 段内缓存（建筑：survey 不可变假设下构造时预建
   LUT 或惰性缓存——契约注释钉死）；wellsection 地震缝的
   行级 LUT 绕开点改走治本路径（绕开代码删除或保留为
   快路径——R0 勘察后定）；O 复杂度断言（大数据量比率门）。
2. **fence 读回收口**：loadFromStore 补深度域/间距回读
  （与节井集同存——存储契约扩展；旧文件缺字段回落主面板
   状态并如实标注）。
3. **多解释合并策略（最小）**：不造合并算法——立「显式
   选择 + 来源可见」语义：同井多解释资产时用户可选消费
   版本（面板下拉/对话框），选择持久化；默认最新（现行为
   不变）；题注 provenance 点名具体来源（TODOS:75 之「落选
   文件无提示」一并收——若方向 80 已修 cuttings 侧则统一
   到解释资产侧）。
4. **completed→publish 端到端测试**：wellfacies completed
   信号 → publishLithoAsset → catalog 登记 → 剖面消费
   全链测试（TODOS:76-78 之缺）。
5. **scene 拆分**：按 item 族分 TU（wellsectionscene_<域>.cpp：
   _columns/_images/_faults/_seismicgap）+ internal.h；每 TU
   ≤600 行；主文件 ≤800 行；缓存键语义（imageVersion 代际）
   保留。
6. **测试**：tvdToMd 性能比率门 + 正确性对拍（治本前后逐值
   一致）；fence round-trip；多解释选择持久化；端到端链
   测试；scene 拆分零改动回归。

## 通用纪律（方向内全程有效）

- **分层**：deviationsurvey 归 domain，编排归 workflow，
  scene 归 ui/wellsection；`check_layering.py --strict` 绿。
- **行为保留红线**：治本是等价重构（逐值对拍）；多解释
  默认行为不变（选择是新增面）；fence 读回向后兼容。
- **资源**：`./paleo-dev.ps1` 系；ctest 串行。
- **无人值守**：缓存策略与存储契约自行定案记 ledger。
- **ledger**：`.goal-loop-ledger-wellsection-deep.md`。
- **多轮 review（硬要求）**：每批 → 测试全绿 → diff 自审
  （分层/等价对拍/向后兼容/仲裁语义/缓存键 五维）→ 修复 →
  再 review，至少两轮零 High/Medium；Low 记 PR body。
- **提交**：原子提交，中文 conventional 前缀。

## Oracle 验收（逐条需验证证据）

1. tvdToMd：治本前后逐值一致（对拍断言）；大数据量比率门
  （较基线加速系数入 ledger，禁绝对毫秒）；绕开点处置
   （删/留）有证据。
2. fence：round-trip 测试绿；旧文件回落标注可见。
3. 多解释：选择持久化 round-trip；默认最新行为对拍不变；
   题注点名来源（断言）。
4. 端到端：completed→publish→消费全链测试绿。
5. scene：主文件 ≤800 行、TU 各 ≤600 行（wc 证据）；测试
   族零改动通过×2 遍；全量 ctest 对照 R0 红集合 diff 为空。
