# 启动加载页（2026-10-19）

启动只显示一个原生加载窗：产品名、工作台说明、当前加载阶段和无百分比的忙碌进度条。
沿用 DESIGN.md 的字体、间距、中性配色和双主题。点击加载窗不会提前关闭。

QgisRuntime 在构造 QApplication、设置 QGIS 前缀后提供回调，组装根通过 AppContext
在该回调中显示启动窗，再初始化 QGIS provider 和工作区服务。QGIS 封装不引用视图。
主窗口构造和工具接线继续更新阶段；主窗口首帧绘制完成后的下一轮事件循环才关闭启动窗。
启动窗在 AppContext 关闭 QApplication 之前析构。无强制停留、无伪进度。

Windows 主程序使用 GUI 子系统，双击 exe 不生成控制台。CLI --help 仍在创建应用前退出。
上次异常退出保留 ErrorHub 的 historyOnly 警告与报告路径，不再自动弹出 QMessageBox。
已有首页的新建、打开和最近工程入口保持可用；没有新增欢迎向导。

验证入口：tst_uxtheme 的启动窗阶段、点击行为、布局和双主题渲染；tst_runtime 的初始化回调；
tst_startup_trace 的真实进程从 startup_screen_shown 到主窗口首帧及 startup_screen_closed
完整顺序和首帧自动退出；Windows GUI 子系统及 --help 单独冒烟。

本机验证：tst_uxtheme（33 项）、tst_runtime（13 项）、真实启动顺序测试均通过；
Windows 原生后端 QT_SCALE_FACTOR=2 下双主题和点击行为通过，PE 子系统为 2（GUI），
--help 返回 0 且输出正常。layering_strict、ui_invariants、i18n 通过。

全量检查仍有未由本改动引入的红项：identifytool 的 Qgs 直连基线、主窗 TU 数文档漂移、
datalist_tree 规模与 identifypopup 的间距令牌；本机启动性能份额门亦未通过
（tools/env_redset.json 已登记 localdeps 宿主差异）。旧便携程序对照的 QGIS 初始化
份额同样超过 0.22 门限；单轮旧版主入口至首帧约 4.18s，新版约 3.95s，
仅作同机冒烟对照，不作为性能基线。未放宽任何检查器或性能阈值。
