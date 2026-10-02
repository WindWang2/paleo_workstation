# 进度档案 — 层位自动追踪（horizon-autotrack）

> goal loop `goal/horizon-autotrack-20261002` 交付记录。
> 数值核 `src/algorithms/horizontrack.{h,cpp}`（paleo::hztrack）；服务编排
> `SeismicTaskService`（trackHorizon 下沉 + 多种子 + 异步取消）；上图闭环经
> 既有 horizonbinner → GeoTIFF → layermanifest 管线。

## 交付一览

| 提交 | 内容 |
|------|------|
| goal/horizon-autotrack-20261002 | 追踪核（互相关主干+相干门+置信度语义+多种子合并+3D 前沿扫掠+协作取消）；trackHorizon 下沉重构 + trackHorizonMultiSeeds + startHorizonTracking（PaleoTask 可取消）；追踪产物 GeoTIFF+LayerDeclaration 上图；dock 多种子合并替换一步 undo；面板覆盖率/停因 QC 行；tst_horizontrack（14 例）/tst_seismic_interpret 扩 5 例/tst_horizontrackperf |

## 语义决策

1. **追踪主干** = 固定种子模板互相关 + 围绕前一道拾取位置的滑窗搜索
   （±maxSearchSamples = 隐式倾角引导/最大跳跃约束）。固定模板抗漂移：
   归一化相关对位置不敏感，候选窗滑动即可跟随大倾角，同相轴总漂移只要
   逐道步长 ≤ 搜索半径即可追上。
2. **置信度语义（钉死）**：`conf = corr × coherence`。corr = 接受位置
   Pearson 相关（负钳 0）；coherence = 同位置相干门数组值（无门=1）。
   降权信号各自独立单调：波形劣化 → corr 降，横向破碎 → coherence 降。
   手动拾取 = 1.0（D4.10 语义）。相干门 < 阈值 = 硬停（不降权放行）；
   门 NaN = 弃权放行（相干体边界带是缺失不是不合格）。
3. **断层停止双判据**：① 相关丢失（位移越搜索半径/波形破碎 → corr 崩）；
   ② 相干门（门数组直接消费 `seisattr::semblanceCoherence` 输出——复用
   属性核，不重写相干）。
4. **多种子合并**：逐种子独立追踪后按道归并，同道取高置信，并列取先入
   （稳定可重现）。dock 合并替换 = 单个 ReplacePicksCommand（一步 undo）；
   手动列（conf==1）不被机器覆盖。
5. **3D 扩散（最小可用）**：前沿扫掠——种子 IL 剖面 2D 追踪成前沿，逐 IL
   向两侧外推（模板=前沿道窗，逐道跳跃 ≤ maxSearch），死列不复生（不跨
   空外推），限步长 maxInlineStep。服务层 3D 暴露与显式斜率扫描递延
   （TODOS）。
6. **诚实失败**：追踪失败区如实留空（无插值填充）；覆盖率/均值置信度/
   双侧停因入面板 QC 行。
7. **修复 D4.2 垂直镜像 bug**：`SgySliceImage` 行序 = `sampleCount-1-s`
   （行 0 = 时间底，引擎契约），旧 trackHorizon 把采样序种子当行序用——
   真切片上追踪时间轴镜像、种子对不上事件（被 `>=1` 弱断言掩盖；合成夹具
   恰不翻转所以服务测试从未暴露）。修复：服务桥接行→采样翻转（单点），
   夹具改为引擎行序同构（回归防线）。

## 真机实测（966MB 生产形状：411 IL × 641 XL × 901 样 @2ms = 965.8 MiB）

测试 `tst_horizontrackperf`（BASELINE 行，RelWithDebInfo，ccache 命中）：

| 指标 | 值 |
|------|-----|
| 体加载（QuickOpen 探测+索引） | 389 ms |
| 中段 IL 切片提取（641 道 × 901 样） | 11.0 ms |
| 剖面满宽度追踪（单种子双向 641 道） | 2.0 ms |
| **追踪速率** | **≈ 320,000 拾取点/秒** |
| 覆盖 | 641/641 道（全程） |

对比读路径预算：剖面逐点取数预算 64ms/切片（docs/progress/seismic.md），
追踪核 2ms/剖面——**追踪计算成本 ≈ 切片读取的 1/5，逐点取数预算内富余
30 倍以上**。比率门 `trackingCostRatioGate`（追踪 ≤ 8× 切片提取）常跑防
数量级回归。

注：966MB 夹具为 ramp 波形（道间相关恒 1），度量的是满候选搜索的计算
吞吐（每道 FLOPs 与事件数据相同：25 候选 × 24 样窗 × 双向）。事件数据的
数值正确性由 tst_horizontrack 合成断言覆盖。

## 已知边界 / 递延

- 3D 扩散仅在数值核 + 合成断言面（propagateVolume），服务/UI 暴露递延
  （TODOS：3D 面扩散任务 + 画布层位面叠加）。
- 追踪器无「事件唯一性」校验：事件丢失后若搜索半径内存在其他强相关
  波形（真实数据的多事件近距平行），追踪可滑落到相邻同相轴——相干门
  与阈值是现行缓解；显式斜率扫描/振幅极值复核递延。
- 追踪产物上图是 IL/XL 索引局部测网（像元 = 测线步长）；接真工区 XY
  仿射需 SurveyGridGeometry 冻结（与 SMI 导入同语义，TODOS）。
