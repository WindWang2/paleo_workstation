# goal/seismic-inversion — 地震反演（子波库 / 低频模型 / 带限与稀疏脉冲）

分支 `goal/seismic-inversion-20261003`，基线 `4741115`（origin/master，PR #113）。
目标：经典确定性叠后反演链四件——子波提取与库、低频阻抗模型、带限反演（道
积分）、稀疏脉冲反演——落到 `paleo::inversion` 纯数值核 + `InversionWorkflow`
三段式编排 + DERIVED 资产 + 地震剖面 dock 挂点。只做叠后确定性反演，不引外部
库（FFT 自研共享），递延见文末。

## 语义决策

**域**：反演是时间域运算。井阻抗曲线 MD→TWT 走 `TimeDepthModel`（垂直井语义
——方向 19 井轨迹未落地，如实递延）。子波/反射系数/阻抗全部 TWT ms。

**子波提取**：Wiener 最小二乘——模型 s(t)=Σ rₖ·w(t−tₖ)，对 w 的正则方程是
反射系数尖峰 train 的自相关 Toeplitz 阵 × w = 道与尖峰 train 的互相关；高斯
消元 + 对角加载（1e-6·自相关[0]）防病态。输出归一化（max|x|=1）+ 拟合相关
QC + 主频估计（FFT 零填充振幅谱峰）。相位旋转工具 `rotateWaveletPhase`
（Hilbert 单边谱 e^{iθ}）供混合相位对拍。拒绝 project.sqlite 新表持久化——
DERIVED 资产 `type="wavelet"`（wavelet.json）免费拿 sha256/父谱系/只读位，
同 fault_surface JSON 资产先例。

**低频模型**：频段 0–lowCutHz（缺省 8Hz），垂直平滑窗 = 1000/lowCut ms 移动
平均（首零点恰在 lowCut）。层模式：层内井阻抗均值沿层横向 IDW（自研薄 IDW，
井距 0 精确命中；不复用 singlefactor PreparedInput/autos 面——那是单因子参数
语义，会把反演测试耦合到因子夹具）；层位缺格回退层位平均 TWT；无层位/全缺
→ 全局趋势回退（井曲线低通后横向 IDW）。井层无控 NaN 诚实传播。紧凑模型
（每格每层阻抗 + 边界网格 + 预计算边界均值）+ `lowFreqTraceAt` 逐格合成——
道并行只读安全，且免逐道 O(格数²) 重扫（自审拦下的性能炸弹）。

**带限反演（道积分）**：可选子波频域 water-level 反褶积（|W|² 分母下限 1%
峰值能量，带外不放大）→ 递归积分 Z(i+1)=Z(i)(1+r)/(1−r)（|r|≤0.45 护栏）→
相对阻抗**同 lowCut 高通**（减同核移动平均——不减会把相对项的低频漂移与低频
模型双计）后与低频模型原幅度叠加（反褶积输出即反射系数量纲，幅度标定由子波
对反射系数拟合承担）。低频贡献 = σ²_low/(σ²_low+σ²_relHP)。

**轮内语义修正（探针实测定位，非先验）**：Ricker 零面积（谱在 DC=0）→ 带限
反射系数积分是回零振荡而非台阶；地震带不含 <5Hz 分量时 600ms 级层水平本质
不可恢复——**绝对层水平必须由低频模型承载**（这正是低频模型的物理依据）。
Oracle 语义据此定：合并层均值误差 + 相对项与高通真阻抗的相关（零相位实测
0.9423 / 混合 30° 0.8141 / 裸道 0.7628——相位失配如实降级，不强求校正）。

**稀疏脉冲**：自研 FISTA + 软阈值（min 0.5||Wr−s||²+λ||r||₁），褶积算子/转置
手写 O(n·wl)，Lipschitz 幂迭代 ×1.05 裕度，λ 缺省 0.05·max|Aᵀs|（数据驱动，
与道幅度无关）；停机 = 解相对变化 <1e-4 或迭代上限（不谎报收敛）；阻抗由稀疏
r 递推、种子取低频首样；残差能量比入 QC。

