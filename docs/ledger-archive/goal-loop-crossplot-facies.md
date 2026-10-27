# Goal Loop ledger — crossplot-facies

- 日期：2026-10-02；分支 `goal/crossplot-facies-20261002`，从 `origin/master` 11099e2 创建；worktree `.worktrees/crossplot-facies`。
- 构建使用主仓 `vendor/superbuild/prefix`；ONNX runtime 复用主仓下载物。
- 七项 Oracle：数值核、统一采样、交会 UI、分类回写、编图接入、性能/取消、文档。每轮补命令与实测输出；未跑不标通过。

## 轮0 — 接口定案

- 井曲线：`LasDoc`/`LasCurve`（`io/lasdoc.h`），解析入口 `LasCache`/`PreviewDocService::lasAt`；同 LAS 行对齐，NULL 为 NaN。新增 `CrossplotSamples::well(channels, control)`：多通道显式深度，第一通道为参照网格，精确匹配/邻点线性插值、不外推、不跨 NaN；源深度必须严格递增。保留井 id、深度、XY、来源版本。
- 栅格：新增 `CrossplotSamples::rasters(sources, control)`，第一张栅格是参照网格；像元中心经完整六参数仿射、CRS 转换，再到源包含像元；GDAL 块读与 nodata/mask 联合剔除。属性时间切片走同一网格 DTO；SATR 垂直切片无地图平面几何，必须诚实拒绝与层位平面混采。
- 统一值型 `domain/crossplotsamples.h`：行主序多维值、维名/单位、样本空间位置、参照栅格几何、来源版本。≥3 维直接进入纯数值核。
- 核接口 `algorithms/cluster/cluster.h`：k-means++、对角 GMM EM、凸包/盒/原始 lasso 多边形；回调进度与取消。类别为 0 起的无监督簇 id；地质相名称需显式映射，不自动赋地质解释。
- UI：复用底部 `bottomTabs`/原生 dock、`PaleoTheme` 和自绘图表惯例。新增 `ui/crossplot`，只画图、发轴/选区/定位/任务意图；投影、密度计数、选区统计交给服务。2D/3D 正交投影共享屏幕选区语义。
- 功能编排 `workflow/faciesclassify`：任务池计算，`DerivedAssetRegistrar` 登记受管分类 GeoTIFF 与井层段表；写操作经 `PaleoProjectStore::enqueueWrite`，catalog 在服务线程登记。palette、参数 hash、父版本写 provenance。分类栅格声明为 `predict.<horizon>.crossplot`，复用编图页已有相栅格选择和 `deriveFaciesPolygons`；不改其内部算法。
- 定位复用 `QgisCanvasController::zoomToPoint`、`SelectionContext` 井选中广播；栅格位置通过 QGIS 封装转换到画布 CRS。
- 禁区：无第三方 ML、监督训练、mapbook 改动；递延 SOM、时深域交会。

证据：`git fetch origin master`、`git worktree add .worktrees/crossplot-facies -b goal/crossplot-facies-20261002 origin/master` 成功；读取 DESIGN.md、架构/分层计划及上述接口。确认 composepage.cpp 的现有入口接受 `predict.*` 栅格。

## 轮1 — 聚类核

- 自研行主序 k-means++ / Lloyd、对角 GMM / EM（log-sum-exp、方差下限）、BIC；纯 STL，无 Qt/ML 依赖。空集、单点、全同值实际 k、非法输入/数值溢出、协作取消均明确返回。
- k-means 置信度 = 1−最近/次近平方距离（相对分离度）；GMM = 最大后验概率。原始 lasso 偶奇规则保留凹顶点顺序，凸包另走 monotone chain；边界包含。
- 命令：`cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DQGIS_PREFIX=/home/kevin/projects/paleo_workstation/vendor/superbuild/prefix`；`cmake --build build --target tst_cluster -j4`；`ctest --test-dir build -R '^tst_cluster$' --output-on-failure`。
- 输出：1/1 PASS（2.08s）；数值断言覆盖双峰中心 <5%、纯度 >.95、k=1 精确均值、两高斯均值/权重 <10%、BIC 解析式/优于 k=1、EM 似然非降、退化与取消、凸包/凹 lasso/盒/边界。`python3 tools/check_layering.py --strict`：无层违规。

