# 2026-10-24 创建工程即崩（0xc0000374）· 井点预览只显 8 行

## 用户症状

- 「创建项目开始导入数据的的似乎就会崩溃」：新建工程后 2 秒内 paleo.exe
  崩溃（Windows 事件日志 APPCRASH `0xc0000374` 堆损坏；18:47 test15、
  19:08 test16 两次，test14 时代另有 7 次 SIGSEGV 报告）。工程目录里的
  `catalog.sqlite` 只剩 156KB 未提交 WAL，0 实体——导入被打断。
- 「数据好像出问题了，只有8个井了？？」：新建工程向导第 2 步井点预览表
  只列 8 行。**这不是数据丢失**：`井坐标.xlsx` 20 口井完好，无头全量导入
  370/370 文件、20 井、520 分层全部成功；预览表被
  `welltable.cpp` 的 `out.preview.size() < 8` 上限截住（#666 起）。

## 根因一（崩溃）：陈旧目标文件导致类布局错配 → 每次建看板堆越界 8 字节

PageHeap + cdb 实锤链：

1. `ProjectStatisticsPage::ProjectStatisticsPage` 构造中写 `[this+0xC0]`
   （`m_decorations` 成员，#689 新增的 5 个成员之一）。
2. 而链接进 exe 的 `DataPreviewTabs::openProjectDashboard` 里
   `new ProjectStatisticsPage` 的 size 实参是 `mov ecx, 0C0h`——按**没有**
   `m_decorations` 的旧头文件编译的陈旧 .obj。PDB 里同名类的布局也确实
   少 `m_decorations`（sizeof=0xC0）。
3. 于是每次打开工区看板都越界写 8 字节。工程创建/打开时主窗口自动开看板
   （#565），导入随即密集操作堆 → 下一次堆校验报 0xc0000374 → 用户看到的
   「创建即崩」。部署到 `C:\paleo-run` 的 paleo.exe 同样带 `0C0h`。

为什么会有陈旧 .obj：主 build 目录当天被两个会话并发 ninja（此前已见
LNK1104/C1083 碰撞），依赖图/对象状态不可信。AGENTS.md 的「独立分支用
独立 worktree/build」正是防这个。**修复 = `ninja -t clean` 全量重建**；
代码库本身无此 bug。当天疑似 #691 引入的「启动堆损坏」很可能同为陈旧
构建假象（其 bisect 结论建立在被污染的 build 上）。

## 根因二（预览 8 行）：预览上限写死 8

`readWellTable` 的 preview 供向导第 2 步预览表用，上限 8 行只是防大表卡
UI；确认页「将建立 N 口井」一直是真值。上限放宽到 200 并注释说明，20 口
井的常规表可整表可见。

## 随附加固（本 PR）

- `refreshSurveyMap()` 改层前先 `canvas()->stopRendering()`：导入期间
  `DataCatalog::changed` 密集触发防抖刷新，内存井位层 truncate/重灌与在飞
  渲染作业并发是第二类真实堆损坏路径（与 PreviewMapCanvas 换层同一纪律）。
- 页面隐藏时跳过地图刷新、`showEvent` 补刷：隐藏期间导入风暴不再空刷画布。
- 回归测试 `dashboardSurveyMapSurvivesImportStorm`（36 井导入风暴 + 隐藏
  跳过 + 重显补刷）；`realSourceImportRepro`（`PALEO_REAL_SRC` 门控的
  真实资料全量导入冒烟，默认 SKIP）。

## 验证

- 无头全量导入（真实 paleo_comp_data，370 文件）：PASS，20 井落库。
- `dashboardAndMapAreIndependentOverviewEntries`（干净重建后）：PASS。
- 风暴回归测试 ×3：PASS。