**体格式 IIMP1**：magic + u32 头长 + JSON（轴域/测网/方法参数/频段口径）+
float32 体，布局 `((il·nXl)+xl)·nS+s`，NaN=缺失，无时间戳字节稳定（同输入
两次 compute 字节一致——并行确定性断言）。PPROP1 同模式不复用类型
（PropertyVolume 是深度域地层网格）。

**测网几何**：采样 ≤48 个道头（`readTraceHeader` 的 Source X/Y + IL/XL）最小
二乘自拟合仿射。不用 `SegyGeometry` 角点——cornerSlot 按中点比较分槽，奇数
线数测网的中线道会覆盖真角点（实测 3×3 的 slot3=(inlMax, xlineMid)）；
segyreader 属在飞方向 21 文件面，不改它，绕开。

**诚实口径**：产物 extra 必带 lowCutHz 与 frequencyBand（「低频模型 0–XHz +
地震带限」）；UI 不标「高分辨率」；无子波不留暗兜底（显式要求先提取）；取消/
失败不 stage、不落 DERIVED；道缺失 >30% 整道拒绝；带限/稀疏失败道计入
failedTraces 如实显示。

## 接口

```cpp
// src/algorithms/dsp/fft.h（自 seismicattr.cpp 提出，两模块共用）
namespace paleo::dsp { void fftRadix2(double*, double*, int, bool);
                       int nextPowerOfTwoAtLeast(int); }

// src/algorithms/inversion/（数据层，无 QtWidgets/GIS）
namespace paleo::inversion {
  struct Wavelet;                       // sampleIntervalMs/t0Ms/samples + lengthMs/peakAmplitude/dominantFreqHz
  Wavelet makeRicker(f0Hz, dtMs, lengthMs);
  Wavelet rotateWaveletPhase(w, phaseDeg);
  struct ReflSpike { double twtMs; float amplitude; };
  WaveletExtractResult extractWavelet(trace, n, t0, dt, spikes, nSpikes,
                                      waveletT0, waveletSamples, options={});
  QByteArray waveletToJson(w, extra={});
  bool waveletFromJson(bytes, *w, *extra, *error);

  struct LowFreqWellTrace / HorizonTwtGrid / LowFreqModelInput / LowFreqModelResult;
  LowFreqModelResult lowFreqImpedance(input);      // 层模式/全局趋势回退
  void lowFreqTraceAt(model, il, xl, *out);        // 逐格合成（道并行安全）
  void lowCutMovingAverage(x, n, half, *out);      // lowCut 频段公共低通核

  BandlimitedResult bandlimitedInversion(trace, n, dt, wavelet?, lowFreq?, options={});
  SparseSpikeResult  sparseSpikeInversion(trace, n, dt, wavelet, lowFreq?, options={});

  QByteArray writeImpedanceVolumeBlob(volume, header);
  bool readImpedanceVolumeBlob(blob, *volume, *header, *error);
}

// src/workflow/inversionworkflow.h（功能层，三段式；现状 API 不依赖方向 20）
namespace paleo::inv {
  struct WaveletJobRequest;   // 井曲线+时深+seismic 路径（prepare 算尖峰快照）
  prepareWaveletJob / computeWaveletJob / publishWaveletJob(DerivedAssetRegistrar&);
  struct InversionJobRequest; // seismic+wavelet(+path)、method/lowCut/λ/迭代/
                              // maxThreads(0=auto≤4)、lowFreqWells(TWT)、horizons
  prepareInversionJob / computeInversionJob(cancelled, progress) / publishInversionJob;
}
// 资产类型：wavelet（wavelet.json）、impedance_volume（impedance.iimp）；
// 无状态类，世代号守卫由调用方自管（dock 现成模式）。

// src/ui/seismicsection/inversionpanel.h（视图，只发信号）
InversionPanelParams{method, lowCutHz, lambda, maxIterations, waveletPath};
signals: inversionRequested / cancelRequested / extractWaveletRequested
```

