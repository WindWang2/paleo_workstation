# 算法审计：§10–12 清单 × 系统实现现状（只读审计）

状态：**审计快照 2026-09-26**（wave3/model-hardening；对应 TODOS P1「native:\*
算法逐项审计」）。只读审计 + 建议替代——本文档不伴随算法实现改动。证据等级：

- **[测试]** 结论有可执行断言背书：`tests/tst_algorithm_harness.cpp`
  `providerSurfaceContract`（provider 面契约 + native:\* headless 实跑）。
- **[CLI]** 本机 `qgis_process`（QGIS 4.2.2-Belém）枚举：`--no-python` 只有
  `native`(343)/`3d`/`pdal`；带 Python 才有 `gdal`(59)/`qgis`(22)/`grass`(307)。
- **[代码]** 给出源码指针。

## 1. 前提：C++-only 应用的 Processing 面（最重要的事实）

三条硬事实（全部 [测试]+[CLI] 双背书）：

1. **`QgsApplication::initQgis()` 不自动注册任何 Processing provider。**
   Paleo 应用目前只注册自家的 `paleo` provider
   （`src/algorithms/paleoalgorithms.cpp` `PaleoProvider::loadAlgorithms`）。
   「QGIS 已经提供正式的 IDW/Contour」这句 plan 里的假设，在本应用的
   进程里**默认一条都不成立**。
2. **native provider 是纯 C++**：`QgsNativeAlgorithms`（qgis_analysis，
   `/usr/include/qgis/qgsnativealgorithms.h`），343 个 `native:*` 算法，
   一行 `addProvider(new QgsNativeAlgorithms())` 即可用（测试已实跑
   `native:distancetonearesthub` 产出要素）。
3. **gdal:/qgis: provider 在本发行版是 Python 实现**：C++ 进程永远拿不到
   （`libqgis_analysis` 无 C++ GDAL provider，strings 零命中）。plan §11
   树里写的 `QGIS IDW / QGIS TIN / GDAL Grid / Contour / Raster Calculator`
   五个外部依赖里，四个（QGIS IDW、TIN、GDAL Grid、Contour 的现成
   Processing 封装）都长在这两个 Python provider 上。

**首要建议（一行、零算法实现）**：`QgisProcessingService` 构造函数
（src/qgis/qgisprocessingservice.cpp:445-453，现有 PaleoProvider 注册点）
同处 `addProvider(new QgsNativeAlgorithms())`——立刻解锁 343 个 C++ 算法
（含距井距离、zonal statistics、rasterize、polygonize、slope/aspect）。
归集成者接线（src/qgis 为共享面），本文不直接改。

## 2. §10–12 逐项审计表

