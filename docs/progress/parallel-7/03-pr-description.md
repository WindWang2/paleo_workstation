# refactor(03): 地震显示、井震联动与现有 AI 预测运行链稳定化

## 范围与所有权

方向 03（七任务矩阵）：只迭代已有功能，无新业务面。分支
`goal7/seismic-ai-prediction-20261014`，基线 origin/master `b0520429`。
24 个生产/测试文件全部在本方向白名单内（src/ui/seismicsection/、src/ai/、
src/services/seismictaskservice*、src/workflow/aiassistworkflow|aichat*、
tests/），`git diff origin/master...HEAD --name-only` 逐文件核对通过。

## 冲突排除

- **Open PR #437**（68 文件保护区）：零交集（本分支不触碰
  mappingworkbench*/wellpredictionreview/predictionevidence/selectioncontext/
  wellmaplink 等）。
- **Open Issues #352–#421**：全部进动态排除表，未修任何已立案项（#365
  interact 点击路径、#366 LOD、#367 增益缓存键、#370 最近井、#371 反演
  守卫、#372 空轴、#391 缓存键、#402 ONNX 销毁竞态、#405 tst_aichattoolloop、
  #412 paint amplitude/seismic3dtf、#417 semblance 维度序、#421 sleep 杠杆
  的涉事文件/函数均未修改）。#435/#436 已交付不重做。
- 发现但属他人所有权的登记项见 ledger「留给 Issue Agent」表（本 PR 不动）。

## 交付内容（7 提交）

1. **`161b545b` 垂直剖面行序镜像修复（方向 A 旗舰）**：vendor ExtractSlice
   契约行 0=最深样，canvas 全链（标尺/hover/拾取/井分层/合成记录）按采样
   升序设计——旧实现直喂导致显示↔标尺镜像、hover TWT 是镜像时间、追踪
   种子落在镜像样点。视图边界统一翻转（ToAscendingSampleRows），
   生产喂数点零改动。**红证据**：displayValueAt(2,5)=316.5（样 58 镜像位
   真值）≠ 303.25（样 5 真值）；修后绿。fixture 改按 vendor 行序生成。
2. **`c8bbd885` AI 状态机诚实性**：tile 推理成功但产品落盘失败 →
   failPublication 翻转任务终态（任务页/工具回灌/dock 三消费面看到失败，
   不再冒充「已登记 DERIVED」）；LLM 错误路径按尾部 assistant 补齐
   role=tool 应答（修前下轮请求违反 OpenAI 协议 400、会话每轮复现）。
3. **`0b2c1545` 服务层**：readTraceHeader 首道偏移复用 segyio segy_trace0
   （扩展文本头文件读到错误道的头——红证据 5088 vs 11488）；剖面相干
   兑现面板参数（法向半窗/加权变体+道距/沿窗越界拒绝——SATR 登记值自此
   与实算一致）。
4. **`6b122376` UI 交互**：跨线书签视口重放（fitToWindow 复位竞争）；
   卷帘 B 图随增益/对比度/极性重烘；拾取面板「定位」实装视口滚动
   （旧为 no-op）；井标题后缀幂等。
5. **`004891d4` AI 域守卫**：trace scorer 输出越出 [0,1]（logits 型）整跑
   拒绝（修前 score>1、置信恒 1）；OnnxLocalPredictor 四错误分支直测。
6. **review 修复提交**：S5 独立审查 2 P1（zone 叠加镜像回归、定位跨模式
   丢线号）+ 3 P2 修复闭环，附两个方向性钉子。

## 测试与验证

- 新增/改写测试 12 个用例（跨 6 个测试文件），关键缺陷均先红后绿（镜像、
  trace0、相干参数、tile 发布失败、协议完整性、logit 拒绝）。
- 触及套件 13/13 通过；全量 `ctest -LE perf -j4` 结果见 ledger（master
  既有红项单独归因，不掩盖）。
- `cmake --build build --target paleo -j4` 退出码 0；
  `python3 tools/check_layering.py --strict` 通过；`git diff --check` 通过。
- 独立 Review（S5，未参与实现）：2 P1 + 3 P2 已修并复测，2 P2 登记为
  已知限制（卷帘重烘节流、井标题幂等守卫为防御性）。

## 已知限制 / 未处理

- 卷帘 B 图重烘无节流（A 侧每 tick 全图烘焙为既有行为，B 侧翻倍——
  递延优化）。
- AGC/增益曲线不作用于卷帘 B 图（既有口径，注释写明）。
- 井标题幂等守卫在当前调用序下不可达（防御性保留）。
- 未处理的 open issues：全部 70 项（所有权排除）；关联新发现见
  ledger「留给 Issue Agent」表。

## 下一阶段建议

- B 图重烘合并/延迟一拍（参数拖动期 O(W×H) 双份烘焙）。
- seismichorizoncluster TimeRaster 路径零测试 + clusterSeismicHorizon 吞
  readInline 失败（文件不在本方向白名单，登记给 Issue Agent）。
- LoopbackServer 无状态码注入（认证失败场景不可测，mock 能力缺口）。
