# Goal-Loop 方向 68：Python 脚本面——脚本运行入口 / 内嵌控制台 / 结果落地通道

## 背景（实测事实，勿再勘察；行号为 2026-10-06 master `f130fb2` 口径）

`PythonEnvService` 只做四类原子操作——找解释器/建 venv/装
依赖/解 zip，全部异步 QProcess（`src/services/pythonenv.h:9-11`）。
唯一消费方是 MAMCL 外部工具链：`MamclTool` 状态机 Extract →
CreateVenv → InstallDeps → Launch（`src/workflow/mamcltool.cpp:16`），
从预测页按钮触发（`src/ui/paleomainwindow_attach_predict.cpp:132-157`）。

即：Python 面目前是「外部工具启动底座」而非「嵌入式脚本面」。
缺口（全仓 rg 零命中验证）：

- **无用户脚本运行入口**——用户不能跑自己的 .py 处理脚本。
- **无 Python 控制台/REPL**——无交互面。
- **无脚本面板**——无脚本管理/参数输入/stdout 呈现。
- 基底选型已埋伏笔：`pythonenv.h:20-22` conda-forge + Tk/Xft
   中文 GUI 说明（TODOS「完整 Python GUI 嵌入」递延口径）。

本方向做**进程级嵌入**（QProcess 驱动的脚本运行 + 控制台），
不做 Python C API 解释器内嵌（那是另一量级，递延保持）。

## 环境接线（Windows 本机实测口径）

```powershell
cd C:\Users\wangj.KEVIN\projects\paleo_workstation
git fetch origin
git worktree add .worktrees\python-scripting -b goal/python-scripting-20261007 origin/master
cd .worktrees\python-scripting
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo `
  -DQGIS_PREFIX=C:/Users/wangj.KEVIN/paleo-qgis-prefix `
  -DCMAKE_PREFIX_PATH="C:/deps/Qt/6.8.0/msvc2022_64;C:/Users/wangj.KEVIN/paleo-qgis-deps/Library;C:/deps/qscintilla-install" `
  -DQSCINTILLA_PREFIX=C:/deps/qscintilla-install
cmake --build build -j8
```

坑位同方向 58。无新 vendor 依赖（复用 pythonenv 的解释器发现）。

## 目标形态（建议按序）

1. **脚本运行服务**：`src/services/scriptrunner.{h,cpp}`（数据
  层，QProcess 异步）——运行 .py（venv 或系统解释器，沿用
  pythonenv 发现序）；stdout/stderr 流式回传（信号）；退出码
   + 超时；并发闸（同时 ≤2 脚本，先例 paleotaskservice）；
   取消（terminate→kill 递进，先例 stratigraphicwebsession）。
2. **脚本契约（轻量）**：约定脚本 stdout 的 JSON 行协议
  （progress/result/error 三型）——脚本侧可选遵守（不遵守
   也能跑，输出按纯文本呈现）；契约文档 + 示例脚本进
   `tools/reference/scripts/`。
3. **控制台面板**：`src/ui/python/`——脚本选择/参数（argv
   透传）/运行/停止/输出区（stdout/stderr 分色）；运行历史
  （最近 N 条）；DESIGN.md token 遵守。
4. **REPL 面板（最小）**：交互式 python -i 会话桥（stdin
   写入 + stdout 回显）——不做语法高亮/补全（递延）；明确
   标注实验性。
5. **结果落地通道**：脚本产出的栅格/表格按既有导入词表识别
  （manifest 先例）——脚本输出目录的文件可一键导入（复用
   dataimportservice 面，不新造）。
6. **测试**：scriptrunner 单测（退出码/流式/超时/取消/并发
   闸——夹具脚本 `print`/`sleep`/`exit 1`）；JSON 行协议
   解析单测；offscreen 面板测试（运行→输出呈现）。

## 通用纪律（方向内全程有效）

- **分层**：runner 归 services（无 QtWidgets），编排归
  workflow（若需），面板归 src/ui/python；`check_layering.py
  --strict` 绿。
- **安全**：脚本执行是显式用户动作（无自动执行面）；进程
   沙箱声明（无——如实记「用户自担脚本安全性」于文档与
   面板提示）；不内置任何自动下载执行。
- **诚实面**：无 venv/无解释器 → 禁用态 + 引导（不是空按钮）；
   REPL 标实验性；JSON 协议是可选约定不强制。
- **资源**：`-j8`；ctest 串行。
- **无人值守**：协议设计与并发闸口径自行定案记 ledger。
- **ledger**：`.goal-loop-ledger-python-scripting.md`。
- **多轮 review（硬要求）**：每批 → 测试全绿 → diff 自审
  （分层/进程生命周期正确性/取消语义/安全口径/i18n 五维）→
  修复 → 再 review，至少两轮零 High/Medium；Low 记 PR body。
- **提交**：原子提交，中文 conventional 前缀。

## Oracle 验收（逐条需验证证据）

1. 运行服务：夹具脚本退出码/stdout 流式/超时/取消（terminate
   →kill）/并发闸（第 3 个排队或拒收——按定案断言）全绿。
2. JSON 行协议：progress/result/error 三型解析单测；不遵守
   协议的脚本按纯文本呈现（测试）。
3. 面板：offscreen 运行→输出分色呈现；历史记录;参数 argv
   透传（夹具回显 argv 断言）。
4. REPL：基本往返（输入 print(1+1) 回显 2，offscreen 测试
   或进程级测试）；退出干净（无孤儿进程——进程枚举断言）。
5. 结果落地：脚本产出 GeoJSON/CSV 被识别进导入面（词表
   命中测试）。
6. 全量 ctest 对照 R0 红集合 diff 为空；check_i18n 绿。