## 轮2 — 统一采样

- `SampleSet`、LAS 深度对齐、GDAL 像元配对（仿射/CRS/nodata/mask）、多维属性平面、投影/密度/选区统计均在数据层。
- `attributeHorizon(sections, horizons, control)` 明确接时间层位（ms）：XY 找层位包含像元→TWT 找 SATR 时间行（row 0 为最深）；不外推、联合缺失剔除，保留参考栅格像元映射。未知时间轴/不一致几何诚实失败。
- 命令：`cmake --build build --target tst_crossplot_samples -j4`；`ctest --test-dir build -R '^tst_crossplot_samples$' --output-on-failure`。
- 输出：1/1 PASS（0.47s）；深度插值精确值/NaN 不跨越/无外推/无序拒绝；栅格 12 像元联合剔除 2 → 10 对；属性×层位 4 道剔除 1 → 3 对且值为 0/11/22；三维直接聚类；投影选区 N/均值/比例与凹多边形语义。strict 分层与 i18n 通过。

## 轮3 — 交会面板

- 原生底部页签面板 + 2D/3D 轴选择、方位/俯仰、100k 自动密度云、套索/Shift 框选、分类与写回/取消意图；所有投影、点拾取与 N/均值/占比来自数据服务。
- 命令：`cmake --build build --target tst_crossplotpanel -j4`；`PALEO_CROSSPLOT_SCREENSHOT=/tmp/crossplot-panel.png ctest --test-dir build -R '^tst_crossplotpanel$' -V`。
- 输出：UI 意图/轴切换/点击/框选/统计/忙态门控断言 PASS；100k 密度格总和精确 100000，2D 与 3D 重绘实测 BASELINE 行保存在 `/tmp/crossplot-panel-evidence.log`（轮5汇总）。所有新 UI 颜色/字体取 PaleoTheme，数据类别色单独列为符号 palette。strict 分层、i18n 通过。
- 全仓 `ui_invariants_strict` 发现既有 petrophyspanel.cpp 的裸 QSS 色值；收尾门禁需修复到主题 token（不改其功能）。

## 轮4 — 分类回写、编图与实际壳接线

- 新增 `FaciesClassificationService`（默认 z-score、多维凸包/盒手选、簇置信度/距离、参数与采样 provenance）、`FaciesClassifyWorkflow`（任务世代/取消/失败、单写队列、DERIVED 登记）及组装根 `CrossplotController`。
- 入口：底部「交会相分类」页签，`Ctrl+Alt+X`；catalog LAS 原始/派生曲线、GeoTIFF、SATR 资产直接列为可选通道。SATR 读取器验证魔数/版本/大小，并以源 survey、时间层位采样；普通栅格不冒充 TWT。外链 SHA 复验，测网拟合残差记入采样 provenance。
- 分类 GeoTIFF：Byte palette，0–254 簇 id、255 nodata。受管路径 + sha256 + 父版本 + parameterHash；声明 `predict.<horizon>.crossplot` → 既有编图相栅格入口与 `deriveFaciesPolygons`。井层段 JSON DERIVED：interpretation 角色关联，原始行号阻止跨 NaN 断段；端点为观察深度，单点段 top=base，未伪造厚度。
- `CrossplotMapLink`：地图 CRS 转换、精确选中样本点高亮（一次变换对象）、点定位；井选区走 SelectionContext，栅格选择聚焦源图层树。
- 命令：`cmake --build build --target paleo tst_crossplot_samples tst_crossplotpanel tst_faciesclassify tst_crossplot_controller -j4`；`ctest --test-dir build -R '^tst_(crossplot_samples|faciesclassify|crossplot_controller)$' --output-on-failure`；全部 3/3 PASS。
- 补充完整 Oracle 命令：`PALEO_REAL_PROJECT_AREA=/home/kevin/projects/paleo_project/data/project_area PALEO_CROSSPLOT_SCREENSHOT=/tmp/crossplot-panel.png ctest --test-dir build -R '^tst_(cluster|crossplot.*|faciesclassify)$' -V`：6/6 PASS（10.88s），含轮5性能 harness 的预跑。
- 断言：SATR→源测网→层位→3 对精确属性值（20/11/2）；拟合坐标误差按既有 25m 闸门，夹具 y 残差 −2m，拒绝伪称精确几何。井段 0–1、3–3、4–4 不跨缺失行；分类栅格 15 有效+1 nodata、palette/统计、catalog 重开、只读拒写；真实壳通道读入→轴换→异步分类→写回→编图下拉可见；地图中心/精确点高亮、坏源重读清除旧样本并禁用操作；任务取消/失败如实。

