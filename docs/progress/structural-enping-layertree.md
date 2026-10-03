# 结构单因素对拍（恩平工区）+ 图层树重排

状态：进行中（未提交）。范围是 `singlefactor-completion.md`（方向23）之后
的两条收口：①约束线/结构单因素对上游 `haiyou-visualization@27fdb99` 的
恩平数据验证，②图层树「置顶共享 + 层位组」重排（WS-B）与约束页结构化
参数面（WS-C5）。

## 范围

- 工作目录：主仓 `paleo_workstation`（非 worktree），构建目录 `build-sb`
  （vendored QGIS 4.2.2 + superbuild 补丁）。
- 参考：`/home/kevin/projects/_refs/haiyou-visualization` `27fdb99`。
- 数据：`/home/kevin/下载/恩平一二段_汇报资料整合包.zip`（解压于
  `~/.cache/paleo/enping/`；黄金值 `enping_golden.json` 同目录，**不入库**）。
- 参考链与黄金值生成：`tools/reference/singlefactor/README.md`
  （`run_structural_reference.py` / `make_structural_synthetic.py` /
  `compare_structural.py` / `compare_results.py`）。

## 图层树重排（WS-B）

- **布局器 = `QgisLayerOrganizer`**（`src/qgis/qgislayerorganizer.{h,cpp}`，
  挂在 AppContext，工程打开后 `reorganize()` 全量归位一次，此后逐图层
  跟随 `legendLayersAdded`/`layerInstantiated` 归位）。
- 三分区：`00_Data` 共享层（井位 wells、井轨迹 well_trajectories、
  测区边界 boundary.*）置顶树根；层位声明带 `paleoHorizon` 凭据组
  （按 `mappingHorizons()` 浅→深排序）；组内按 canonical 组秩
  （07→00，05_PaleoMap 顶、00_Data 层位底图垫底）；非层位产层
  （05_PaleoMap 等无 horizon 声明）落层位组带之下的共享平铺区。
- **无声明手动层 = Unmanaged，对摆放透明**：不移动、不被挤位。
- **归位 = clone+insert+remove**：图层引用、勾选态、自定义属性全保留；
  先摘后插的自排除坐标系修正了 off-by-one。
- **拖拽不被接管**：`QgsLayerTreeModel` InternalMove（mimeData 克隆路径）
  不触发 `legendLayersAdded`——用户手动摆位保持，规整只在工程打开与
  实例化时刻收敛。
- **`QgsProject::setInstance` 修复**：`dropMimeData` 反序列化走
  `QgsProject::instance()` 单例——服务自有工程不注册单例时，拖动的
  图层/组 `layer()` 悬空 → 画布空白（「拖动丢显示」的根因）。服务构造
  时注册，析构时 QGIS 自清。
- **显示序 = 树序**：`QgsLayerTreeMapCanvasBridge` 既已接管画布序；
  `layertreepanel::currentLayerGroup` 对 `paleoHorizon` 组回退 decl.group
  （层位组不是编图组，跳页仍按声明组）。
- 当时旧组名兼容口径（历史快照）：`02_Constraints` 为旧数据输入别名，
  当前约束产点已统一为 `03_Constraints`；`01_Prediction`/`03_Predict`/
  `03_Composite` 经 `PaleoLayerVocabulary::canonicalize` 在消费面折算；
  约束声明产点本波已写 canonical `03_Constraints`。

## 标注随图层 z 序（WS-A）

- vendored QGIS 补丁 `vendor/superbuild/patches/qgis-4.2.2-labels-with-layer.patch`：
  图层自定义属性 `rendering/labelsWithLayer=true` 时，该层标注在本层渲染
  目标上、随本层内容被上层盖住（每层一个 `QgsDefaultLabelingEngine`，
  PAL 避让只在本层标注间）。处理了 second-pass job、缓存失效、
  选择性掩膜源（掩膜源图层保留共享引擎）与 staged engine（GeoPDF/排版
  导出不受影响）。编译宏 `QGIS_PALEO_LABELS_WITH_LAYER`。
- `QgisLabelZOrder`（`src/qgis/qgislabelzorder.{h,cpp}`）给工程内每个
  图层钉该属性（存量 + layersAdded + readProject 后），并清掉旧
  `rendering/renderAboveLabels` 残留。
- **仅 build-sb 生效**：普通 `build`/系统 QGIS 无此补丁，属性为无害空值，
  标注回原生置顶（优雅降级，不是回归）。

## 约束页结构化面 + 边界导入（WS-C5）

- `factorMethodCombo` 增 `structural_idw`（「结构 IDW（测区边界）」）；
  选中时展开成图边界下拉 + 「导入…」+ 格网分辨率（默认 339 = 上游默认，
  沿边界最长边结点数），cellSize 行对 structural 隐藏且不作生成条件。
- `ConstraintPage::boundaryImportRequested` → 壳层文件对话框 →
  `MappingWorkbench::importBoundaryLayer`：面图层校验（必须是多边形、
  局部米制 CRS——`acceptsLocalGrid`，LOCAL_CS/工程局部网均收）→
  GPKG 快照进 `artifacts/layers/` → 声明 `boundary.<文件名>`（00_Data、
  无层位绑定 → 置顶共享区）→ `applyBoundaryLayerStyle` 空心填+墨描边。
