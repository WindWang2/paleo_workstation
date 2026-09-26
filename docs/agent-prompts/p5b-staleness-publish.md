# Agent Prompt — p5b: staleness 链路接线 + 发布门 advisory

你在 paleo_workstation（Qt6/C++20/QGIS 地质工区工作台）上工作。
并行波浪里还有 2 个 agent 在同仓不同 worktree 开发——**严格守住你的文件边界**。

## 准备

```bash
cd /home/kevin/projects/paleo_workstation
git fetch origin && git worktree add ../pw-staleness -b data/staleness origin/master
cd ../pw-staleness
ln -sfn /home/kevin/projects/paleo_workstation/vendor/onnxruntime vendor/onnxruntime
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build -j$(nproc)
```

必读：`AGENTS.md`、`docs/DATA_FABRIC_ADOPTION.md`（B 包 + 发布门）、
`src/catalog/datacatalog.h`（`downstreamClosure`、`markDownstreamStale`、
`verifyExternalVersionSha`）、`src/ui/datapreview/datapreviewtabs.cpp`
（约 :802 的外链 sha 复验门 + wave4 的 relocateMissingSourceWith）、
`src/ui/paleomainwindow.cpp`（`m_refreshPublishGate` 钩子与发布流程）、
`src/workflow/`（发布门实现所在）、`docs/VERSION_PUBLISH_STATE_MACHINE.md`。

## 任务

staleness 标记已持久化（`extra["stale"]`/`extra["staleReason"]`），但两个
产生端/消费端还没接线：

1. **预览侧产标**：`datapreviewtabs.cpp` 外链 `verifyExternalVersionSha`
   失败路径——除现有「找不到源文件/校验不符」文案外，调
   `markDownstreamStale(versionId, "上游外链版本 sha 校验失败")` 让下游
   DERIVED 版本如实标过时。catInvoke/marshal 纪律照现有代码（catalog
   只在 GUI 线程碰）。
2. **发布门 advisory**：发布评估处加一条非阻断项——目标范围内存在
   `extra["stale"]` 的 DERIVED 版本时，发布面板/确认文案如实列出
   「存在过时下游产物（N 个）」；不阻断发布（advisory），但必须可见。
   找 `m_refreshPublishGate` 钩子的实现位置落实。
3. **数据页徽标**（若资产表在 pagepanels 则跳过——那是并行包的文件）：
   stale 资产在预览标签标题或状态行带「过时」标注即可，别跨包改
   pagepanels。

**你拥有的文件**：`src/ui/datapreview/datapreviewtabs.{h,cpp}`、
`src/ui/paleomainwindow.cpp`（**仅** `m_refreshPublishGate`/发布门相关
区域 + 信号接线——`attachWorkflows` 函数体属于并行包，别动）、
`src/workflow/*`（仅发布门实现文件）、`tests/` 新增 `tst_staleness.cpp` 或
追加既有测试、`CMakeLists.txt` 末尾连续块。

**禁止触碰**：`datacatalog.*`、`pagepanels.*`、`dataimportservice.*`、
`ingestplan.*`、editingtoolbar、`attachWorkflows` 内部。

## 规格要点

- stale 标记幂等（datacatalog 已保证同标记不写盘）——重复触发不空涨
  revision，测试可断言。
- sha 失配 → markDownstreamStale 的 reason 用中文一致文案。
- 发布门 advisory 只读 `extra["stale"]`，不阻止、不自动修复。
- 卸载/重开工程后 stale 标记仍在（已持久化）——加个 reopen 用例。

## 纪律

- TDD；ninja 零错误；`ctest --test-dir build` 全绿（基线 67/67 + 新增）。
- 已知 flake（不修）：计时类用例（fiftyWells/correlation）负载敏感；
  `attachWorkflows` 非幂等别二次调；QFSFileEngine 警告无害。
- ninja dyndep assert 修复：`rm build/.ninja_deps build/.ninja_log
  build/CMakeFiles/*/CXX.dd` 后重建。
- 完成：`git commit`（`feat: …` + Devin trailer）、
  `git push -u origin data/staleness`、`gh pr create`（Summary+Test plan）。

## 汇报

commit sha、文件、测试数、接线点行号、偏差清单。
