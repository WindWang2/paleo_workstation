# [med] 视图层仍直写工程目录文件：#482 收敛的 jsonsidecar helper 调用点全留在 src/ui

## 问题

#482 把三处视图子树各自重复的 QSaveFile 原子写收敛成数据层 helper（src/services/jsonsidecar.h），但调用点全部留在 src/ui 视图树——undo 库、dataops sidecar、井组合 sidecar 仍由视图层落盘，等于把「视图写工程文件」从散落私货升级为跨三处的共享 sanctioned 契约。

## 证据

- 三处调用点（当前 HEAD 逐行读过）：`src/ui/pages/datalist_undo.cpp:163` `paleo::writeJsonFile(path, QJsonDocument(root), "DataPage undo vault")`（同文件 :126 `QDir().mkpath`）；`src/ui/pages/dataops/dataopsmodel.h:78-80`（path = sidecarPath(cat, name) 后 writeJsonFile "dataops sidecar"）；`src/ui/wellcomposite/wellcompositestore.cpp:195-199`（`WellCompositeStore::save()` 写 m_sidecarPath）。
- src/services/jsonsidecar.h:5-6 自述「目录不搬，持久化细节归数据层工具」。
- docs/UI_LAYER_PLAN.md §1 视图行明令「禁止…写目录/工程文件、跑业务编排」；tools/check_layering.py 只判 include 方向（ui→services 合法），此类写入完全不可见。

## 影响

视图层继续持有持久化路径与时机，任何壳/无头场景复用这三类数据都要连 UI 一起拖；契约上「视图只发信号」被实质击穿且无机械护栏。

## 建议

把三个 sidecar 的 load/save 收敛为数据层门面（如 services/sidecarstore 显式 API：undoVaultLoad/Save、dataopsSidecar、wellcompositeSidecar），视图只发意图信号，由功能层/壳落盘。

## 验证

`grep -rn "writeJsonFile\|QSaveFile" src/ui` 在视图树零命中（或仅剩白名单项）；新增 ctest 断言无头路径可完成三类 sidecar 读写。

## 关联

- 契约来源：docs/UI_LAYER_PLAN.md §1、AGENTS.md 分层表；同族：#482。