模块登记：`inversion`、`dsp` 是既有 algorithms 模块子目录，不跑
`scripts/new_module.sh`；新 `cmake/extra-seismic-inversion.cmake` 登记进
`_paleo_extra_manifest`（faultsurface 先例）。

## Oracle

| # | 验收 | 证据 |
|---|------|------|
| 1 | 子波提取 | `tst_inversion_wavelet` 9/9（2ms）：真 Ricker 相关 ≥0.95（实测 0.99+）、主频 25±2Hz、长度 65 样、拟合相关 ≥0.99；含噪 5%rms 相关 ≥0.9 且拟合相关如实下降；4ms 同链；无/单/零尖峰显式失败 |
| 2 | 低频模型 | `tst_inversion_lowfreq` 9/9：井位处模型 = 井曲线过同平滑核逐样相等；两井等距中点 = 均值；无层位回退全局趋势（25 格全等）；lowCutHz/平滑窗/层数入结果（编排层 extra 见 #6）；倾斜层位过渡点 1240±半窗；井层缺失 NaN 传播；退化失败 |
| 3 | 带限反演 | `tst_inversion_bandlimit` 9/9：层状模型闭环合并层误差 <2%（实测 0.00%）；零相位相对相关 0.9423 ≥0.90；混合 30° 0.8141 < 零相位且 ≥0.7（如实降级）；裸道 0.7628 ≥0.6；零道返回低频原样；NaN 语义/采样率失配显式失败 |
| 4 | 稀疏脉冲 | `tst_inversion_sparse` 7/7：L1 误差 ≤0.15·‖r‖₁、事件 ±2 样同号、残差 <0.05；含噪残差高于无噪、L1 有界不 NaN；迭代上限 8 轮即停不谎报；退化失败 |
| 5 | 闭环 | `tst_inversion_workflow::bandlimitedEndToEnd`：井曲线→SEG-Y 正演→提取子波（拟合 0.987）→发布→读回→反演→井位格层均值 5500/5900/5300 ±2%（双井横向低频 6400±2%） |
| 6 | 编排 | `tst_inversion_workflow` 8/8：DERIVED 版本（stage/父谱系/extra 键含 frequencyBand）+ 失败/取消不 stage + 字节稳定双跑一致；`tst_inversion_panel` 7/7（信号/回填/不标高分辨率） |
| 7 | 性能 | `tst_inversion_perf`：262,144 道合成体全量带限反演 **2,742 ms**（预算 90,000 ms，ratio 0.030）；道并行加速比 1→4 线程 **3.69**（门 ≥1.6；串行段=SEG-Y 顺序解码，如实实现测） |
| 8 | 真工区 | `tst_inversion_realarea`（env 门控）：200P_seismic.sgy **263,451 道 ×901 样 ×2ms（IBM）** 全量带限反演 **11,110 ms，0 失败道**，只读契约（目录快照逐项不变），体读回头自洽 |

命令：`ninja -C build && ctest --test-dir build -R '^tst_inversion_' --output-on-failure`；
真工区：`PALEO_REAL_PROJECT_AREA=<area> ./build/tst_inversion_realarea`。

## 递延

- 多道横向约束反演（本边界：逐道独立 + 低频模型约束；不做井间变差函数）。
- 叠前 AVO / 弹性阻抗 / 地质统计反演（方向 18 克里金落地后可谈地质统计）。
- 多子波：全工区子波场插值（本边界：单井/井旁道对提取）。
- 井轨迹域模型（方向 19 落地前垂直井语义）。
- 层位栅格直接进 dock 低频输入（当前 UI 链路用候选井全局趋势；层位网格 API
  已备——`HorizonTwtGrid` 传即用）。
- SEG-Y 体写回（产出是 IIMP1 DERIVED 资产；如需 SEG-Y 输出另立 writer）。
- 阻抗体三维可视化（复用 seismic3d 面，另开方向）。
