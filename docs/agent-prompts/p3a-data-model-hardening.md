你在 paleo_workstation 项目实现「数据模型与测试基建加固」包：剥离遗留实体字段、补齐 schema 迁移与并发策略、自建 C++ 算法测试框架、合成最小 SEG-Y 夹具、两份审计文档、非 UI 性能预算（对应 docs/PROJECT_AREA_PLAN.md pass-2 批准的 D12 与 TODOS.md P1 工程评审项）。

【执行方式】
- 预算约 3 亿 tokens。自主推进到【验收 Oracle】全部通过，不中途请示；卡住时换方法而不是退出。
- 单线程串行实现。**禁止使用** workflow / teamwork / swarm / 并行子代理编排（不 spawn_subagent、不开 worker）。所有代码你自己写。
- 按 goal-loop 协议执行（若宿主有 /goal-loop 或 /goal skill 则调用之）：
  - 开始前声明一次：目标一句话、完成条件 Oracle（=下方【验收】全部通过）、迭代上限 40 轮
  - 在工作区根维护 `.goal-loop-ledger.md` 账本；每轮只做一处聚焦改动，亲自跑验证命令，记账 `第N轮 | 改动 | 验证结果 | 通过/未通过 | 下一步`
  - 验收未满足禁止宣告完成；完成后关键验证连跑两遍
- TDD：每个交付物先写 QTest 失败测试（红），再实现到通过（绿）。测试文件用 `add_paleo_test()` 注册。
- 用 /qa、/review 类只读 skill 做自查可以；不要用它们衍生别的执行模式。

【环境】
- 仓库 /home/kevin/projects/paleo_workstation，C++20，Qt 6.11.2，QGIS 4.2.2 系统安装（headers /usr/include/qgis，libs -lqgis_core -lqgis_gui，moc=/usr/lib/qt6/moc）。系统 QGIS 已装，勿 vendor。
- 构建：`ninja -C build`；测试：`cd build && QT_QPA_PLATFORM=offscreen ctest --output-on-failure`。
- 权威契约：`docs/PROJECT_AREA_PLAN.md`（pass-2 报告 + T 任务清单 + D 决议）；架构边界 `docs/PALEO_QGIS_PLAN.md`；视觉 `DESIGN.md`（本包基本无 UI）；不做清单 `TODOS.md`。
- 先跑 gstack 自检：`_GS=""; for _D in "${GSTACK_ROOT:-}" "$HOME/.claude/skills/gstack" "$HOME/.grok/skills/gstack"; do [ -z "$_GS" ] && [ -n "$_D" ] && [ -d "$_D/bin" ] && _GS="$_D"; done; [ -n "$_GS" ] && echo GSTACK_OK || echo GSTACK_MISSING`
- 验收数据源：`/home/kevin/projects/paleo_project/data/project_area`。**禁止**把 966 MB SEG-Y 和层位 .dat 提交进仓库；夹具切片放 `testdata/`。

【第一步：建 worktree】
```bash
cd /home/kevin/projects/paleo_workstation
git fetch origin && git worktree add ../pw-model-hardening -b wave3/model-hardening origin/master
cd ../pw-model-hardening   # 之后所有工作在此 worktree 内进行
cp build/CMakeCache.txt build-cache-seed.txt 2>/dev/null; mkdir -p build && cd build && cmake .. && cd ..
```

【并行会话的接缝——重要】
另外两个 agent 与本会话并行，文件面划分如下，**你越界=合并冲突**：
- 你拥有：`src/catalog/**`、`src/io/**`、`src/services/projectdata.*`、`src/services/timedeptool 相关`、`src/metadata/**`、`tests/` 里本域测试文件、新增 docs 文件、新增夹具/生成器。
- 不碰：`src/workflow/**`（B 包：派生产物/发布门/ONNX/速度模型）、`src/ui/**`（C 包：胶囊/字体/空态/a11y/T28/T29）、`src/ui` 下 webview 相关。
- **灰区**：`D12` 剥 `uwi`/`aliases` 字段时若发现 `src/ui` 或 `src/workflow` 有引用，只做「能编译」的最小删除并在 PR 里点名；禁止顺手重构对方区域。`dataimportservice.*` 正在被本会话改成异步 IO——你的改动集中在字段/校验层，别动其流程结构。
- `CMakeLists.txt`：三方都会加源文件/测试——你的新增放**一个连续块**并注释 `# wave3/model-hardening`；合并冲突时保留双方行。
- 本会话正在做 Wave-2（异步 IO + TaskPanel + D6/D7/D8/D11 UX），**不要**实现：SegyReader 异步化、任务页进度条、井上图层、预览高度策略、厚度触发按钮、GeoJSON 临时配准。
- 不要更新 `docs/PROJECT_AREA_PLAN.md` 与 `TODOS.md` 的任务勾选——文档由编排会话统一对账；你的完成项写进 PR body。

【任务背景】
- DataCatalog 已有：实体/资产/版本/显式关联、managed+external、SHA-256、`schema_version` 门（缺键=当前版本）、`.bak` 轮转、`BatchSave`、`unresolvedLinks()`、装载期坏段跳过、失败后拒写（`refusesWrites`/`m_catalogReady`/`catalogOpenFailed` 信号）。
- 已知遗留：实体模型仍带 `uwi`/`aliases` 旧字段（plan §62 批准剥离）；四类存储（`metadata/project.sqlite`、`project.gpkg`、`.qgz`、`catalog.json`）没有统一版本化/迁移策略；两实例同开一工程的并发写未规定（TODOS P1）；QGIS 官方算法测试基座是 Python/YAML 不可用，需自建 C++ harness；真 SEG-Y 966MB 无法进仓库，测试需合成夹具。

