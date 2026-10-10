# AI 工具结果结构化呈现（ai-resultviz）— 交付记录

- 分支：`goal/ai-resultviz-20261010`（worktree `.worktrees/ai-resultviz`）
- 范围：AI 助手的工具结果从「160 字符 JSON 摘要」升级为卡内结构化视图——
  分派 / 紧凑表格 / 键值对 / 血缘小图 / 兜底折叠块。纯呈现层：工具执行、
  role=tool 回灌、预算语义零变更（workflow/ai/catalog/services 零 diff）。

## 形态与分派表

```
AiChatToolRunner::toolFinished(call, ok, resultJson)   ← 既有信号，dock 直连
  └─ AiAssistDock::showToolResult → AiToolResultView（src/ui/ai/aitoolresultview）
       ├─ paleo.query_project wells/horizons/assets → 紧凑表格卡
       │    列=井名/类型/井深 或 名称/类型/版本/阶段；高度封顶 192px
       │    「复制表格」→ QClipboard TSV；行点击 → assetNavigateRequested /
       │    entityNavigateRequested（wells 行是实体，导航语义如实分开）
       ├─ paleo.query_project summary → 键值对卡（实体/资产计数 + revision）
       ├─ paleo.query_project well_details → 键值对头（井名/ID/关联数）+
       │    内嵌关联表（角色/资产/主用/版本/阶段；行点击 → 资产定位）
       ├─ paleo.asset_lineage（节点 ≤40）→ 血缘小图
       │    复用数据页渲染核 DerivationGraph（视图层内部复用，零重造）：
       │    出参 nodes/edges 无损还原 paleo::derivation::Graph 喂载；
       │    selection 闭包高亮、节点悬停版本 tooltip（渲染核既有能力）；
       │    「在血缘页打开」/节点点击 → lineageNavigateRequested
       └─ 其余（tile/horizon/facies/未知/失败/空闭包/超限）→ JSON 折叠块
            默认收起；展开 pretty JSON（mono，显示上限 8000 字符截尾如实）
```

宿主接线（paleomainwindow_attach_shell.cpp：attachAiAssistant）：
assetNavigate → showPage("data") + DataPage::selectAsset；entityNavigate →
selectAssetsForEntities；lineageNavigate → focusVersion（先例：
assetActivated 通道）。跳转定位是导航，不是数据修改。

## 纪律与证据

- 只读红线：aitoolresultview 零数据面引用（无 DataCatalog/stores——rg 证据
  见 ledger）；执行面只读由 tst_aichatprojectquery 的 catalog 快照对拍承载。
- 摘要行取舍（review 轮2 定案）：结构化视图（表格/键值对/血缘小图）取代
  160 字符摘要行（信息面更全）；兜底折叠（含成功态）与失败态保留摘要行
  ——DESIGN.md「兜底 = 摘要行为既有 160 字符形态 + 折叠块补全文」。
- 测试 tst_aitoolresultview：分派映射 / 表格内容+TSV 剪贴板 / 行点击导航
  （QSignalSpy）/ KV 两形态 / 血缘 nodeCount 对拍 + 打开信号 / 折叠块
  收起态 + 超限注记 / dock 端到端（假 LLM 服务器发 query_project 工具帧 +
  真 catalog）+ 截图（PALEO_AI_RESULT_SHOT_DIR，docs/progress/ai-resultviz-shots/）。
- 门禁：check_layering --strict / check_i18n / check_ui_invariants --strict
  三绿；AI 测试族 9/9 零改动通过（含 ORT 的 tst_aichattoolloop）。

## 环境（Windows 本机，方向 72 统一链）

- 本 worktree 接入 vendored onnxruntime 1.30.0（vendor/manifest.json 钉哈希
  取包）——master tip 非 ORT 构建有预存断链：aiassistworkflow.cpp 无条件引用
  runTileInference/suggestHorizonTracking（ORT 门控 TU）。CI vendored 构建不
  触发；本方向按 manifest 钉哈希接入后与 CI 同口径（PALEO_HAVE_ORT=TRUE）。
- master tip 另有一处编译断链已在本分支顺手修：#314 给 paleomainwindow.cpp
  加 cancelPreviewData() 调用未带 sectionworkbench.h（本工具链 C2027）——
  单独提交，PR body 单列。