## 轮5 — 真工区、性能与交付审查

### 审查修复

- 全量审查 `git diff origin/master...HEAD` 及轮5未提交差异：无调试打印/死代码，BASELINE 输出仅在测试；纯数值核无第三方 ML，视图无采样/聚类/统计实现，编图内部和 mapbookqueue 未改。
- GMM 推理拒绝非有限后验；SampleSet 校验位置/深度/投影角、像元网格和类别范围；写出前精确核对 counts、置信度和距离，检查 GDAL 元数据/像元/flush 返回值。
- 手选叠加继承选区外类别、置信度和距离，记录前一 provenance 与标签 hash；凸包仍明确区别于原始凹 lasso。粗层位像元内多道聚合成逐维均值，保证每像元只写一次；记录聚合数、时间轴和测网残差。
- catalog 同井曲线通道 ID 包含 well id；外链 SHA 按路径缓存。任务尚在队列即取消时使用任务真实状态/错误文本，避免未执行结果的默认字段误报；坏源重读清旧成果。
- 控件窄布局、完整 8 类图例、换轴清旧套索经截图核验；点定位实际缩放到井周边 1km / 栅格周边 16 像元窗口，中心和角点一起转地图 CRS。截图保存在 `docs/progress/crossplot-facies-shots/density-3d.png`。

### 七项 Oracle 命令与输出

命令（在本 worktree；所有测试用 add_paleo_test 注册）：

```sh
PALEO_REAL_PROJECT_AREA=/home/kevin/projects/paleo_project/data/project_area \
  PALEO_CROSSPLOT_SCREENSHOT=/tmp/crossplot-panel.png \
  ctest --test-dir build -R '^tst_(cluster|crossplot.*|faciesclassify)$' -V
```

最终专项输出 `/tmp/crossplot-oracle-delivery.log`：**6/6 PASS，4.97s**。

| Oracle | 断言/实测摘要 |
|---|---|
| 1 聚类核 | `tst_cluster` 7 passed：双峰中心误差 <5%、纯度 >.95、k=1 全局均值；空/单/全同值如实返回；两高斯均值/权重误差 <10%，LL 非降、BIC 公式及模型比较；凸包/盒内外边界、凹 lasso 顶点语义、溢出与取消 |
| 2 数据源 | `tst_crossplot_samples` 7 passed：井深对齐/NaN/无外推，12 像元剔2→10，多维输入；属性×时间层位 4道剔1→3，SATR+源测网加载值20/11/2，粗像元均值；真实测网残差 −2m 按既有25m门 |
| 3 UI/联动 | `tst_crossplotpanel` 4 passed、`tst_crossplot_controller` 3 passed：轴切换、实际点点击/框选/套索统计、树/井选择与地图定位；100k 2D投影+绘制 **5.313ms**、3D轴切换+绘制 **13.805ms**（均<200ms）；坏源/取消失败诚实 |
| 4 回写 | `tst_faciesclassify` 7 passed：15有效+1nodata、逐像元counts、palette、DERIVED、hash/父版本、catalog重开、只读拒写；井段0–1/3–3/4–4不跨NaN；手选继承.37置信度、统计篡改拒写 |
| 5 编图 | 上述回写测试调用既有 `deriveFaciesPolygons` 成功；组装壳测试确认编图项 `predict.D53.crossplot` 可见 |
| 6 性能/任务 | `tst_crossplot_perf` 4 passed：100k k8 **14.819ms**（<3s）；1M分类 **135.627ms**+实际TIFF写出 **16.114ms**=**151.742ms**（<10s）；10×样本比率 **9.152<25**；协作取消/队列取消/失败、单调进度断言通过 |
| 7 记账/递延 | 每轮原子提交+本 ledger；`docs/progress/crossplot-facies.md` 记录选型、置信度、操作与全部实测；TODOS 记录有监督/SOM/时深域等递延 |

