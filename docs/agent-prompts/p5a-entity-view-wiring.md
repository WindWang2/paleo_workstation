# Agent Prompt — p5a: EntityView → DataPage 接线（角色槽数据页）

你在 paleo_workstation（Qt6/C++20/QGIS 地质工区工作台）上工作。
并行波浪里还有 2 个 agent 在同仓不同 worktree 开发——**严格守住你的文件边界**。

## 准备

```bash
cd /home/kevin/projects/paleo_workstation
git fetch origin && git worktree add ../pw-entity-view-wiring -b data/view-wiring origin/master
cd ../pw-entity-view-wiring
ln -sfn /home/kevin/projects/paleo_workstation/vendor/onnxruntime vendor/onnxruntime
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build -j$(nproc)
```

必读：`AGENTS.md`、`docs/DATA_FABRIC_ADOPTION.md`（B 包语义）、
`src/catalog/entityview.h/.cpp`（已合并：`entityDataView(cat, entityId)` →
`EntityView{entity, roleSlots[], derivedProducts, missingSources}`，
`RoleSlot{def, primary, members, unresolved}`）、`src/catalog/roleregistry.h`、
`src/ui/pages/pagepanels.h/.cpp`（DataPage 现状）、`src/catalog/datacatalog.h`。

## 任务

把已合并但未接线的 `EntityView` facade 接进 DataPage——实体行现在能看到
「该实体每个角色的主关联/成员/未决 + 缺失角色 + 下游产物 + 悬空父引用」。

**你拥有的文件**：
- `src/ui/pages/pagepanels.h` / `.cpp`（全部）
- `tests/tst_panels.cpp`（追加用例）或新增 `tests/tst_entitypage.cpp`
- `CMakeLists.txt`（若新增测试文件：文件末尾追加一个连续块）
- `src/catalog/entityview.h/.cpp`（仅在必要微调 API 时——改动需注释说明理由）

**禁止触碰**：`datacatalog.*`、`ingestplan.*`、`dataimportservice.*`、
`paleomainwindow.*`、`datapreviewtabs.*`（并行包所有）。

## 规格

1. DataPage 实体选中后，通过 `entityDataView()` 组装角色槽并呈现在
   资产表/链接区——按 registry 顺序枚举角色（含空角色，显示为「缺失」槽位，
   参考 upstream 的 missing-source 可见性原则，UI 样式克制：灰字/「—」）。
2. 每槽显示：角色 display 名、primary（资产名+版本号）、members（按
   `ordinal` 序，非 primary 的其余成员）、unresolved（未决链接计数/名称）。
3. `derivedProducts` 列显示下游 DERIVED 版本（displayName + versionNumber +
   `extra["stale"]` 为真时标「过时」——B 包已落该标记）。
4. `missingSources` 非空时如实显示诊断行（悬空的 parentVersionId）。
5. 空视图（未知 entityId / catalog 未开）→ 空态提示，不崩不猜。
6. 纯查询——facade 不写任何东西；页面刷新走 `catalog.changed()` 信号重取。

## 纪律

- TDD：先写失败用例再实现。每步构建+测试，红灯不扩大改动面。
- 已存测试不许删改（可加）。真数据 fixture：`testdata/project_area`。
- `attachWorkflows` 非幂等——绝不重复调用同一窗口。
- 已知 flake（不要修）：`fiftyWellsRenderUnderThreeSeconds`、correlation
  计时类用例在负载下会抖；`QFSFileEngine::open` 警告无害。
- ninja 崩溃（dyndep assert）：`rm build/.ninja_deps build/.ninja_log
  build/CMakeFiles/*/CXX.dd` 后重建即可。
- 完成条件：本包新用例全绿 + `ctest --test-dir build` 全绿（基线 67/67 +
  新增）+ ninja 零错误。然后 `git add -A && git commit`（仓库 commit
  风格，`feat: …` + `Generated with [Devin](https://devin.ai)` trailer）、
  `git push -u origin data/view-wiring`、`gh pr create`（标题/正文同仓库
  风格，body 含 Summary + Test plan + Devin trailer）。

## 汇报

commit sha、改动文件、测试数、UI 截图路径（offscreen 跑不了就述说结构）、
偏差清单。
