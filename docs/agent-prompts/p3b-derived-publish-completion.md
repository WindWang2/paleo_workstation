你在 paleo_workstation 项目实现「派生产物登记 + 发布链收口」包：派生栅格全部入 `artifacts/derived/` 并登记 DERIVED catalog 版本（消灭 /tmp 死链）、发布门加数值残差下限、ONNX 端到端推理冒烟、保存/发布状态机规格文档、速度模型与测线↔CDP 映射（对应 docs/PROJECT_AREA_PLAN.md pass-2 批准的 T26/D10/D15 与 TODOS.md P1 项）。

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
- 权威契约：`docs/PROJECT_AREA_PLAN.md`（pass-2 报告 + T 任务清单 + D 决议）；架构边界 `docs/PALEO_QGIS_PLAN.md`（§41.7 保存/发布语义、§1223 版本提交边界、§40 SeismicMapLink）；视觉 `DESIGN.md`；不做清单 `TODOS.md`。
- 先跑 gstack 自检：`_GS=""; for _D in "${GSTACK_ROOT:-}" "$HOME/.claude/skills/gstack" "$HOME/.grok/skills/gstack"; do [ -z "$_GS" ] && [ -n "$_D" ] && [ -d "$_D/bin" ] && _GS="$_D"; done; [ -n "$_GS" ] && echo GSTACK_OK || echo GSTACK_MISSING`
- 验收数据源：`/home/kevin/projects/paleo_project/data/project_area`。**禁止**把 966 MB SEG-Y 和层位 .dat 提交进仓库；夹具切片放 `testdata/`。

【第一步：建 worktree】
```bash
cd /home/kevin/projects/paleo_workstation
git fetch origin && git worktree add ../pw-derived-publish -b wave3/derived-publish origin/master
cd ../pw-derived-publish   # 之后所有工作在此 worktree 内进行
mkdir -p build && cd build && cmake .. && cd ..
```

【并行会话的接缝——重要】
- 你拥有：`src/workflow/**`（mappingworkflow/onnxworkflow/mapexport 等）、map 版本/发布相关服务、`src/qgis/**`、新增 seismic 服务文件、`tests/` 里本域测试文件（tst_mapping/tst_onnxworkflow 等）、新增 docs 文件。
- 不碰：`src/catalog/datacatalog.*` 的 API 签名（你只用既有 `addAsset/addVersion/linkEntityAsset/BatchSave`——**若确需新 API，先加最小只增量的函数，不改现有签名**）、`src/io/**`（A 包）、`src/ui/**`（C 包；发布按钮/对话框 UI 已存在，你动的是 workflow 与 gate 层）。
- **灰区**：T26 要改 `LayerManifest::declareAlgorithm` 的输出路径——`src/metadata/layermanifest.*` 属 A 包域；你可以改（这是本包核心），但要**最小改动**并在 PR 点名，且别动 A 包可能同时在改的文件。
- `CMakeLists.txt`：三方都会加源文件/测试——你的新增放**一个连续块**并注释 `# wave3/derived-publish`。
- 本会话正在做 Wave-2（异步 IO + TaskPanel + D6/D7/D8/D11 UX），**不要**实现：SegyReader 异步化、任务页进度条、井上图层、预览高度策略、厚度触发按钮、GeoJSON 临时配准。
- 不要更新 `docs/PROJECT_AREA_PLAN.md` 与 `TODOS.md` 的任务勾选——文档由编排会话统一对账；完成项写进 PR body。

【任务背景】
- 现状：厚度/ONNX/facies 等派生栅格经 `tempRasterPath()` 写 `QDir::temp()`，重启即死链（autoplan pass-2 H4 审计）；`LayerManifest::declareAlgorithm` 声明的输出也指向临时路径。catalog 的 DERIVED 版本机制已存在（office→PDF 预览链在用：父版本=RAW 版本 id）。
- 发布门现状：MapVersionStore gate 校验「有条 PDF 记录 + pdf asset id + sha256 + 残差行完备（每井有行或原因）」；pass-2 批准加「数值残差下限」（D10）。
- ONNX：411×641 硬门 + D61 geotransform + 真 ORT 服务已接线；缺一个端到端推理冒烟（D15 批准项）。
- 保存/发布状态机：语义散在 §41.7/§1223，缺一份集中规格。
- SeismicMapLink（§40）：剖面↔地图定位需要 depth↔TWT（井 TD 表已有 TimeDepthTool 插值）与 line-geometry↔CDP 映射（测网 P1-P3 几何 + `PALEO_INLINE_*` 栅格元数据已有）。

