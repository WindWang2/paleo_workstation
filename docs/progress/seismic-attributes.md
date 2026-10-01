# seismic-attributes — 地震属性引擎（goal/seismic-attributes-20261001）

分支 `goal/seismic-attributes-20261001`（自 master 2d33c5e 起）。迭代账本
`.goal-loop-ledger-seismic-attributes.md`（轮次/Oracle 证据）；本文是交付记录。

## 交付一览（按提交）

| 提交 | 内容 |
|------|------|
| feat(algorithms) | 属性核函数库 `src/algorithms/seismicattr.{h,cpp}`：自研 radix-2 FFT + 镜像填充频域 Hilbert（解析信号）；瞬时族（包络/瞬时相位/瞬时频率/瞬时 Q 原型）；时窗振幅族（RMS/最大绝对振幅/平均能量）；semblance C2 相干（IL/XL 半窗分设）；甜点。纯数值零 Qt/GIS 依赖，数值单测 13 例 |
| feat(services) | `SeismicTaskService` 属性任务面：`startAttributeSlice()`（≤4 并发闸、读/算两段进度单调、三检查点协作取消、逐道 ≤4 线程分片）+ `registerAttributeSliceAsset()`（SATR 容器 + catalog DERIVED 登记）。测试 8 例 |
| feat(ui) | `SeismicSectionCanvas` 属性叠加层（同几何半透明色层、NaN=透明、几何失配防线）；`SeismicAttrPanel` 参数面板（表单→意图信号）；dock 编排（「◈ 属性」开关、计算/取消/登记闭环）。offscreen 全链测试 4 例 |
| test(perf) | 比率门（合成体：逐道族 ≤8×/相干 ≤12× 切片提取基线，机器无关）+ `PALEO_REAL_PROJECT_AREA` 门控真机实测（BASELINE 行誊下表） |

## 语义决策（合并评审重点）

1. **Hilbert 走频域法**（镜像填充 reflect 到 ≥2n 的 2 的幂 + 正频×2/负频清零，
   scipy.signal.hilbert 同约定）。vendor/系统/Qt 均无合规 FFT（FFTW double-only
   头且未链接、破坏钉位策略）——自研 radix-2 是零新依赖唯一路。
2. **有限道 Hilbert 边缘效应**：镜像折点（迹缘 C1 不连续）的 Hilbert 尾 ~1/d
   衰减，实测（1000 样/30Hz 正弦）包络误差 0.035@d=100；FFT 填充 2n→4n 零改善
   （误差源是折点非缝合点）。测试护栏 20% 带按实测定门——工业整道 FFT Hilbert
   同级行为，非缺陷。
3. **瞬时频率用相位差分法**（解析信号共轭乘，Barnes 2007）——免解卷绕；
   负值如实保留（真实数据现象）。
4. **相干 = C2 semblance**（Marfurt 1998）：`S=Σt(Σj u)²/(J·ΣtΣj u²)`，
   [0,1] 有界、无特征分解；IL/XL 半窗分设（两向道距不等各取各的）。
   全零窗精确零判 NaN（0/0 未定义）；子波远尾微小能量窗（~1e-38）合法
   （同波形恒 S=1），不得用绝对阈值误杀——轮2 实测教训。
5. **瞬时 Q 为原型**：`Q=π·f/|d ln(env)/dt|`（中心差分）；包络无衰减处 NaN
   （无信息，不臆造）；衰减指数合成件内段偏差 ≤5%。
6. **Time 切片属性如实拒绝**：瞬时族需整道谱、时窗族需垂向窗——单采样面
   不满足输入形状；整体扫描属性体递延（TODOS）。
7. **稀疏测网邻线按轴值表相邻位解析**（不臆造 ±1 号）；测网边缘线相干
   如实拒绝（无双侧邻线）。
8. **切片↔体窗布局**：切片 [row][x] 行主序（row 0=最深样）vs 核体
   [(line,x)·nS+s] 道连续——组装/出图两次显式转置（平铺拷贝会行列搅混，
   轮2 二连教训：恒 1/3 假象/边界 NaN 移位）。
9. **属性切片显示语义 = 剖面叠加层**（密度之上、wiggle/解释要素之下，透明度
   可调，NaN=透明）；层树栅格条目递延——非地理参考的剖面属性图无诚实栅格
   URI，待整体扫描属性体（时间切片天然 IL×XL 地理栅格）一并落。
10. **SATR 容器**：`SATR` 魔数 + 版本 + width/height + JSON 头（参数/来源/
    值域/耗时）+ 小端 f32 值块；登记先例对齐 registerFaultAsset（asset
    `seis_attr_<源>_<attr>_<il|xl>_<线号>`，DERIVED 外链版本，父版本回指
    地震 RAW，extra.origin=seismic-attributes）。

## 966MB 真工区实测（tst_seismicattrperf，PALEO_REAL_PROJECT_AREA 门控）

环境：`200P_seismic.sgy`（il 1315..1725 × xl 4165..4805 × ns 901 @ 2000µs），
中段 IL（1520），直读后端，单次热态：

| 指标 | 延迟 |
|------|------|
| 体打开（冷，直读 Load） | 418–527ms |
| IL 切片提取（基线） | 14–15ms |
| 包络（读 15 + 算 21） | **36ms** |
| 瞬时频率（读 13 + 算 20） | 33ms |
| 甜点（读 13 + 算 21） | 34ms |
| RMS（读 16 + 算 4） | 20ms |
| 相干 3×3×3（读 45 + 算 33） | **78ms** |

- 逐道族读=单线、相干读=三邻线（读耗时 ~3× 印证）；逐道计算 ≤4 线程分片，
  相干整线单线程（641 道 × 901 样 × 27 窗乘加 ≈ 33ms，交互级）。
- 全属性 <100ms——交互级可用；比率门见 `tst_seismicattrperf::attributeCostRatioGate`
  （逐道 ≤8×、相干 ≤12× 基线，防数量级回归）。
- 只读契约：源目录前后清单一致（tst 同步断言）。

## 已知边界 / 递延（TODOS 同步）

- 时间切片/整体属性体：需全测网逐道谱（或分块体窗）——SATR 体格式与
  时间切片栅格上图一并落。
- 相干沿剖轴半窗按剖向映射（IL 剖→XL 向），trace 间距各向异性未做道距
  加权（标准 C2 可加距离权，当前等权）。
- 瞬时 Q 原型语义（解释慎用）；曲率未做（需层位输入，另立方向）。
- 属性结果缓存：同（体×属性×线×参数）重复计算未去重（数据缓存层
  SgyDataCache 形态可复用，量级小暂不做）。
