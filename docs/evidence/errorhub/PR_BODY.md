# 方向 64：错误面统一——ErrorHub 错误通道 + 通知/模态/状态栏三级呈现（方向 54 修订重发）

## 改了什么
- **`services/errorhub`（数据层，无 QtWidgets）**：错误条目（级别/来源域/标题/正文/首末时间戳/去重键/聚合计数/severe/historyOnly），
  60s **固定窗口**去重（自首次出现计），环形历史 500，`entries(Filter)` 查询面，`errorRaised(entry, firstInWindow)`，全加锁可跨线程。
- **`ui/notifications`**：NotificationCenter（右下角 4 卡预建池；warning 8s / error 12s；severe→窗口模态同键 60s 单弹；info→状态栏 5s）、
  ToastCard（活体主题样式）、ErrorHistoryPanel（级别/文本过滤、TSV 复制、清空）、`PaleoNotify`（视图层统一出口 + 模态保留清单 `ask*`/`report`）。
- **装配**：AppContext 持有 ErrorHub 并 installGlobal；主窗口 `attachErrorHub` 挂呈现层 +「错误历史」dock（「布局与面板」菜单入口——主窗口无字面「视图」菜单）。
- **调用点迁移**：`rg -o "QMessageBox::\w+" src/ui -g "*.cpp"` **169 → 15**（剩余 15 全部在 `ui/notifications` 内部实现）。99 处 warning/information/critical
  + 16 处确认框逐点对照见 `docs/evidence/errorhub/migration-table.md`。
- DESIGN.md「错误呈现」节（先于视觉实现提交）+ Decisions Log；54 任务书加「已修订重发为方向 64」注记；ui-polish E1 / dataops UNDO 同步。

## 行为保留
- 文本逐字透传；入账时机 = 原调用点；返回值/提前 return/回滚语义不变。确认类（是否/确定取消/保存放弃取消/重试取消）**仍模态**，按钮、默认键、图标逐一对应。
- 未安装 ErrorHub 或无存活呈现层（单测、组装根之前）→ 原样回落旧 QMessageBox；既有 tst_panels / tst_welltopseditor_ui / tst_constraintediting 的自动点框逻辑不受影响。
- 调用方在另一个可见的非对话框顶层窗（独立设计器、浮动 dock）→ 保留旧模态并只入账历史，避免通知卡被盖住。

## 验证（如实）
- 本机为非 QGIS 子集构建（Qt 6.8 + GDAL 3.10）：`tst_errorhub`（14）与 `tst_notifications`（19，offscreen）**真编译真运行全绿**，连跑 5 次稳定；子集 R0 红集合 diff 为空。
- 风暴：100 条/秒 × 10s（模拟时钟）每批排队心跳 100/100 被处理、卡片分配恒 4、工作线程 1000 条排队全送达；**无墙钟毫秒断言**。
- 所有迁移的 ui TU、appcontext、main、paleomainwindow*：**仅对 QGIS 4.2.3 / Qt 6.10 头 `-fsyntax-only`**，未链接未运行——需 CI / Windows 全量构建确认。
- CMake：`tst_errorhub LIBS paleo_services`（同 tst_crashreport），`tst_notifications LIBS paleo_ui`（同 tst_taskpanel，经 paleo_ui_deps 拿 QtWidgets）。
- check_layering（默认/--strict/--selftest）绿，--transitive 与 master 同为 11/3；check_i18n 绿；check_ui_invariants clean；git diff --check 净。

## Low（review 记录，未修）
1. info 走状态栏 5s，可能被其他 showMessage 覆盖；全文在错误历史。状态栏把换行压成空格。
2. 应用模态对话框在场时通知卡 ✕ 不可点（仍按时自动收起）。
3. severe 模态以主窗口为父，而非原调用点的对话框父。
4. severe 不入去重键：同文本的 severe 与非 severe Error 合并（当前无非 severe Error 生产者）。
5. 并发 raise 时 ×N 计数可能乱序到达（只影响显示）。
6. 历史面板可见时每 150ms 最多整表重建一次（≤500 行），非通知路径。
7. translations/paleo_zh_CN.ts 未重跑 lupdate（源语言即中文）。
8. ToastCard `setFixedWidth(360)` 与 `kCardWidth` 字面重复；paleonotify include 插在 Qt include 块中间（风格）。
9. 远端已有他人 PR #251（`goal/errorhub-20261006`，方向 54）与本方向同题，合并前需人工裁决取舍。
