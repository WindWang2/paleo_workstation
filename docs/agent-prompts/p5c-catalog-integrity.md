# Agent Prompt — p5c: catalog 词表强制 + attachWorkflows 幂等化

你在 paleo_workstation（Qt6/C++20/QGIS 地质工区工作台）上工作。
并行波浪里还有 2 个 agent 在同仓不同 worktree 开发——**严格守住你的文件边界**。

## 准备

```bash
cd /home/kevin/projects/paleo_workstation
git fetch origin && git worktree add ../pw-catalog-integrity -b data/integrity origin/master
cd ../pw-catalog-integrity
ln -sfn /home/kevin/projects/paleo_workstation/vendor/onnxruntime vendor/onnxruntime
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build -j$(nproc)
```

必读：`AGENTS.md`、`docs/DATA_FABRIC_ADOPTION.md`（A 包 + defer 清单）、
`src/catalog/datacatalog.h/.cpp`（addLink/addEntity/addVersion、
`roleRegistry()`、unresolved 语义）、`src/catalog/roleregistry.h`、
`src/ui/paleomainwindow.cpp`（`attachWorkflows`——现状：重复调用会重复建
dock/信号连接并致后续用例段错误）。

## 任务

两件事，都是「让基建真正管事」：

1. **role 词表强制（诚实降级而非硬拦）**：`addLink`/`attachLink` 收
   role 时经 `roleRegistry().isKnown(role)` 校验——
   - 已知 role：照常。
   - 未知 role：**仍写入**（诚实——不丢数据、不静默改词），但
     `link.note` 末尾追加「未知角色: <role>」诊断（已有 note 用 `；`
     连接）；同时 `qWarning` 一次。实体类型不在该 role 的
     `entityTypes` 里同样只诊断不拦。
   - 新 API：`DataCatalog::invalidRoleLinks()` 返回带诊断的链接集
     （扫描 note 或缓存），供诊断面用。
   - 不加 hard-reject：词表是工程自定义的（project_area.json 可扩），
     硬拦会把合法自定义挡在旧二进制外。
2. **attachWorkflows 幂等化**：同一 PaleoMainWindow 二次调用不得重复
   建 dock/连接/崩溃。最小正确做法：函数入口早退守卫
   （`if (m_workflowsAttached) return;` + 成员旗标）或逐段对象名查重——
   选旗标，附注释说明为什么幂等（测试套件会二次触达）。修后写一个
   直接证据用例：同一窗口连续 `attachWorkflows` 两次，dock 数量不翻倍、
   不崩。

**你拥有的文件**：`src/catalog/datacatalog.h/.cpp`（addLink/attachLink
区 + invalidRoleLinks 新段——**别动** linksForEntity/ordinal/closure/stale
区，那是已合并 B 包）、`src/ui/paleomainwindow.{h,cpp}`（**仅**
attachWorkflows 函数体 + 私有成员旗标——m_refreshPublishGate 区域属并行包）、
`tests/tst_catalog.cpp`（追加用例）或新测试文件、`tests/tst_ui.cpp`
（attachWorkflows 幂等用例——**追加到类末尾**，现有用例顺序不得动，
它们共享 attachWorkflows 状态）、`CMakeLists.txt` 末尾连续块（如加新测试）。

**禁止触碰**：`pagepanels.*`、`datapreviewtabs.*`、`ingestplan.*`、
`dataimportservice.*`、`roleregistry.*`（只读用）、editing 相关。

## 纪律

- TDD；ninja 零错误；`ctest --test-dir build` 全绿（基线 67/67 + 新增）。
- 幂等用例单跑+连跑套件都要过（这正是原 bug 的表现面）。
- 已知 flake（不修）：计时类用例；QFSFileEngine 警告无害。
- ninja dyndep assert：`rm build/.ninja_deps build/.ninja_log
  build/CMakeFiles/*/CXX.dd` 后重建。
- 完成：`git commit`（`feat: …` + Devin trailer）、
  `git push -u origin data/integrity`、`gh pr create`（Summary+Test plan）。

## 汇报

commit sha、文件、测试数、幂等守卫的实现选择、偏差清单。
