# Goal-Loop 方向 46：交会有监督分类——样本标注→训练→推理 + 置信度伴生栅格

## 背景（实测事实，勿再勘察）

无监督聚类已落（k-means/GMM，方向 crossplot-facies）——簇只有编号
不自动赋地质相名。递延（TODOS P3）：

- `src/services/faciesclassificationservice.{h,cpp}`：已有真实置信度/
  距离向量；RemotePredictionRouter 沿 AI 方向深化——勘察推理面
  可挂点（本地核 vs router）。
- 样本→训练→推理三段全缺：训练集来自井段相解释（交会清单已有
  井段归属面）；分类模型勘察可落地族（LDA/QDA/随机森林/kNN——
  自写或 vendored，禁 GPL 传染）。
- 置信度伴生栅格：分类服务已有置信度/距离向量但未持久化——
  按编图消费需要落伴生栅格（与 Byte 分类图同网格、多一层），
  provenance 记训练集指纹。
- **本方向扩量约定**：大规模 GMM 分块 EM、SOM 自组织图**纳入
  本方向**（与监督链同立）——工区 N×k 缓冲超内存预算时分块 EM
  为必做路径；SOM 作第二无监督族与 k-means/GMM 并列可选。
  时深域交会、打印排版仍递延（需独立契约，ledger 记）。
- 交会训练标签可加地质相名（相界词表/方向 39 边界语义联动勘察——
  有字段则用，无则标签走自由词并在 provenance 标注口径）。
- ONNX Runtime 已 vendored（tst_onnx 端到端过）——若模型走 ONNX
  管线则复用，自写核则走 algorithms 层。

## 环境接线

```bash
cd /home/kevin/projects/paleo_workstation && git fetch origin
git worktree add .worktrees/xplot-sup -b goal/xplot-sup-20261004 origin/master
cd .worktrees/xplot-sup
ln -s /home/kevin/projects/paleo_workstation/vendor/superbuild/prefix vendor/superbuild/prefix
ln -s /home/kevin/projects/paleo_workstation/vendor/onnxruntime vendor/onnxruntime
[ -d /home/kevin/projects/paleo_workstation/vendor/prefix ] && \
  ln -s /home/kevin/projects/paleo_workstation/vendor/prefix vendor/prefix
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DQGIS_PREFIX=/home/kevin/projects/paleo_workstation/vendor/superbuild/prefix
cmake --build build -j8
```

## 目标形态（建议按序）

1. **样本标注面**：井段相解释→交会特征空间训练集（勘察交会清单
   井段归属现有数据结构）；标注缺类/样本失衡如实提示。
2. **训练**：监督族至少两族落地（勘察 LDA/QDA/kNN/决策树/
   随机森林可选范围，自写或 vendored，禁 GPL 传染）+ ONNX
   管线勘察接入；交叉验证出混淆矩阵/每类精度，不写「准确率」
   模糊词。
   **分块 EM**：N×k 缓冲超内存预算时 GMM 自动走分块路径——
   分块与全量结果一致性断言（容差内）。
   **SOM**：自组织图族补齐（网格尺寸/学习率/邻域半径参数化），
   输出与现有簇编号同消费面。
3. **推理**：训练模型对交会样本/栅格逐点推理——输出类标签 +
   置信度/距离向量（faciesclassificationservice 既有字段消费）。
4. **置信度伴生栅格**：分类图同网格多一层置信度持久化——
   catalog DERIVED 版本，provenance 含训练集指纹+模型参数。
5. **UI**：交会页训练→推理→成图走通；低置信度区在分类图上
   可辨（透明度/掩膜其一，不掩盖置信信息）。

## 通用纪律（方向内全程有效）

- **分层**：`src/` 新文件头三行 `// 层：<词表>`；训练/推理核归
  algorithms/services，标注面归 domain，交互归视图层。
- **诚实面**：混淆矩阵/置信度如实呈现；样本不足/单类退化如实
  拒绝训练报因；伴生栅格 provenance 完整。
- **资源**：构建/测试一律 `-j8`。
- **UI**：对照 `DESIGN.md`；i18n 两门；与既有分类图样式一致。
- **数值正确性**：合成可分数据上训练收敛、推理精度达阈；
  不可分数据如实出低置信度而非强行分。
- **冲突**：faciesclassificationservice/交会页热区注明。
- **ledger**：`.goal-loop-ledger-xplot-sup.md`。
- **多轮 review（硬要求）**：每批 → 测试全绿 → diff 自审（分层/训练正确/
  置信诚实/provenance/i18n 五维）→ 修复 → 再 review，
  **至少两轮零 High/Medium**；Low 记 PR body。
- **提交**：原子提交，中文 conventional 前缀。

## Oracle 验收（逐条需验证证据）

1. 训练闭环：合成两相样本 → 模型训练出混淆矩阵，类精度 ≥ 阈
   （可分类目），退化单类样本如实拒绝。
2. 推理一致：同一模型对训练集推理的标签/置信度与训练期一致
   （确定性断言）。
3. 伴生栅格：分类图 + 置信度层同网格落成 DERIVED 版本，
   provenance 含训练集指纹。
4. 低置信可辨：两类交叠区推理出低置信度，图面可辨（像素级
   证据或掩膜计数）。
5. 分块 EM：超限夹具下分块路径与全量结果容差内一致；
   峰值 RSS 有界（不随 N 线性膨胀）。
6. SOM：合成簇数据上拓扑保持性有断言（相邻原型向量距小于
   非相邻），输出入同一分类消费面。
7. 空态：零标注/单类标注 → 训练入口禁用带原因文案。
