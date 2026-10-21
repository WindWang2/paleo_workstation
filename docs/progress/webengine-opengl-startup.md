# WebEngine / OpenGL 启动一致性（2026-10-21）

## 问题与修复

`src/app/main.cpp` 原先只在 `QT_PLUGIN_PATH` 为空、且 exe 同目录具有
`plugins/platforms/qwindows.dll` 时默认设置 `QSG_RHI_BACKEND=opengl`。
开发目录启动及预先设置插件路径的启动器会跳过它。Windows 上 Qt Quick
默认使用 D3D11，与同一顶层窗内的 `QOpenGLWidget` 合成要求不一致。

把默认图形后端设置移出便携包路径检测，所有启动方式都在构造
`QgsApplication` 和创建窗口前执行。已有显式 `QSG_RHI_BACKEND` 仍优先。
OpenGL 3.3 Core 默认格式与 `AA_ShareOpenGLContexts` 的既有顺序不变。

Qt 对同一顶层窗口只能使用一种图形 API 的约束见
[QQuickWidget Graphics API Support](https://doc.qt.io/qt-6/qquickwidget.html#graphics-api-support)。

## 本机验证

使用本机 Qt 6.11.2 构建两个独立诊断程序，启动段分别取自修改前、后的
`main.cpp`。设置 `QT_PLUGIN_PATH`，清除 `QSG_RHI_BACKEND`，在同一个
顶层窗口中放置 `QOpenGLWidget` 与 `QWebEngineView`，加载内存 HTML；
窗口使用 `WA_DontShowOnScreen`，不读取用户工程或配置。诊断程序与日志
留在本任务 worktree 的 `build/graphics-probe/`，不进入源码或安装包。

| 观测 | 修改前 | 修改后 |
|---|---|---|
| 默认 `QSG_RHI_BACKEND` | 空 | `opengl` |
| 图形 API 不兼容 / `no QRhi` 消息 | 出现 | 未出现 |
| 网页 `loadFinished` | 成功 | 成功 |
| OpenGL 控件有效且可读回帧 | 是 | 是 |
| 诊断程序退出码 | 2（检测到 API 不兼容） | 0 |

`main.cpp` 通过 MSVC `/Zs` 语法检查；layering strict、i18n、file budget
strict 与 diff 空白检查通过。无基线变更。

验证边界：未复现用户完整应用的崩溃调用栈，未链接或替换用户应用。
两个诊断程序仍均有 Chromium GLES 共享上下文警告；现有 Chromium
软件回退配置未在本次修改。这次证据证明了启动分支遗漏导致的合成 API
不兼容得到修复，不能据此声称所有网页或三维崩溃均已解决。