真工区门控已设置，未跳过：D53/D61/D62 三维栅格 **263446** 有效像元、**5** 联合缺失，与独立 GDAL mask/finite/nodata 交集逐项对拍；采样 **17.254ms**、k8 **442.925ms**。A1 LAS 两曲线 **15272** 有效、**309** 剔除。真数据只读，分类文件落临时目录。

### 全量门禁与既有失败对照

- 首轮全量 173/176；失败为 `tst_import`、`tst_smoke_realdata`、`tst_mapbook`。主 checkout `11099e2` 同环境对照命令 `PALEO_REAL_PROJECT_AREA=/home/kevin/projects/paleo_project/data/project_area ctest --test-dir build -R '^tst_(import|smoke_realdata|mapbook)$' --output-on-failure`，同样 **3/3失败**（55.86s），输出 `/tmp/crossplot-baseline-gates.log`。
- 导入夹具仅扫描原始交付8目录，避免源工程受管哈希副本优先去重；真实文件夹具排除 `project.paleo` 束、参考附件按当前目录计数；核心20井/8层位/1测网和A1四角色仍精确断言。
- mapbook 范围实际 `(0,NaN,1000,NaN)`。用户明确授权「允许这项最小布局修复」后，将 tile/montage 地图范围初始化移到尺寸设置后并用 `zoomToExtent` 完整覆盖；补有限/包含/中心回归断言，取消按钮按闲态禁用/忙态发信号核验。批量队列/编排无修改。专项15断言通过，全量亦通过。
- 既有 PetroPhysPanel 裸 QSS 色值改主题 token，UI 功能不变。全量修复后一次 **176/176 PASS，231.76s**（`/tmp/crossplot-full-ctest-final.log`）。最后定位缩放审查修复后一次全量出现 `tst_panels` 模态输入未回填、`tst_startup_trace` QGIS初始化占比.2256>.2200；独立复验命令 `ctest --test-dir build -R '^tst_(panels|startup_trace)$' --output-on-failure` **2/2 PASS，19.16s**（`/tmp/crossplot-flake-audit.log`），未调阈值或放宽断言。
- 最终代码全量命令 `PALEO_REAL_PROJECT_AREA=/home/kevin/projects/paleo_project/data/project_area ctest --test-dir build --output-on-failure` → **176/176 PASS，529.01s，退出0**（`/tmp/crossplot-ctest-release.log`）；启动比率门、全仓比率门、真工区测试与所有新测试均通过。

### 静态与构建审查

- `cmake --build build -j4` vendored prefix 全目标构建成功（`/tmp/crossplot-build-delivery.log`）；改动项目代码无新 warning，日志警告仅既有 QGIS 头中的 QVariant弃用。无 vendor 修改。
- `python3 tools/check_layering.py --strict` →「检查通过：无层违规。」；`python3 tools/check_i18n.py` →「文案入口无裸字面量」；`python3 tools/check_ui_invariants.py --strict` → `clean (violations=0, baseline=0)`；`git diff --check` → 无输出，均退出0。
- `python3 tools/check_tidy.py --build-dir build` → **11个改动src TU全部OK**（`/tmp/crossplot-tidy-delivery.log`），含 mapbook 最小修复。
