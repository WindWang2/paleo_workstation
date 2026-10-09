# Goal-Loop 方向 97：datapreviewtabs 家族拆分——cpp 1,758 + internal.h 1,607 双体解体（候选清单榜首）

## 背景（实测事实，勿再勘察；行号为 2026-10-09 master `32851a1e` 口径）

`src/ui/datapreview/` 预览面板族是**全仓最大单族**：

- `datapreviewtabs.cpp` **1,758 行** + `datapreviewtabs_internal.h`
  **1,607 行**（合计 3,365）——九类资产预览面板的门面
 （LAS/剖面/文档/图片/GeoJSON/层位/栅格/测井综合/成果图件）。
- **TODOS.md:21-31 方向 80 拆分候选清单榜首**（#302 归并审视
  产出，开放项）——#310 曾会删它（已防），本方向正式执行。
- 族内文件：`datapreviewtabseismic.cpp`、`datapreviewtabwelllog.cpp`、
  `datapreviewtabhorizon.cpp`、`datapreviewtabgeojson.cpp`、
  `datapreviewtabimage.cpp`、`datapreviewtabmapproduct.cpp`
 （#296 析出的 372 行独立 TU——先例）、`previewmappage.cpp`、
  `previewidentifypanel.cpp`、`previewtocpanel.cpp`、
  `previewhistogramwidget.cpp`、`previewprofilepanel.cpp`、
  `previewmapstates.cpp` 等。
- **previewdoc 门面**：方向 59 收口后 `src/ui` 内 30 个文件
  include previewdoc.h（视图层内部合法消费）——本方向不碰
  previewdoc（那是 59 号已收口的面），只拆 datapreviewtabs
  本体。

## 环境接线（Windows 本机实测口径；方向 72 后统一链）

```powershell
cd C:\Users\wangj.KEVIN\projects\paleo_workstation
git fetch origin
git worktree add .worktrees\previewtabs-split -b goal/previewtabs-split-20261010 origin/master
cd .worktrees\previewtabs-split
./paleo-dev.ps1 build
./paleo-dev.ps1 test
```

等价手工接线见 BUILDING.md:140-152。主回归面：tst_previewdoc/
tst_previewmap_canvas/tst_panels（预览面板族）。

## 目标形态（建议按序）

1. **R0 结构勘察**：DataPreviewTabs 门面按资产类型盘点
  （九类 tab 的构建/切换/取数接线）+ internal.h 的共享类型
  盘点（哪些类型只服务单 tab——可下沉到 tab 自有头）；
   30 个消费 TU 的 include 面（哪些只用了门面的哪部分——
   最小 include 面机会）；切分线记 ledger 定案。
2. **公共 API 冻结**：DataPreviewTabs 类定义公共区 diff 对拍
   零变更；消费方零改动。
3. **internal.h 瘦身**：共享类型按 tab 归属拆分（单 tab 专用
   类型进 tab 自有头；真共享的留 internal.h）——目标
   internal.h ≤800 行。
4. **门面拆分**：tab 构建接线按资产族分 TU
  （datapreviewtabs_<族>.cpp：_seismic/_well/_map/_doc/…）；
   每 TU ≤600 行；主文件 ≤700 行（门面+切换逻辑）。
5. **include 面机会**：30 个消费 TU 的 previewdoc include
   逐个核——若部分 TU 只需窄出参（如仅 LAS 头），可改窄
  （不强制；有证据才做，避免为拆而拆）。
6. **行为红线对拍**：tab 切换/取数/预览语义逐点不变；
   tst_previewdoc 族零改动通过×2 遍。

## 通用纪律（方向内全程有效）

- **分层**：全部留 `src/ui/datapreview/`（视图层）；新文件
  头三行 `// 层：视图`；`check_layering.py --strict` 绿。
- **行为保留红线**：九类预览的取数/渲染/关闭语义逐条不变；
  「顺手改进」违规记 TODOS。
- **资源**：`./paleo-dev.ps1` 系；ctest 串行；全量构建后再
  ctest。
- **无人值守**：切分线与 include 收缩面自行定案记 ledger。
- **ledger**：`.goal-loop-ledger-previewtabs-split.md`。
- **多轮 review（硬要求）**：每批 → 测试全绿 → diff 自审
  （API 等价/internal 归属/include 纪律/TU 边界/无死代码
  五维）→ 修复 → 再 review，至少两轮零 High/Medium；Low 记
  PR body。
- **提交**：原子提交，中文 conventional 前缀。

## Oracle 验收（逐条需验证证据）

1. 类零变更：DataPreviewTabs 公共区 diff 零变更；消费方零
   改动（rg 证据）。
2. 体量：主文件 ≤700 行；internal.h ≤800 行；新 TU 各
   ≤600 行（wc 证据入 ledger）。
3. 行为等价：tst_previewdoc/tst_previewmap_canvas/tst_panels
   预览族零改动通过×2 遍；九类 tab 打开/切换/关闭回归
  （offscreen 抽查 ≥5 类）。
4. include 面：有证据的窄化清单（或「评估后不做」记因）
   入 ledger。
5. 全量 ctest 对照 R0 红集合 diff 为空；layering 三档绿；
   TODOS.md:21-31 候选清单划掉本项。
