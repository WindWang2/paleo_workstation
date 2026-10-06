# 方向58 audit-tail：AUDIT_ISSUES 尾项终态（BIZ-07 / RUNTIME-03 / BIZ-10 / BIZ-11 / TEST-06）

分支 `goal/audit-tail-20261007`（基 `origin/master f130fb2`）。账本 `.goal-loop-ledger-audit-tail.md`（R0–R7）。

## 变更
| 项 | 提交 | 判定 | 验证 |
|---|---|---|---|
| BIZ-07 临时配准仿射参数闸 | 7c94359, ed2ea71 | 已修 | `tst_import_geoaffine`：RED 旧写口 9/9 坏参数（NaN/Inf/零缩放/1e300）照常写盘 → GREEN 12/12；`registration.cpp` 仅语法检查 |
| RUNTIME-03 LAS 合乘诊断回填 | 0d01bd6 | 已修 | `tst_cache_las::coalescedRidersReceiveIssues`：RED 搭车者 `issues` 0≠1 → GREEN 13/13，8 次重复稳定 |
| BIZ-10 工程 CRS 约束变换 | f359a74 | 标记漂移 + 窄残余加固（**非「已修」**） | PROJ 9.6 复现 ENGCRS→EPSG 0 候选操作（`biz10-projinfo.txt`）；三处算法仅语法检查 |
| BIZ-11 无投影 GeoTIFF | 0e7df51 | 窄残余已收 + 一项文档化残余 | 11 个写口调用点逐一分类（账本 R4）；`tst_algorithm_rasterout_crs` 仅语法检查 |
| TEST-06 零测试头 | 9d8dfa8 | 高/中危已清（方向52）；低风险 18 项逐项终态 | `test06-untested-headers.md`（待补 7 / 不值得 11） |
| AUDIT_ISSUES 终态 | 58c2161 | 46 个 ID 全部有终态，无未注记「仍存在」 | — |
| 评审修复 | 0e8c9fc, 776946a | — | 见下 |

## 验证环境（如实）
- 本机无可链接的 QGIS 4.2（Debian 13 glibc 2.41 < 26.04 闭包要求）。数据层/算法层非 QGIS TU 走子集构建（Qt 6.8 + GDAL 3.10，-j8）并实跑；**QGIS 依赖 TU 只做 syntax-only**（QGIS 4.2.3 头）：`src/workflow/registration.cpp`、`src/algorithms/{paleoalgorithms,distancetransform,mincurvature}.cpp`、`tests/tst_algorithm_rasterout_crs.cpp`。需 CI 两 leg 确认。
- 子集 R0 红集 = ∅，终态红集 diff = ∅（tst_cache_las / cache_async / cache_core / horizonbinner / ensemblestats / import_geoaffine）。
- 门禁：check_layering 默认/--strict/--selftest 绿，--transitive/--symbol-audit 与 master 一致；check_i18n、check_ui_invariants 绿；`git diff --check` 清。

## Low 项（评审记录，未改）
1. `geoAffineParamsValid` 原因串为数据层 `QStringLiteral` 中文（与 geojsonaffine 既有错误串同口径），经功能层 `tr()` 包裹上报；新 `tr("仿射参数不是数值：%1")` 未跑 lupdate，翻译骨架待合并时统一重跑。
2. 仿射量程阈值（1e-9 / 1e9 / 1e10 m）为工程判断，非来自规范；负缩放（镜像）保持合法。
3. RUNTIME-03：内存/磁盘命中不重放诊断是既有契约，仅在 `lascache.h` 注释钉死，未改；`LasCache` 新增两个 `ForTest` 测试缝（生产不设钩子）。冷解析路径现在总收集诊断（调用方未传 issues 时也收集），开销可忽略；失败时 `doc.error` 也能从 Error 级诊断取到具体原因（对无 issues 指针的调用方是改进）。
4. BIZ-11 回落假设「工程内无 CRS 输入即在局部网格上」；外部导入的无 .prj 投影数据在 workflow 路径会被标为局部网格（与工程约定一致，但非数据自证）。
5. BIZ-10：`QgsCoordinateTransform::isValid()` 对「两端 CRS 有效但 PROJ 无操作」的返回值取决于 QGIS 内部；无论哪种，新代码都给出 QgsProcessingException 报因（构造期或逐要素）。
6. 工作流级 BIZ-07 夹具（需 DataImportService + QgisLayerService）未加；io 层测试覆盖同一闸函数。
7. TEST-06 扫描基于 include 文本图（按文件名匹配），不代表链接期执行覆盖。
