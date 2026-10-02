# AI 辅助解释（ai-geological-assist）— 交付记录与性能档案

- 分支：`goal/ai-geological-assist-20261001`（worktree `.worktrees/ai-geological-assist`）
- 范围：`src/ai/` 从骨架推理服务推进到地质工作流真实辅助能力——会话池/指纹/
  warmup、tile 批量推理（进度/取消/halo 缝合）、置信度产品、层位追踪建议
  （解释员裁决语义）、模型注册表、远端路由降级。

## 管线总览

```
PaleoOnnxService（ORT 会话池：LRU≤4 + SHA256 指纹 + 输入签名 + warmup 计时
  + 失败分类 NotFound/BadFormat/UnsupportedIr/CreateFailed）
  ├─ runTileInference（ai/tileinference）          ← tile 批量分类
  │    plan（内区恰好覆盖一次）→ fetch（io 解耦回调）→ ORT [1,C,H,W] logits
  │    → softmaxGrid（argmax/最大概率/归一化熵置信度，NaN→255）→ 内区缝合
  │    → AiAssistWorkflow.classifyTiles：三产品（相/低置信掩膜/置信度）
  │       落 artifacts/derived + DERIVED + 只读 + 声明 03_Predict
  ├─ suggestHorizonTracking（ai/horizonsuggest）    ← 层位追踪建议
  │    种子点确定性扩张 → trace scorer 逐道峰值拾取 + Bernoulli 熵置信度
  │    → AiAssistWorkflow：建议对象纯内存待裁决；accept/veto 不写 catalog；
  │       commitAccepted 才落 JSON 点集（无诚实地图几何不声明图层）
  └─ RemotePredictionRouter                          ← 远端路由（范围6）
       /health 健康检查（超时判死）→ /predict；失败/断线 → 本地 ORT 跑同网格
       （请求须携 samples，没数据不造假）；无默认外网端点，测试全 loopback
ModelRegistry（ai/modelregistry）：models/manifest.json 扫描；缺 manifest=
  「未装模型」降级；钉哈希不符/文件缺失/坏条目逐条如实分类（appcontext 扫描
  进消息日志；PredictPage 无 onnx 条目时写明「未装模型」）
OnnxFixtureWriter（ai/onnxfixture）：手写 protobuf 字节的微型模型生成器
  （add40 标量 / seg NCHW logits / scorer trace 窗 sigmoid），零网络零
  python-onnx 依赖，同参数字节确定性。
```

## 夹具模型语义（数值可手算）

| 模型 | 图 | 输入 → 输出 | 解析语义 |
|------|----|------------|----------|
| `add40` | Add | `x:float32[1] → y[1]` | y = x+40（2.0→42.0，toy 兼容） |
| `seg3` | Mul+Add | `x:float32[1,1,H,W] → y[1,3,H,W]`（H/W 动态） | logits 类 c：gains{1,−1,0}·x + biases{0,0,0.1}；argmax/熵解析可算（softmax 熵对拍独立双精度实现） |
| `scorer` | Mul+Add+Sigmoid | `t:float32[1,1,T] → p[1,1,T]`（T 动态） | p = sigmoid(8t)：尖峰≈0.9997，背景 0.5 |

protobuf 字段号实测钉死两处：`TensorShapeProto.Dimension.dim_value=字段1`
（python onnx 产物对照确认；写字段2 会被 protobuf 按线型不符当未知域丢弃，
ORT 报 −1 动态维）；`TensorProto` dims packed、float 走 raw_data(9)。

## 性能档案（比率门，非墙钟）

测试：`tst_aiassist_perf`（`add_paleo_test … LIBS paleo_ai paleo_sbm`）。
断言口径：**批式单 tile 均耗 ≤ 同构单发（推理+softmax）均耗 ×4**（防逐 tile
泄漏式回退）+ 不变量 `inferenceMs ≤ elapsedMs`；真机数据 env 门控
（`PALEO_SEISMIC_REAL_SGY` / `PALEO_REAL_PROJECT_AREA`），输出
`BASELINE <metric> = <value>` 行。

### 本机实测（2026-10-01，CachyOS，-O2 RelWithDebInfo，ORT 1.30 vendored）

| 场景 | 网格 | tile | 单发均耗 (ms/tile) | 批式均耗 (ms/tile) | 比率 |
|------|------|------|--------------------|--------------------|------|
| fixture 合成（seg3, 64×64, halo 8） | 411×641 | 77 | 0.573 | 0.649 | 1.13 |
| perf_big.sgy 220MiB 实测（inline 中剖） | 1024×221 | 64 | 0.366 | 0.512 | 1.40 |

- 实测吞吐：**≈6.9M 样本/秒**（220MiB 体 inline 切片全网格，含 softmax 与
  缝合；`BASELINE aiassist_tile_throughput_samples_per_sec = 6904340`）。
- 966MiB 工区体（`200P_seismic.sgy`）：**本机不在场**——门控 harness 已就绪
  （同一 env 变量族，tst_seismic_realarea 先例），有数据时直接复测誊入本表。
- 微型 Mul/Add 模型的单 tile 推理亚毫秒（整数 ms 计时全 0），故均耗一律
  nsec 口径；真实模型（卷积/Transformer）接入后量级会变，比率门口径不变。

## 建议语义红线（Oracle 4）

- 追踪建议：`suggestTracking` 全程纯内存；`acceptSuggestion/vetoSuggestion`
  零 catalog 变更（测试对 `catalog.json` 逐字节比对）；`commitAccepted` 显式
  提交才登记 DERIVED 版本（JSON 点集，测网道空间，无诚实地图几何就不声明
  图层）。种子是解释员锚点：默认接受、不可否决。
- 不做训练、不下载权重；AI 输出永远是「建议/派生产品」，不自动写解释结果。

## 降级语义（Oracle 5）

| 状态 | 行为 |
|------|------|
| models/ 无 manifest.json | 未装模型——PredictPage 写明「未装模型」，onnx:* 不出现在算法表 |
| manifest 坏 JSON | 消息日志如实报错（Critical），不轰炸 |
| 条目缺文件 / 钉哈希不符 | 逐条 Warning（含路径/双哈希），该模型不上算法表 |
| ORT 运行库缺失 | 构建 PALEO_HAVE_ORT=FALSE，ai/ 空壳，消费方链接名不变 |
| 远端路由 | 健康检查/预测超时判死 → 本地 ORT 降级；无 samples 数据 → 如实失败（「不造假」入错误链） |

## 测试面（新增 6 个套件，43 用例）

| 套件 | 用例数 | 覆盖 |
|------|--------|------|
| tst_onnxsessions | 16 | fixture 确定性/解析值/池命中/LRU 逐出/指纹翻转/坏字节/IR9999/并发 4×20 |
| tst_tileinference | 11 | plan 恰好覆盖一次/softmax 独立对拍/缝合只写内区/端到端/进度单调/取消（fd/RSS/池无泄漏）/诚实失败 |
| tst_modelregistry | 11 | 未装模型降级/有效清单/缺文件/钉哈希/坏 JSON/缺必填/动态维 |
| tst_aiassistworkflow | 8 | 三产品像素级断言（低置信/NaN 全处理）/只读纪律/裁决零变更/提交落盘/异步完成+取消 |
| tst_remotepredictrouter | 9 | loopback 四模式（正常/秒断/挂起/垃圾）/降级/无数据不造假/取消 |
| tst_aiassist_perf | 2 | 比率门 + env 门控真机吞吐 |
