# seismichorizoncluster — 地震窗反射特征聚类（实战系补账立账）

本文为补账文档：`src/services/seismichorizoncluster.{h,cpp}` 由 40398d20
（新建 196 行）与 f0e7b10d（重写 +188，「直接按原始层位提取地震窗并聚类」）
落地，此前零 progress 文档。行号为 origin/master `f31ee461` 口径。

## What（交付面）

| 面 | 内容 |
|----|------|
| 入口 | `clusterSeismicHorizon(seismicPath, horizonPath, cacheDir, faciesCodes, progress, cancel, source)`（seismichorizoncluster.h:21-26）。线程契约：纯函数计算，调用方放任务线程（头注释 seismichorizoncluster.h:20） |
| 消费者 | 唯一——`MappingWorkbench` Mock 相预测链（src/workflow/mappingworkbench.cpp:748）。参数 `horizon_source_format`（`smi_xyz_inline_crossline` / `time_raster`）随预测参数留痕（mappingworkbench.cpp:676） |
| 输出 | `SeismicClusterResult`：cells（行主序相码栅格，nodata=-9999）+ extent + columns/rows + validCells（seismichorizoncluster.h:10-16） |
| 提交 | 40398d20 新建；f0e7b10d 重写（Scatter 原始点直取 + 测区一致性校验）；e9d17e80 的方言修复是其前置（号域退化会让 :82-84 前置校验失败） |

## 口径（语义决策）

1. **双源输入，Scatter 优先**。SMI 散点（x y TWT il xl）直接用原始点提取，
   输出保持原测网分辨率（头注释 seismichorizoncluster.h:19）；时间栅格
   （TimeRaster）只是已有工程的兼容输入，GDAL 北向上 TWT 栅格降采样至
   ≤64×64（seismichorizoncluster.cpp:50-51）。工作台选源逻辑：先找
   catalog 里的原始层位版本（RAW/snapshot），找不到才退层位栅格
   （mappingworkbench.cpp:597-663）。
2. **取窗契约（f0e7b10d 修复点）**：以层位 TWT 为中心 **±12ms** 开窗
   （seismichorizoncluster.cpp:221-222），样本号 `ceil(start)..floor(end)`
   取整（:225）；窗越出道长整点跳过不外推（:223-224）。
3. **号域语义（Scatter）**：层位头 P1/P2/P3 三角点须与地震测网格网坐标
   一致 ≤1.0m——同号域不同测区拒收（:146-157 注释「号域相同仍须核对
   坐标，避免把另一测区的同号道当本测区」）；数据行坐标与层位头网格
   映射偏差 >0.001 逐点拒收（:184）；重复 Inline/Crossline 位置报错
   （:186-189）。
4. **特征 3 维**：窗内均值 / RMS / 过零率（:227-237），z-score 标准化
   防振幅量纲支配（:247-254）。
5. **确定性 k-means**：最远点初始化（首样本起步，:258-270）+ 40 轮上限
   （:272）+ 收敛即停（:291）。低层函数按中心 RMS 排序产生稳定聚类编号。
   2026-10-18 更新：工作台不再将该序号直接当作相解释，接入测井多数票标定，
   详见下节。
6. **诚实边界**：有效交集不足（features < 类数）报错不硬聚（:243-246）；
   类别数 1–64（:28-31）；解释类别映射为 Mock（头注释 :18）——非确定性
   岩相解释，是「当前层位 TWT 窗内的真实反射特征」聚类。
7. **取消/进度**：全路径协作取消；进度约定——SEG-Y 解码 0-20、层位读
   20-30、逐线取窗 30-75、k-means 75-99、100 收口（:74-79, :169, :240, :290）。

## 测井多数票标定（2026-10-18）

`seismicclusterlabels` 保留聚类分区，按其内的测井预测点数决定相，不按振幅
排名指定地质相。每井一票，多簇允许归为同一相；票数相同取较小编码，无票簇
按种子 42 在有效测井结果出现的类别内选择。NoData 不参与投票或填充。
没有有效测井相时失败，不从整个分类库制造随机结果。

`seismicwellcalibration` 在任务线程打开已保存的不可变点文件，先验证 SHA-256，
将井点转换到输出网格 CRS。按目录登记顺序从新到旧逐井去重，最新无数据/未预测
记录也屏蔽旧记录；未保存编辑不参与。历史编码根据冻结的完整三级路径与当前
分类匹配，不能把相同数字直接解释成当前的新含义。缺少对应路径时要求先统一分类。

产物方法为 `mock-horizon-window-kmeans-well-majority-v2`，保留 Mock 及 ±12ms
标记，记录 `cluster_facies_mapping`（各簇类别、票数与规则）、固定种子、支撑井，
并将实际使用的测井结果版本加入来源谱系。测井文件缺失、摘要改变或无法定位时
拒绝发布；取消或切换工程不发布旧任务结果。

### 本次验证

- 构建 `tst_seismichorizoncluster`、`tst_mappingworkbench` 通过（`-j8`）。
- 地震聚类完整 CTest 通过，覆盖原取窗/NoData 契约和新增多数票、平票、无井兜底、
  重复井、边界、取消及候选范围；三项 layering CTest 通过。
- 编图六个定向用例通过：缺少测井结果拒绝运行、跨 CRS 与冻结分类路径匹配、
  真实 SEG-Y/SMI 预测及多批井点去重/谱系、原有地震转面编辑链、取消、全无数据。
- UI 大小、文案、界面不变量、QGIS 散布和文档漂移检查通过。

## 证据

- 与交会分类（`faciesclassificationservice` + `algorithms/cluster`）的关系
  定案：**独立链，不复用同一分类核**。rg 证据——`seismichorizoncluster.cpp`
  与 `mappingworkbench.cpp` 均无 `algorithms/cluster` include（零命中）；
  交会侧 `FaciesClassificationService::classify` 走 `cluster::Options`
  （KMeans/GMM、seed=42、maxIterations=100，src/algorithms/cluster/cluster.h:33-40；
  src/services/faciesclassificationservice.h:13-26），本服务为内联确定性
  k-means（无 seed、最远点 init、40 轮）。差异依据在头注释「确定性聚类」
  （seismichorizoncluster.h:18）——低层聚类不使用随机源；新增无井区域的相解释
  兜底使用固定种子，仍可复现。交会分类走可复现种子随机链。
- 任务书所述「projectclassifier」经核实为 **导入路径分类器**
  （src/domain/projectclassifier.h:4-6，「规则移植自 paleo-merged-main
  libs/ingest，project_area 导入分类」，PROJECT_AREA_PLAN.md §3）——与聚类
  完全正交，非交会分类核。命名易混，后续 prompt 勿再互指。
- 取窗契约测试锚：tests/tst_seismichorizoncluster.cpp（本方向补，见下）。

## 对账与递延

- 与方向 23/43（seismic-attributes / seismic-inversion）：**正交**——聚类
  读原始道取窗，不消费属性体/反演体产物；共享 `SegyReader` 的 dt/t0 语义
  （sectiontrace.h:88 同口径处理记录延迟）。
- 与交会分类的计算核分叉（两套 k-means：标准化统计量/初始化/轮数各一套）
  仍为后续收敛候选。本次在独立聚类分区上按测井结果标定地质相，
  不再把两套算法的聚类序号当作相语义，也不改写交会分类核。
- TimeRaster 路径降采样 ≤64×64 为兼容语义，分辨率损失如实（不插值回原
  网格）；`解释类别映射为 Mock` 的正式相解释链（AI 预测服务）不本文范围。