【交付物】

1. **D12 — 剥离 `uwi`/`aliases` 遗留实体字段**
   - 从实体结构、catalog.json 序列化、well 文件解析、folder 导入建井路径中移除这两个字段；读旧 catalog 时忽略该键（向前兼容，不报错）。
   - 查找引用统一走 `name`；`src/ui`/`src/workflow` 里若引用这两个字段，最小删除并在 PR 点名。
   - 测试：旧 catalog（含 uwi/aliases）打开不报错且字段不再回写；新建实体序列化不含两键。

2. **schema 迁移策略 —— 设计文档 + 最小落地**
   - 写 `docs/SCHEMA_MIGRATION.md`：覆盖 `metadata/project.sqlite`（`PRAGMA user_version`）、`project.gpkg`（元数据表或 user_version）、`.qgz`（QGIS 自带版本属性，评估即可）、`catalog.json`（已有 `schema_version`+`.bak`，写清现状）。每类给出：当前版本号、读旧兼容策略、写新版时机、迁移失败行为（拒开并保留原件）。
   - 两实例同开一工程的并发写策略：评估并给出选择（锁文件 / sqlite WAL / 拒第二写实例），写进文档；若实现成本低则落地锁文件方案，否则仅文档。
   - 最小落地：`metadata/project.sqlite` 打开时读/写 `PRAGMA user_version`，未知更高版本拒开；测试覆盖。

3. **C++ 算法测试 harness**（TODOS P1「算法测试框架自建」）
   - `tests/` 下新增一个可复用基座（如 `AlgorithmTestBase`）：能调用 `paleo:*` provider 算法、喂参数 map、对输出栅格做逐像元容差比较、对输出矢量做要素计数+几何近似比较。
   - 用它写 ≥3 个已有算法的用例（IDW、facies polygonize、horizon binning 任选），证明 harness 真能用。
   - 文档一小节说明用法（放测试目录 README 或 harness 头注释）。

4. **合成最小 SEG-Y 夹具**
   - 生成器（python 脚本或 C++ 小工具，放 `testdata/` 或 `tools/`）：产出合法 SEG-Y——3200B textual+400B binary 头、若干道、inline 字（偏移 188）恒为 0、CDP 在偏移 20（本工区约定）、道号连续。
   - 用夹具替换/新增 SegyReader 测试用例：索引建立、单线解码、角点定位（72/76 偏移约定）、范围门拒绝超网道。
   - 夹具文件本身提交进 `testdata/`（KB 级）。

5. **审计文档两份**（TODOS P1，只读审计+写文档，不改算法实现）
   - `docs/ALGORITHM_AUDIT.md`：`docs/PALEO_QGIS_PLAN.md` §10–12 所列每个算法逐项标注：native provider 名、C++/GDAL 实现是否存在、缺口列表（raster contour、距井距离/hub 类等）、建议替代。
   - `docs/CRS_ASSUMPTION_AUDIT.md`：核查 §17 MapContext 单 CRS 假设在代码中的落点——列出所有隐含单一工程坐标的位置、真实项目混合地震工区 CRS 与地图基准的冲突点、建议。

6. **非 UI 性能预算测试**（§41.6 NFR 的可执行化，TODOS P1）
   - QTest 计时用例：工程打开（含 catalog.json+manifest）、folder 枚举、`DataCatalog::save()` 在 ~200 版本规模下的耗时上限。上限值用真数据量测后钉（写宽松 2 倍余量），注释写明量测环境。
   - 只测服务/IO 层，不实例化窗口。

【验收 Oracle】（全部满足才算完成）
- `ninja -C build` 零错误；`QT_QPA_PLATFORM=offscreen ctest --output-on-failure` 全绿（含新增测试），连跑两遍。
- 新测试覆盖：D12 旧 catalog 兼容+不再回写、sqlite user_version 拒开高版本、算法 harness ≥3 用例、合成 SEG-Y 索引/解码/角点、性能预算项各一。
- `docs/SCHEMA_MIGRATION.md`、`docs/ALGORITHM_AUDIT.md`、`docs/CRS_ASSUMPTION_AUDIT.md` 三文档落盘且与代码实际行为一致（doc 里引用的行为要有测试或代码指针）。
- 若 `PALEO_REAL_PROJECT_AREA` 可用：真数据回归不劣化。

【纪律】
- 不碰清单见【接缝】：`src/workflow/**`、`src/ui/**` 禁碰（D12 最小删除除外）；不改 `projectdata.h` 已冻结 API；不做 Wave-2 内容（异步 IO/TaskPanel/D6/D7/D8/D11）。
- 不做：多 realization、相界地质类型、第二工区参数化实现（只审计不实现）、crash 报告机制。
- 提交：worktree 分支上按仓库惯例 commit（小步、说 why）。完成且验收绿后：
  ```bash
  git push -u origin wave3/model-hardening
  gh pr create --title "wave3: data model + test infra hardening" --body "$(cat <<'EOF'
  ## Summary
  - <逐项：D12 / schema migration / algo harness / SEG-Y fixture / audits / perf budgets>
  #### Test plan
  - [ ] ninja -C build clean; ctest 53+N/53+N green ×2
  EOF
  )"
  ```
  若 push/gh 不可用，报告分支名与 commit 列表，不要强行处理。
- 最终报告：测试通过数、改动文件清单、三份文档路径、性能实测数值、给集成者的冲突提示。
