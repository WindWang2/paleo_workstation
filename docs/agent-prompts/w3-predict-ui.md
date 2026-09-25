你在 paleo_workstation 项目让预测页的算法选择真实工作。

【执行方式】本任务必须使用 teamwork-preview 模式执行：自行拆解子任务
（下拉填充 / ONNX 参数区 / 测试更新），能并行的部分用 teamwork-preview
并行开 worker，不能并行的串行。

【环境】仓库 /home/kevin/projects/paleo_workstation。C++20，Qt 6.11.2，
QGIS 4.2.2，moc=/usr/lib/qt6/moc，offscreen QTest。

【第一步：建 worktree】
cd /home/kevin/projects/paleo_workstation
git worktree add ../pw-predictui -b wave/predict-ui
cd ../pw-predictui

【现状】src/ui/pages/pagepanels.{h,cpp} 的 PredictPage：horizonCombo +
algoCombo + run 按钮，algoCombo 目前手填占位；runRequested(horizon, algId)
已连到 PredictionWorkflow::runPrediction(horizon, algorithmId, params)。
PredictionWorkflow 刚加了 availableAlgorithms() — 返回 "paleo:*" +
"onnx:<model>" 混合列表（读 src/workflow/workflows.h 确认签名）。
ONNX 路径 params 键："input" QVariantList<float>、"shape" QVariantList<qint64>、
"inputName" QString（读 workflows.cpp 的 onnx: 分支）。

【交付物 — 只许改这三个文件】
1. src/ui/pages/pagepanels.h — PredictPage 信号改为
   runRequested(QString horizon, QString algorithmId, QVariantMap params)；
   setAlgorithms(QStringList) 已有——确认其被 attachWorkflows 用时传
   wf->availableAlgorithms()；算法 id 存 itemData（display 文本可美化：
   "onnx:toy" 显示为 "toy (ONNX)"）。加一个 params 输入区：对 onnx:* 算法
   显示 3 个 QLineEdit（input 逗号分隔 floats / shape 逗号分隔 ints /
   inputName 默认 "x"），对 paleo:* 隐藏。QComboBox::activated 时切换显隐。
2. src/ui/pages/pagepanels.cpp — 实现以上；parseInputParams() 私有方法把文本
   框解析成 QVariantMap（非法输入 → QVariantMap 留空并在 statusLabel 提示，
   不 emit）。
3. tests/tst_panels.cpp — 更新 PredictPage 测试：spy runRequested 三个参数；
   喂 setAlgorithms({"paleo:x","onnx:toy"}) 后 combo 两项，data 正确；
   切到 onnx 项 params 区可见；填 "2.0" / "1" / "x" 点击 run → params map
   键值对正确（input=={2.0} shape=={1} inputName=="x"）；非法 input
   （"abc"）点击不 emit；ConstraintPage 的 drawConstraintRequested 三参
   （horizon,shape,code）测试已有别动。

【纪律】TDD；/tmp standalone 编译（Qt6 Widgets/Gui/Core/Test + 仅 panels.cpp
+ 依赖即可）。不碰 mainwindow/workflows/CMakeLists（父层接）。worktree 分支
git commit（仓库惯例 trailer）；报告。