【交付物】

1. **T26 — 派生栅格落 `artifacts/derived/` + DERIVED 版本**
   - 所有派生输出（厚度栅格、wells.thickness GeoJSON、facies 多边形、ONNX 预测栅格、未来 processing 输出）写到 `<projectDir>/artifacts/derived/` 下的受管版本路径（沿用 catalog 的 `{stage}/{asset_id}/{version_id}/{filename}` 约定，stage=`derived`）。
   - 每个产物登记 DERIVED catalog 版本，provenance 记父版本（如厚度栅格父版本=D61 时间栅格版本），sha256 入库。
   - `declareAlgorithm`/写派生输出的路径全部改到工程目录；清单损坏时照旧走 tryDeclared 错误通道（别吞错）。
   - 测试：厚度链路产物落在工程目录、catalog 里能查到 DERIVED 版本且 sha 可复验、重启后路径仍有效（新开会话读同一路径断言文件在）。

2. **D10 — 发布门数值残差下限**
   - 在现有完备性检查之上加「数值残差行数 ≥ N」门槛（默认 15/20，按批准记录定参数来源）；不足时发布被拒且原因可读。
   - 测试：构造残差不足场景断言拒发布 + 原因字段；满足场景不影响既有通过用例。

3. **D15 — ONNX 端到端 fixture 推理**
   - 一条真 ORT 推理冒烟：小 fixture 输入 → `OnnxWorkflow` 真服务 → 产出 411×641 栅格断言非空/有限值比例。模型夹具放 `testdata/`（若无小模型，生成一个最小可推理的 .onnx 或复用现有 fixture 策略）。
   - 只跑通证明差异化路径可用，不做精度断言。

4. **保存/发布状态机规格**（§41.7/§1223 集中化）
   - 写 `docs/VERSION_PUBLISH_STATE_MACHINE.md`：保存版本/发布/Published 三者关系、发布后是否可再编辑、工作副本与已发布快照发散规则、undo 边界。与现有 `MapVersionStore` 实现逐条对照——不一致处列成缺口清单（文档内注明「现状 vs 规格」），发现实现 bug 才改代码。

5. **速度模型 + line-geometry↔CDP 映射**（§40 依赖，TODOS P1）
   - 新模块（放 `src/services/` 或 `src/workflow/` 合理处）：depth↔TWT 用既有 TD 插值；map XY ↔ (inline,xline,CDP) 用测网 P1-P3 几何 + 栅格 `PALEO_INLINE_*` 元数据反解。
   - 测试：已知网格参数下 XY→inline/xline 往返一致、TD depth↔TWT 用真实表插值不发散、超网返回明确失败而非夹取。

【验收 Oracle】（全部满足才算完成）
- `ninja -C build` 零错误；`QT_QPA_PLATFORM=offscreen ctest --output-on-failure` 全绿（含新增测试），连跑两遍。
- 新测试覆盖：派生版本登记+sha 复验+重启存活、D10 拒发布场景、ONNX 端到端冒烟、速度模型与 CDP 映射往返。
- `docs/VERSION_PUBLISH_STATE_MACHINE.md` 落盘，含现状/规格对照缺口清单。
- 若 `PALEO_REAL_PROJECT_AREA` 可用：真数据 smoke 不劣化（A1 厚度 36.22m 基准）。

【纪律】
- 不碰清单见【接缝】：`src/catalog` API 签名、`src/io/**`、`src/ui/**` 禁碰；不做 Wave-2 内容。
- 不做：多 realization、相序规则融合、crash 报告、第二工区参数化。
- 提交：worktree 分支上按仓库惯例 commit（小步、说 why）。完成且验收绿后：
  ```bash
  git push -u origin wave3/derived-publish
  gh pr create --title "wave3: derived-asset registration + publish chain completion" --body "$(cat <<'EOF'
  ## Summary
  - <逐项：T26 / D10 / D15 / state-machine doc / speed+CDP>
  #### Test plan
  - [ ] ninja -C build clean; ctest 53+N/53+N green ×2; real-data smoke not degraded
  EOF
  )"
  ```
  若 push/gh 不可用，报告分支名与 commit 列表，不要强行处理。
- 最终报告：测试通过数、改动文件清单、状态机缺口清单摘要、给集成者的冲突提示。