| Plan 条目（§节） | 需要 | 现状 | native/provider 名 | C++/GDAL 实现在哪 | 缺口 | 建议 |
|---|---|---|---|---|---|---|
| 普通 IDW（§10.1） | 井点→栅格 | **已覆盖** | `paleo:paleo_constraint_idw`（不带 CONSTRAINTS 即普通 IDW） | 自研，src/algorithms/paleoalgorithms.cpp | 无 | 无需 `qgis:idwinterpolation`（Python-only）；plan §10.1 的「优先调 QGIS Interpolation Provider」修订为「用自家 constraint_idw 无约束档」 |
| 约束 IDW（§11） | 井点+物源/展布线+地质边界 | **已实现** | `paleo:paleo_constraint_idw` | 自研（软屏障绕行距离） | 无（[测试] tst_algorithms/tst_algorithm_harness） | — |
| 相融合（§11） | N 单相栅格→编码栅格 | **已实现** | `paleo:paleo_facies_fusion` | 自研 | 无 | — |
| 地质光滑（§11） | 编码栅格多数滤波 | **已实现** | `paleo:paleo_geological_smoothing` | 自研（3×3 众数，不跨相插值） | 无 | — |
| 地层/砂体厚度（§10） | top−base | **已实现** | `paleo:paleo_isopach` | 自研 | 无 | — |
| 相多边形化（§36） | 栅格→可编辑面 | **已实现** | `paleo:paleo_facies_polygonize` | 自研（GDALPolygonize+GEOS coverage） | 无 | — |
| **最近井距离（§10）** | 井位→距离面 | **缺口** | `native:distancetonearesthub` / `native:distancematrix` | native（C++）**已存在**，但应用未注册 provider | provider 未注册；hub 线→栅格还差一步 rasterize | 注册 QgsNativeAlgorithms 后：`native:distancetonearesthub`（点→hub 连线+距离）+ `native:rasterize`；或自研 `paleo:distance_to_wells`（井点场 GDALComputeProximity——**GDAL C API，libgdal 已链接**，工作量 S） |
| 预测置信度（§10） | ONNX 输出面 | 自研 ONNX 服务 | —（不走 Processing） | src/ai/onnxpredictionservice.cpp | 无（B 包 D15 领域） | — |
| **TIN 插值（§12）** | 保离散控制点插值 | **缺口** | `qgis:tininterpolation`（Python provider） | C++ 数学内核在（qgis_analysis `QgsTinInterpolator`/`QgsInterpolator`），Processing 封装是 Python | 无 C++ Processing 算法 | 触发条件未满足（编图链用 IDW 已够）：需要时自研 `paleo:tin` 直调 QgsTinInterpolator（工作量 M） |
| **等值线（§12）** | 栅格→等值线要素 | **缺口** | `gdal:contour`（Python provider） | **GDAL C API `GDALContourGenerate`——libgdal 已链接** | 无 C++ Processing 封装 | TODOS P1 点名的 raster contour 缺口：自研 `paleo:contour` 包 `GDALContourGenerate`（工作量 S，参数 interval/base→线要素 gpkg） |
| **Raster Calculator（§11/§12）** | 逐像元表达式（砂地比等比率图） | **缺口** | `qgis:rastercalculator`（Python）；native 面无同名（[测试] 断言 `native:rastercalculator` 不存在） | 无现成 C++ | 砂地比=两厚度栅格相除这类比率图做不了 | 自研 `paleo:ratio`（A/B 两栅格逐像元除 + 除零→nodata，工作量 S）可先于通用表达式计算器；通用 `paleo:rastercalc`（QgsRasterBlock 逐块求值，M）递延到有真实表达式需求 |
| GDAL Grid（§11） | 点→栅格格网 | 缺口（名义） | `gdal:grid`（Python provider） | GDAL C API `GDALGridCreate` 在 | 与 IDW 用例重叠 | 不引入；constraint_idw 已覆盖井点插值主路径。记录即可 |
| Zonal statistics（§21 验证模块可能用） | 面内统计 | native 可用 | `native:zonalstatistics` | native（C++） | provider 未注册 | 同首要建议，注册即得 |
| 坡度/坡向等地形类（§10 延伸） | 地形因子 | native 可用 | `native:slope` / `native:aspect` | native（C++） | provider 未注册 | 同上 |

## 3. 结论（按工作量排序的行动清单）

1. **S（一行）**：`QgisProcessingService` 注册 `QgsNativeAlgorithms` ——
   解锁最近井距离、zonal stats、rasterize、slope/aspect 等 343 个 C++ 算法。
   可行性已由 `providerSurfaceContract` 实跑证明。
2. **S（一个算法）**：`paleo:contour` 包 `GDALContourGenerate`（TODOS P1
   点名缺口；libgdal 已链接，无新依赖）。
3. **S（一个算法）**：`paleo:ratio` 或最小逐像元 A/B——补砂地比/比率类
   单因素图；通用栅格计算器递延。
4. **M（递延，触发条件已写）**：`paleo:tin`（直调 qgis_analysis 的
   QgsTinInterpolator）；`paleo:distance_to_wells` 若 native 两步管线
   （hub→rasterize）精度/性能不满足再做 GDALComputeProximity 单步版。
5. **不做**：gdal:grid（用例被 constraint_idw 覆盖）、任何对 Python
   provider 的依赖（架构红线）。

## 4. 与 plan 文本的差异备忘（供编排会话对账）

- §10.1「优先调用 QGIS Interpolation Provider」：QGIS 的 interpolation
  Processing 算法在本应用拿不到（Python provider）； Paleo 自家
  constraint_idw 事实上承担普通 IDW。建议 plan 措辞改为指向 paleo 算法。
- §11 依赖树中「QGIS IDW / QGIS TIN / GDAL Grid / Contour / Raster
  Calculator」五行：前四项与最后一项的现成封装均为 Python provider；
  C++ 可用面 = native provider + 自研 + GDAL C API。
- 本审计不改 `docs/PALEO_QGIS_PLAN.md`（对账归编排会话）。
