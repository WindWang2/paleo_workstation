# Goal-Loop 方向 80：快速跟随批——#295 后实战面小修收口（岩性词面配置化 + cuttings 引号转义 + 同域补丁文件清单化）

## 背景（实测事实，勿再勘察；行号为 2026-10-08 master `e3c8d31d` 口径）

10-07/10-08 的实战系/codex 系提交（#262/#265/#268/#270/#295 等）
留下了几批「小而急」的债，一次收口：

1. **岩性判词硬编码**：`src/workflow/constraintwellfactors.cpp:51-52`
   砂岩判 `litho.contains("砂岩") || == "砂" || contains("sandstone")
   || == "sand"`；泥岩族同为 if 链（灰岩/白云岩/砾岩等）——
   工区词面差异（如「细砂岩」「粉砂质泥岩」的 contains 归类
   歧义）无配置出口。方向 73 交付时已知，未给方案。
2. **cuttings CSV/TSV 无引号转义**：`src/io/cuttingsdoc.*`
  （#273 新增）解析无引号处理——岩性/描述词含分隔符（逗号/
   制表符在引号内）会列右移；列数恰好够的错位行**进库不报警**
  （TODOS:71-73 原话）。xlsx 路径走 readWorkbook 面不受影响，
   测试只覆盖 CSV+合成表。
3. **cuttings 多版本落选无提示**：同井多份 cuttings 链接取
   currentVersion 最新者，落选文件无提示、段 provenance 统一
   「岩屑录井」不点名文件（TODOS:75-77）。
4. **快捷键硬编码键名两处**：保存按钮提示「保存工程（Ctrl+S）」、
   定位器占位符「搜索井位/层位 Ctrl+K」硬编码——
   `src/ui/shortcuts/` 中央注册表（方向 63 交付）有 `keyFor()`
   可取（TODOS:8-9）。
5. **Delete 遮蔽**：图层树面板主窗级 `WindowShortcut`
  （layers.remove）遮蔽数据树焦点内 Delete（data.assets.remove）
   与节点编辑 Delete（map.vertex.delete）——启动日志已报两条
   shadow 且 `tst_shortcuthelp::knownShadowsArePinned` 钉住；
   候选修法 `Qt::WidgetWithChildrenShortcut`（TODOS:6-7，需
   真机核实图层树内 Delete 仍可用）。
6. **同域补丁文件清单化**（收口副产物）：datalist_tree.cpp
  （#262 重构后 10-07 起连补 6 次）、mappingworkbench.cpp
  （5 次）、appcontext.cpp（4 次）——本方向给这三处做一次
  「补丁归并审视」（不拆文件，只消除明显重复/整理结构），产出后续
   拆分候选清单记 TODOS。

## 环境接线（Windows 本机实测口径；方向 72 后统一链）

```powershell
cd C:\Users\wangj.KEVIN\projects\paleo_workstation
git fetch origin
git worktree add .worktrees\quickfollow -b goal/quickfollow-20261009 origin/master
cd .worktrees\quickfollow
./paleo-dev.ps1 build
./paleo-dev.ps1 test
```

等价手工接线见 BUILDING.md:140-152。

## 目标形态（建议按序）

1. **岩性词面配置化**：domain 层词表——`LithoLexicon`
  （砂族/泥族/碳酸盐族/其他的有序规则表：contains vs 精确
   匹配 + 优先级——「粉砂质泥岩」同时含「砂」「泥」时按
   优先级归泥）；默认词表=现硬编码集（行为保留），支持
   工程级扩展（catalog extra 或 metadata 声明——勘察后定）；
   归类结果进 factor_value 的来源列（可审计）。
2. **cuttings 引号转义**：CSV/TSV 解析加 RFC4180 引号处理
  （"" 转义、跨行字段按规范——若解析器是行制则先核实格式
   声明）；引号内分隔符不再列右移；错位行（列数不符）拒绝
   + 列因（不再静默进库）；夹具：引号内逗号/引号/换行样张。
3. **落选版本提示**：多份 cuttings 解析时落选文件记入
   importledger 或资产 extra（provenance 点名具体文件）；
   面板/图签 provenance 呈「解释·岩屑录井（<文件名>）」。
4. **键名动态化**：两处硬编码改 `keyFor()`（缺注册时回落
   原文案）；Delete 遮蔽按 TODOS 候选修法试修 + 真机核实
  （图层树/数据树/节点编辑三处 Delete 行为全保留才收），
   修不动则保持 pinned 清单并记因。
5. **补丁归并审视**：三文件重复/错位整理（纯内部，行为
   保留）；拆分候选清单（含 datapreviewtabs 族 cpp 1751 +
   internal.h 1579 已知项）记 TODOS 供后续方向。
6. **测试**：词表归类表驱动测试（含歧义词优先级）；cuttings
   引号夹具 round-trip；落选提示断言；键名动态化（改注册表
   键位后 UI 文案跟随——mutation 式）；Delete 三处行为回归。

## 通用纪律（方向内全程有效）

- **分层**：词表归 domain，解析归 io，UI 文案归 ui；
  `check_layering.py --strict` 绿。
- **行为红线**：默认词表下 #295 已验收的归类结果逐井不变
  （对拍——词表化是等价重构）；cuttings 既有合法文件解析
  结果不变（引号处理只影响含引号的行）。
- **诚实面**：Delete 遮蔽若真机核实失败如实保持 pinned +
  记因；归并审视不夹带行为变更。
- **资源**：`./paleo-dev.ps1` 系；ctest 串行。
- **无人值守**：词表 schema 与优先级语义自行定案记 ledger。
- **ledger**：`.goal-loop-ledger-quickfollow.md`。
- **多轮 review（硬要求）**：每批 → 测试全绿 → diff 自审
  （分层/等价重构对拍/解析健壮性/行为保留/i18n 五维）→
  修复 → 再 review，至少两轮零 High/Medium；Low 记 PR body。
- **提交**：原子提交，中文 conventional 前缀。

## Oracle 验收（逐条需验证证据）

1. 词表：表驱动测试含歧义词（粉砂质泥岩等）优先级归类；
   默认词表对 #295 夹具井的归类结果对拍零差异；工程级扩展
   词表生效（若有）。
2. cuttings：引号夹具（内逗号/内引号/内换行/错位列）解析
   ——引号内保留、错位拒收列因（修前红：错位行进库）；
   合法无引号文件对拍零差异。
3. 落选提示：双 cuttings 夹具——provenance 点名文件、落选
   进 ledger/extra（断言）。
4. 键名：mutation（改 shortcutcatalog 键位）后两处 UI 文案
   跟随；Delete 三处行为回归全绿（或 pinned 保守结论）。
5. 归并审视：三文件 diff 零行为变更（对拍测试）+ 候选清单
   入 TODOS。
6. 全量 ctest 对照 R0 红集合 diff 为空；check_i18n 绿。
