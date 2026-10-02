# Goal Loop — goal/ai-geological-assist：AI 辅助解释生产化（新功能）

你接手一个自治迭代循环。方向：**把 `src/ai/` 从骨架服务推进到地质工作流里的
真实辅助能力**——ONNX 层位/相自动追踪、置信度图、批量推理编排、模型注册表。

## 背景事实

- `src/ai/`（层：功能）现有 `onnxpredictionservice.*` + `remotepredictionservice.*`；
  `PALEO_HAVE_ORT` 编译定义；ONNX Runtime 1.30.0 vendored（`vendor/onnxruntime`，
  官方 manylinux_2_28 二进制 SHA256 钉死，`tst_onnx` 测试在）。
- 数据通路：地震体窗/切片访问 API 在 src/io+services 侧已验证（体窗 46ms 级）；
  井曲线/层位/分层域对象在 src/domain+metadata 侧。
- ai/ 层禁令：无 QtWidgets、无 ui/ include——推理输出经信号/数据对象回传，
  上图与交互走 ui 层既有挂点。
- 模型资产策略：不下载真实大模型（外网 TLS 受限先例）——用程序化生成的
  微型 .onnx fixture（可手写 protobuf 字节或用 python onnx 包生成小图），
  验证管线正确性而非真实精度；真实模型接入走「注册表+路径约定」接口层。

## 范围（按依赖序，做到 Oracle 为止）

1. **推理会话管理**：ORT session 池（env/interThreads 配置钉住）、懒加载+warmup
   计时、模型指纹（SHA256+输入签名）校验、失败分类（格式不符/输入维度/
   版本不兼容→诚实错误链）。
2. **批量/瓦片推理**：体窗/切片按 tile 网格分批推理，进度+可取消（复用
   `paleotaskservice` 异步模式），tile 重叠-缝合策略（halo 接缝消隐），
   结果回写为属性体/栅格层（catalog 产物登记）。
3. **置信度产品**：输出除预测值外另产置信度图（softmax 熵或概率阈值），
   UI 叠加降透明/掩膜显示低置信区。
4. **地质工作流挂点**（择一落地，可扩展）：
   - 层位自动追踪种子点→局部追踪建议（输出为可确认/否决的建议对象，
     不自动写——解释员裁决语义）；
   - 相分类切片级辅助（属性体→相概率图→叠加显示）。
5. **模型注册表**：`models/` 目录约定 + manifest JSON（名称/版本/输入形状/
   输出语义/适用数据类型），启动扫描、缺失如实降级（功能面板显示「未装模型」
   而非报错）。
6. **远端推理服务**（remotepredictionservice）：若已有 HTTP 骨架——补健康检查/
   超时/断线降级到本地 ORT 的路由策略，全部可测试（loopback fixture server）。

## Oracle

1. 微型 .onnx fixture 生成器入库（`tools/` 或测试资产生成代码），无网络依赖。
2. 推理闭环：tile 输入→ORT→概率图→上图叠加，fixture 上断言输出形状/数值域正确。
3. 批量任务：进度单调、可取消（取消后无泄漏：RSS/句柄断言）、断线/坏模型诚实失败。
4. 置信度掩膜上图（断言低置信区像素被处理），建议对象不自动写（测试断言
   接受前 catalog 无变更）。
5. 模型注册表扫描+manifest 校验测试全绿；缺模型时 UI/服务如实降级。
6. 性能档案：fixture 体 + 966MB 实测 tile 吞吐、单次延迟入
   `docs/progress/ai-assist.md`；比率门非墙钟。
7. ctest 全绿；`check_layering --strict` 绿；ledger；push + `gh pr create`。

## 勘察指引

- `src/ai/onnxpredictionservice.cpp` 现状接口形状、`PALEO_HAVE_ORT` 编译条件块、
  `tst_onnx.cpp` fixture 风格。ORT C++ API：`vendor/onnxruntime/include/`。
- 任务/取消范式：`src/services/paleotaskservice.*`；上图路径参考
  seismicmapping/图层角色登记。
- 微模型生成：onnx protobuf 手写最小图（单个 Conv/Relu/Softmax 足够验证管线）；
  python onnx 若本机已装可直接脚本生成（`pip list` 查），否则手写字节。
- tile 缝合接缝处理先例：图像处理 overlap-add；坐标系沿用瓦片约定（x=xline
  偏移、行 0=最大 inline 显示向）。

## 禁区

- 不发明真实训练管线（不做训练/不下载权重）；管线正确性用 fixture 模型验证。
- 不动 io/seismic 读路径内部。
- AI 结果永远默认「建议」语义——自动写解释结果是产品级决策，不在本方向擅开。
- 远端服务不给默认外网端点（测试用 loopback）。

## 迭代协议

- 轮0-1：ORT API 面勘察 + 微模型 fixture 生成打通（最小推理 hello world）。
- 中段：会话管理→tile 推理→置信度→工作流挂点→注册表→远端降级，每步实测+测试。
- 完成定义 = Oracle 7 条全绿，ledger 轮次齐全。
