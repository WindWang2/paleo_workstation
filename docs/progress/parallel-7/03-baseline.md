# Baseline：方向 03（地震显示/井震联动/AI 预测运行链）R0 普查

基线 origin/master `b0520429`。四路并行只读审计（S1 数据/算法、S2 渲染、
S3 AI 双路线、S4 测试/fixture）+ 主 Agent 对旗舰发现逐行亲核。

## 1. 调用图/数据流（本方向）

```
SEG-Y ──SgyVolume.ExtractSlice──> SgySliceImage（行 0=最深样，vendor 契约）
  ├─ dock（seismicsectiondockwidget.cpp:373/574）直喂 canvas.setSectionData
  │    canvas：rebuildDisplayValues(极性→增益曲线→AGC) → rebuildImage
  │    （paintValueRegion 行直传 QImage）→ paintEvent imgDest 顶=timeToPixelY(t0)
  │    交互：hover/click sampleIdx=round((twt-t0)/dt)（采样升序假定）→ m_slice.Value
  ├─ 属性：startAttributeSlice（_slice.cpp）内核入参翻行升序、出图翻回行序
  ├─ 追踪：startHorizonTracking（_interp.cpp:85-93 traceMajorSection 翻行）
  │    种子=pick.sampleIndex（addPickFromCanvas 按画布标签 TWT 反算）
  └─ 3D：seismicslicerenderer v=1↔yTop(sample 0)——升序正确参照

AI 双路线：
  本地：ModelRegistry.scan→manifest 门控+钉哈希 → PaleoOnnxService 会话池(LRU4)
       → AiAssistWorkflow.startClassification(池线程,协作取消) → runTileInference
       （halo 规划/NaN→0/255 掩膜/softmax）→ 主线程 writeTileProducts 三件套
  远端：RemotePredictionRouter(/health→/predict,世代号+runId) → 失败可降级
       LocalPredictor(OnnxLocalPredictor,需 samples) → finishPrediction 冻结校验
  单井：WellPredictionService→WellFaciesService(jobId 缓存键=modelId+well+rows
       内容哈希,#435 已补版本)→轮询 backoff→finish 确定性词表扩展
  聊天：AiChatController(工具环≤5)→AiChatToolRunner(FIFO+世代号)→五工具
```

## 2. 已核实可观察行为（审计证据，file:line）

1. NaN 诚实传播（seismicattr.cpp:206-211 窗族；:59-63 全 NaN 道；render.cpp:256
   NaN→#303131；attr/zone NaN 透明）。
2. 像素↔道号/时间换算族自洽（nav.cpp:165-199 往返互逆；tst_seismic_section.cpp:233
   有断言）；时间切片像素↔IL 链一致（interact.cpp:178-182）。
3. 显示值 baking 顺序固定：极性→增益曲线→AGC（render.cpp:51-114）；
   hover 上报原始值（有意，头注释）。
4. 纹理缓存键含全部显示参数（canvas.cpp:113-120；#367 的 gainCurve 已由他人修复
   在位：键含 GainCurveFingerprint）；appendTimeSliceTile 复位 m_lodStride（#366
   同样已在 master 修复——两处均为 #436 前的扫尾提交，勿重做）。
5. 迟到回调守卫：世代号+请求号（dockwidget.cpp:339）、会话路径快照（track.cpp:77）、
   体路径守卫（attr.cpp:150-157）；析构 requestCancel+clear。
6. ONNX 注册表 manifest 门控+钉哈希双查（onnxpredictionservice.cpp:250-274/383-387）；
   tile 数值纪律（tileinference.cpp:52-68/100-101/231-238/269-273）。
7. 远端轮次号契约（remotepredictrouter.cpp:245-277）与传输世代号（:40-99）；
   tst_remotepredictrouter 真 loopback 验证。
8. LLM 错误分类（llmclient.cpp:433-453）与响应上限（:386-390）。

## 3. 旗舰发现：垂直剖面显示垂直镜像（主 Agent 逐行亲核证据链）

