# Goal-Loop 方向 51：AI 辅助面升级——Mock 路由接线 + LLM 地质对话助手骨架

## 背景（实测事实，勿再勘察；行号为 2026-10-05 master `adf2be7` 口径）

src/ai/ 现状（18 文件 2,819 行）是「模型推理辅助」而非「对话 agent」：

- **远端路由产品侧跑 Mock**：`src/workflow/mappingworkbench.cpp:76`
  构造函数内自装 `new MockRemotePredictionService(this)`；
  `setPredictionService`（`:140`）全仓仅 4 处（定义、自装、测试注入
  tst_mappingworkbench.cpp:542、router 自身）。appcontext（app 装配根）
  从未接 router——产品里远端预测走的是 Mock。
- **`MockRemotePredictionService` 定义在生产头** `src/ai/remotepredictionservice.h:44`
  ——Mock 与生产同头是测试物泄漏进产品面。
- **无 LLM 对话面**：全仓无消息历史、function calling、多轮上下文
  管理代码。现有能力=ORT tile 分类（onnxpredictionservice/tileinference）、
  层位建议（horizonsuggest）、测井相 HTTP（wellfaciesservice，https-only
  + QtKeychain + 64MiB 上限）。
- **阻塞网络**：`src/ai/remotepredictrouter.cpp:37-45` QEventLoop
  嵌套等 `QNetworkReply::finished`——GUI 线程阻塞风险。
- **模型注册**：`src/ai/modelregistry.cpp` manifest 扫描，缺 manifest
  走开发降级（目录枚举 *.onnx）；夹具全是微型 Mul/Add 图
  （onnxfixture，手写 protobuf）。

本方向 = 补「AI 助手」的最后一块拼图：把推理路由诚实地接进产品
（Mock 显式声明），并立起 LLM 对话助手的最小可用骨架（本地/远端
可配置，无第三方 SDK，纯 QNetworkAccessManager）。

## 环境接线（Windows 本机实测口径，源 goal/sf-kriging 账本 R0）

```powershell
cd C:\Users\wangj.KEVIN\projects\paleo_workstation
git fetch origin
git worktree add .worktrees\ai-assist -b goal/ai-assist-20261006 origin/master
cd .worktrees\ai-assist
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo `
  -DQGIS_PREFIX=C:/Users/wangj.KEVIN/paleo-qgis-prefix `
  -DCMAKE_PREFIX_PATH="C:/deps/Qt/6.8.0/msvc2022_64;C:/Users/wangj.KEVIN/paleo-qgis-deps/Library;C:/deps/qscintilla-install" `
  -DQSCINTILLA_PREFIX=C:/deps/qscintilla-install
cmake --build build -j8
```

坑位同方向 48（PATH 顺序/沙箱临时目录拒绝——R0 基线对照）。
无新 vendor 依赖（LLM 走 HTTP，不引入 SDK；密钥复用 QtKeychain
先例 wellfacieskeystore）。

## 预算（开发量）

- **Agent tokens**：上限 3 亿，预计 0.9–1.5 亿（新功能面：对话域
  模型 + UI 面板 + 测试）。
- **执行花费**：12–18 轮构建+测试，预计墙钟 2–3 个工作日会话。

## 目标形态（建议按序）

1. **路由接线**：appcontext 装配 router；Mock 拆出生产头进测试
  目录（或独立 testsupport 头）；产品启动时远端未配置 → UI 显式
  「远端预测未配置，走本地引擎」状态，不再无声 Mock。
2. **阻塞解除**：remotepredictrouter QEventLoop 改异步信号链
  （先例：seismictaskservice 的任务/取消/世代号范式）；超时与
  失败降级语义保留（本地 ORT 回落）。
3. **对话域模型**：`src/ai/chat/`（新）——消息/角色/工具调用帧
  的 domain 结构（纯数据，无 Qt Widgets）；会话持久化到工程外
  用户目录（QStandardPaths，不进工程文件）。
4. **LLM 客户端**：`src/ai/chat/llmclient.{h,cpp}`——OpenAI 兼容
  chat completions 协议（POST /v1/chat/completions，SSE 流式）；
  端点/模型/密钥 QSettings + QtKeychain；无密钥=诚实禁用态。
4b. **系统提示与领域工具表**：把现有三条推理能力（tile 分类/
  层位建议/测井相）注册为可枚举「领域工具」描述表（名称/入参/
  出参 JSON schema）——本方向只做描述表与调用分发，不做真正
  function-calling 闭环（递延，记 TODOS）。
5. **UI 面板**：`src/ui/ai/` 助手 dock（对话流、流式渲染、工具
  调用占位卡片、错误态）；DESIGN.md token 遵守；tr() 全覆盖。
6. **测试**：llmclient 用本地 QHttpServer 或 socketpair 假端点
  测协议/超时/流式拼接；对话域模型 round-trip；Mock 路由装配
  断言（appcontext 装配后 router 非 Mock）；**remotepredictionservice.h
  的测试套件由本方向承接**（52 号协调注记的对端）。

## 通用纪律（方向内全程有效）

- **分层**：域模型归 `src/ai/chat/`（数据层），客户端归 ai（数据层，
  无 QtWidgets），编排归 workflow，面板归 `src/ui/ai/`（视图层），
  装配在 appcontext。新顶层子目录无需 new_module.sh（仍在 ai 词表
  目录内，但头三行 `// 层：数据` 必须有）。
- **诚实面**：未配置密钥=禁用态不冒充；Mock 不再出现在生产装配
  路径；工具表是「描述+分发」不冒充已执行的 function calling。
- **安全**：密钥只进 QtKeychain；HTTPS-only；不做遥测；对话历史
  只落本机。
- **资源**：构建/测试 `-j8`；ctest 串行。
- **无人值守**：所有决策自行定案记 ledger；协议细节按 OpenAI
  官方公开文档口径，不猜测。
- **ledger**：`.goal-loop-ledger-ai-assist.md`。
- **多轮 review（硬要求）**：每批 → 测试全绿 → diff 自审（分层/
  安全/协议正确性/诚实面/i18n 五维）→ 修复 → 再 review，至少两轮
  零 High/Medium；Low 记 PR body。
- **提交**：原子提交，中文 conventional 前缀。

## Oracle 验收（逐条需验证证据）

1. 产品装配：appcontext 装配后 `MappingWorkbench` 的 prediction
   service 来自 router（非 Mock）——装配断言测试 + rg 证据
   （mappingworkbench.cpp:76 自装行删除或改为可注入默认）。
2. Mock 退场：`rg "MockRemotePredictionService" src/` 仅测试支撑
   头命中；生产头零 Mock 定义。
3. 阻塞解除：remotepredictrouter 全文件零 QEventLoop（rg 证据）；
   超时/降级/取消语义有回归测试（先例：世代号守卫测试）。
4. 对话骨架：消息/会话 round-trip 测试过；会话文件落
   QStandardPaths 且不进工程文件（测试验证）。
5. LLM 客户端：假端点下请求/响应/流式 SSE 拼接/超时/错误分类
   测试全绿；无密钥时 UI 呈禁用态（测试+截图）。
6. 工具表：三条既有能力描述表可枚举、schema 合法（JSON 校验
   测试）；分发路由到既有服务入口（假服务测试）。
7. i18n：新增 UI 文案 tr() 全覆盖，check_i18n.py 绿。
