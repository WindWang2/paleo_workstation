# 地层对比独立 Web 工作台适配

主窗口 ribbon 在「数据管理」之后提供「地层对比」。中央区域直接显示独立
Web 工作台，切回其他页恢复原面板；再次进入保留当前 Web 会话，不自动刷新。
本页面可以在未打开 Paleo/QGIS 工程时使用。

## 使用

1. 已有服务：在「服务地址」输入地址，点击「连接 / 刷新」。默认地址是
   `http://127.0.0.1:8771/web_prototype/workspace.html`。只填主机与端口时自动补入口路径。
2. 启动本机服务：点击「选择独立项目」，选择包含 `run.py` 和
   `web_prototype/workspace.html` 的外部目录，再点击「启动服务」。已有服务会直接复用。
3. Python：默认使用独立项目的 `.venv`，否则查找系统 `python3` / `python`。
   可点击「选择 Python」指定已装好独立项目依赖的解释器。依赖安装遵循独立项目 README。
4. Web 页面内的文件导入继续使用浏览器文件选择；成果下载弹出 Qt 保存位置选择。
   QtWebEngine 缺失、无屏环境或页面加载失败时会显示原因，并提供系统浏览器入口。

「连接 / 刷新」会重新加载页面；解释的自动保存、计算和撤销由独立工作台管理。
地层对比页将 Ctrl+S 留给 Web 的解释保存，离页后恢复 Paleo 工程保存快捷键。
浏览器 IndexedDB 草稿保存在 Qt 用户数据目录中，独立于主仓库。
Paleo 关闭时只回收它自己启动的服务进程，外部已有服务继续运行。

## 配置与边界

服务地址、项目目录和解释器保存在本机 QSettings 的 `correlationWeb/*` 下，
不写入 Paleo 工程。可使用环境变量覆盖：

- `PALEO_CORRELATION_URL`：服务页面地址。
- `PALEO_CORRELATION_PROJECT_DIR`：独立交付目录。
- `PALEO_CORRELATION_PYTHON`：独立项目 Python 可执行文件。

启动操作只支持本机 HTTP 地址。远程 HTTP/HTTPS 服务使用连接操作。
主仓库只包含 Qt 宿主、连接/启动适配与合成测试；不复制 Web 源码、模型、
井资料、数据库或解释成果，不把外部目录加入 CMake、资源包或安装包。
`workflow/stratigraphicwebsession` 管网络探测与进程；`ui/pages/stratigraphicwebpage`
管显示和 ribbon 意图，`ui/webviewpanel` 管浏览器、下载与宿主主题样式。

## 验证

```bash
cmake --build build -j8
ctest --test-dir build --output-on-failure -j8 -R 'stratigraphicweb|webviewpanel|^tst_ui$|layering'
```

普通测试只使用合成 HTTP 服务和临时 Python 入口，无需私有项目。
真实页面的图形 smoke 可在已有外部测试服务上单独运行：

```bash
QT_QPA_PLATFORM=xcb \
PALEO_CORRELATION_SMOKE_URL=http://127.0.0.1:8771/web_prototype/workspace.html \
build/tst_stratigraphicweb realExternalWorkspaceSmoke
```

可用 `PALEO_CORRELATION_CAPTURE` 指定本机截图路径。真实项目测试应使用
独立数据库副本，避免操作原解释成果；含私有资料的截图不提交到主仓库或 PR。