- vendor 契约：`row = sampleCount-1-s`（SgyVolume.cpp:456-462 Inline；Xline/任意线
  同），tst_seismic_core.cpp:284-288 断言「row 0 is s=63(173.0)」。
- canvas 假定升序：timeToPixelY(t0+y·dt)=行 y 像素（nav.cpp:169-172）；
  paintValueRegion 把 m_slice 行 y 写 QImage 行 y（render.cpp:248-255）；
  imgDest 顶=timeToPixelY(m_t0Ms)=最浅样位置（paint.cpp:44-47）。
- dock 直喂不翻（dockwidget.cpp:373、574-576）。
- 后果链：①标尺与数据镜像；②hover 幅值与像素一致但 TWT 是镜像时间
  （interact.cpp:201-211）；③手动拾取 sampleIndex=标签行（_interpret.cpp:100-103），
  追踪种子按 twtMs 反算同值（_track.cpp:47-49），喂给已按升序内核
  （_interp.cpp:85-93）→ 追的是镜像样点；④机器拾取 twtMs=物理时间
  （_interp.cpp:177-178）→ 绘制在 timeToPixelY(物理)=镜像位置；⑤井分层/
  合成记录按物理 TWT 绘制（paint.cpp:272/289）只在画面中点对齐；
  ⑥WellSideTraceWidget 同镜像（dock_wells.cpp:43-44/69-71）。
- 为什么至今未暴露：canvas 级测试全部用自制升序图（tst_seismic_sectionui
  makeSection :45-60），从未把 vendor ExtractSlice 真切片喂进 canvas；dock 级
  追踪测试直接按物理 TWT 喂 addPickFromCanvas（tst_seismic_interpret.cpp:580-581）
  绕过视觉层。
- 3D 渲染器是正确参照（seismicslicerenderer.cpp:31-56）。
- 修复决策：在 canvas 垂直图四个入口（setSectionData/setCompareData/
  setAttrOverlay/setZoneOverlay）统一翻行为「行 y=采样 y 升序」的画布内部
  空间；时间切片入口（行=IL 轴）不翻。理由：canvas 的标尺/hover/拾取/测试
  全部按升序写（升序才是设计契约，D4.2 注释自证）；服务/资产/3D 全说 vendor
  行序——转换点收敛在视图边界，生产喂图零改动（含白名单外的
  paleomainwindow_attach_models.cpp:575 zone 喂图点）。

## 4. 测试现状（S4 盘点摘要）

- fixture：makeSection（升序自制）、writeTestSegy（5 份拷贝各测试文件）、
  make_segy_fixture.py、tst_horizontrack makeVolume、onnxfixture（float32/
  opset13/单入单出）、mockremotepredictionservice（固定 250ms/无状态码注入）、
  LoopbackServer（Normal/Dead/Slow/Garbage 四模式）。
- 显示真值对拍已有：displayValueAt（极性/AGC/增益曲线/切片重建）；
  缺：vendor 真切片→canvas 行序对拍、卷帘 B 图参数联动、书签换线视口、
  fault stick/synthetic 渲染 smoke。
- 零测试：seismichorizoncluster TimeRaster（文件不在白名单，登记）；
  OnnxLocalPredictor 错误分支；computeSyntheticOverlays 装配层。

## 5. 性能测量方案

- 镜像修复涉及逐样点复制（O(n) 每次喂数）：现有基准
  tst_seismic_perf/tst_seismic_baseline 已有切片-渲染预算门，跑前后对照。
- 相干参数兑现后用 tst_seismicattrsvc 既有用例对照参数变化耗时。

## 6. 风险与排除

- 镜像修复触及所有 canvas 喂图测试 fixture（升序→vendor 行序）；逐个改
  生成器（按物理样 s 写入行 N-1-s），断言语义不变。
- #421 指纹行（tst_seismic_sectionui:616,667,699,798 sleep）不碰。
- 不修 #365/#366/#367/#370/#371/#372/#391/#402/#405/#412/#417/#421 任何项。