- 约束 SHP 导入语义：`importConstraints(h, path, role)` 走
  `readConstraintImportFeatures`（auto 判定方向线/打断线，失败时壳层
  询问用户角色重试）；入库 type 语义化（direction_line/break_line/
  interpretive_boundary/contour_stop/cartographic_detour），
  RATIO/INF_RADIUS/CORE_R/BLK_MODE/ACTIVE 落 params_json。
  **画线不再每条出一个快照版本**——快照只在 generateFactor/import 使用点。
- `generateFactor` 的 cellSize 像素闸门对 `structural_idw` 旁路
  （其域规模由边界多边形+GRID_RESOLUTION 控制）。
- 样式：`applyConstraintLayerStyle` 按 type 分类渲染（方向红实线/打断墨
  实线/软边界橙虚线/停线灰虚线/制图绕行蓝点划线）；
  `applyContourLayerStyle` 细灰线 + ELEV 沿线标注（标注随层，不压他层）。
- structural 因素的等值线强制上游 `field_contours` 提取，interval≤0 =
  自动级别；绝不静默降级 GDAL（`job->structural` 分派）。

## 恩平对拍结果（C++ vs 上游）

`PALEO_ENPING_PACKAGE`/`PALEO_ENPING_GOLDEN` 指向 `~/.cache/paleo/enping/`
下的解包工区与 GEOS 3.15 黄金值（Shapely 2.1.2 捆 GEOS 3.15.0；等值线
几何对 GEOS 版本敏感，3.13 轮子的黄金值弃用）。

```text
PALEO_ENPING_PACKAGE=~/.cache/paleo/enping/恩平一二段_汇报资料整合包 \
PALEO_ENPING_GOLDEN=~/.cache/paleo/enping/enping_golden.json \
PALEO_STRUCTURAL_DUMP_DIR=/tmp/enping_ref/cpp_dump \
QT_QPA_PLATFORM=offscreen ./tst_singlefactor_structural enpingGolden
```

| 项 | 结果 |
|---|---|
| 采用井 | 34/34（与上游逐一相同） |
| 跳过井 | 2（DEMO_MISSING 等缺值如实跳过——**不复刻**上游 OCR_CONF 回退） |
| 格网 | 339×226，mask_diff=0，valid_mask_diff=0 |
| 数值场 | maxAbs 1.22e-15，RMSE 1.49e-16（基本位级一致） |
| 轴向 | axes_max_abs 6.8e-13 |
| 表面生成耗时 | ~210 ms |
| 等值线（fieldcontours） | 级别条数/开合/对称 Hausdorff 全过，~6.9 s |
| 合成夹具 | maxAbs 1.36e-15，mask_diff=0，26/26 井 |

测试：`tst_singlefactor_structural`（importDefaults / importAutoRoleDetect /
syntheticGolden / enpingGolden）与 `tst_singlefactor_fieldcontours`
（syntheticGolden / enpingGolden / structuralContoursEndToEnd）全过。
`compare_structural.py` 独立脚本复核 dump：ok=true。

## 验证矩阵（build-sb，vendored QGIS；当轮历史快照）

下列执行记录保留原样。当前构建/测试资源纪律统一为最多 `-j8`，不再沿用
历史 `nproc` 写法；当前红项对照见 docs/progress/job-framework.md 文末。

- 全量 `ctest -j$(nproc)`：234/238；2 个真实回归已修（tst_workflows /
  当时 tst_constraintstore 断言仍写 `02_Constraints`，产点已迁 canonical——
  断言改 03）；2 个环境性波动见「已知」。
- 图层树：`tst_layerorganizer` 8 例（置顶/层位序/组内秩/占位组/剪枝/
  归位保勾选引用/拖拽不接管/手动层透明）、`tst_layertreepanel`、
  `tst_dragtop_render`（InternalMove 保子节点与勾选）通过。
- 标注：`tst_labelzorder` 9 例过（含 parallel/sequential 两条像素回归——
  证明 build-sb 的补丁引擎真实生效）。
- 门槛：`layering`、`layering_strict`、`layering_selftest`、
  `ui_invariants`、`ui_invariants_strict`、`ui_invariants_selftest`、
  `i18n`、`i18n_selftest` 全绿。

## 已知/边界

- `tst_singlefactor_perf`：L 中位 15159 ms（门 15000，空载复测同水位）——
  被测核 `localidw.cpp` 本波零改动，上一轮同机 14334 ms；判定为机器噪声
  级边界外波动，未放宽门。
- `tst_perf_catalog`：仅在 238 并发全量下复现一次 SHA 查找顺序错位，
  空载通过——与既有「并行负载下启动迹线偶发」同类，非本波引入。
- 用户拖拽摆位在**会话内**保持；工程重开 `reorganize()` 收敛回 canonical
  序（设计取舍：树位权威是声明而非拖拽历史）。
- 等值线标注避让只在本层标注间（per-layer PAL）——跨层标注可视觉相碰，
  但压盖序正确；这是「标注随图层」语义的固有边界。
- Enping 包与黄金值不入库；无 env 时 enpingGolden 两测试 QSKIP。

2026-10-03 对账：当前约束组单点定义为 `PaleoLayerVocabulary::kConstraintsGroup`
（`03_Constraints`）；新夹具与产点均用该常量，`02_Constraints` 仅保留在
canonicalize/groupFamily 与旧数据兼容用例。上文旧断言/修复过程为历史快照。
