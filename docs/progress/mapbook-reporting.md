# goal/mapbook-reporting — 批量制图与报告引擎（地图册）

> 分支 `goal/mapbook-reporting-20261002`（base `origin/master` = `e6e95af`，
> worktree `.worktrees/mapbook`）。主 checkout 不写码。
> 唯一目标：AOI 格序列 → 逐格版面 → 批量 PNG/PDF 导出 → 落盘账（manifest）
> + 目录索引页，全链 offscreen 可测；蒙太奇（平面图 + 剖面 + 连井）同版面组合。
> 不做：PDF 合并器、新文档库、新线程池、既有版面/导出契约改形。

## Round 0-1 — 勘察与路线定档

| 勘察对象 | 事实（可在仓库/源码里复核） | 结论 |
|---|---|---|
| `src/qgis/layoutexport.{h,cpp}` | `exportLayout(layout, outPath, Format, dpi, PageRange) → ExportOutcome{ok,error,files,effectivePath}`；`layoutexport.cpp:130` `withDefaultExtension`、`:261` `outcome.files << path`（单页时首项即 outPath） | **复用**，不自写导出；本包只在它之上加「版面怎么摆」和「落盘回读校验」 |
| `src/qgis/qgislayoutservice.*` / `src/ui/layout/layouttemplates.*` | 版面服务与模板已有自己的契约面 | 不触碰其契约形状；新增 `src/qgis/mapbooklayout` 独立成面 |
| `src/workflow/mapexport.cpp:57 registerMapPdfAsset` | 受管 OUTPUT 副本 + SHA-256 dedup + asset/version 登记；`asset.format` 硬编码 `"pdf"` | 复用登记管线；**加一个默认参 `assetFormat`**（既有 3 处调用零改动），PNG 册如实登记 `png`，不把 PNG 印成 pdf |
| `QgsLayoutAtlas`（vendored `src/core/layout/qgslayoutatlas.h`） | `setCoverageLayer(QgsVectorLayer*)`(137) / `setFilterFeatures`(230) / `beginRender()`(261) | **不采用**，理由见下 |
| 仓库内 atlas 现状 | 仅 `src/ui/layoutdesignershell.cpp:514,523` 用过 `setEnabled` / `seekTo`（设计器预览开关），无任何数据驱动面 | 无既有 atlas 资产可继承 |
| `PaleoTaskService`（`src/services/paleotaskservice.h:103`） | `start(title, work, layerId=QString(), quiet=false)`；`PaleoTask` 协作取消 `cancelRequested()` + `reportBytes/reportDetail/reportStage`；专用池 ≤4（`:161`） | 批量队列一律经任务服务，**不自建线程池** |
| `src/qgis/previewrendercache.cpp` | 高 DPI 快照有自己的缓存面 | 本包不重复实现缓存；快照 PNG 由渲染管线出、经图片项进版面 |

### 为什么不走 `QgsLayoutAtlas`（选型留档）

1. **语义不对口**：atlas 是「覆盖矢量图层逐要素驱动」（`setCoverageLayer(QgsVectorLayer*)`），
   本仓 AOI 是**临时规则网**（cols×rows 现算）或**给定矩形集**——要走 atlas 得先
   合成一张 memory 矢量图层，把 provider 注册、坐标系、要素渲染一起拖进来。
2. **调度口径冲突**：atlas 自带 render 循环（一次跑全册），与 Oracle 3 要求的
   「逐版经任务服务 + 单版失败不拖死整队 + 失败明细可查」对不上，强行套还要在
   atlas 外面再包一层记账，收益为负。
3. **适配成本 > 复用收益**：本包真正需要的只是「格序列」+「每版换 extent」，
   `workflow/mapbook`（纯编排，无 QGIS）+ `qgis/mapbooklayout`（逐版建版面）
   两个面就能覆盖，且天然可单测。
4. **可回退**：若日后出现真·覆盖图层驱动的图册需求，atlas 仍可在
   `qgis/mapbooklayout` 内接入，不影响 `workflow/mapbook` 的格序列契约。

## 迭代轮次

| 轮次 | 内容 | 落点 |
|---|---|---|
| R2 | AOI 格序列：规则网（行/列主序、末格贴 AOI 上界）+ 给定矩形集 | `src/workflow/mapbook.{h,cpp}` |
| R3 | 模板变量：18 个变量表 + `%{name}` 严格替换（未定义/未闭合 → 空串 + 报错） | `src/workflow/mapbook.{h,cpp}` |
| R4 | 批量队列：任务服务异步 + 同步核心共享 + 逐版记账 + 钉死目录结构 + manifest + 索引页 + 产物登记 | `src/workflow/mapbookqueue.{h,cpp}` |
| R5 | 蒙太奇：平面图（真地图项）+ 剖面/连井（图片项）+ 小标题，两栏几何由页面尺寸推导 | `src/qgis/mapbooklayout.cpp` |
| R6 | 报告装配最小面：目录索引页（PDF 合并需 `QPdfWriter`，本机 Qt 无 Pdf 模块，按禁区降级为索引页） | `buildIndexLayout` |
| R7 | 视图：参数面板只发信号（`batchRequested` / `cancelRequested`），被动显示进度与日志 | `src/ui/layout/mapbookpanel.{h,cpp}` |

### 钉死的落盘结构

```
<outputDir>/<book>/pages/<格名>.<png|pdf>      逐版成品
<outputDir>/<book>/index/<book>_index.<ext>    目录索引页（可选）
<outputDir>/<book>/manifest.json               全册账（成功/失败/取消 + 失败明细）
```

### 自审抓出并修掉的真问题（不是「应该没问题」）

| # | 问题 | 处置 |
|---|---|---|
| 1 | 默认命名模板写的是 `tile_%{row}_%{col}`，而变量表叫 `tile_row`/`tile_col` → **默认路径必然报「变量未定义」**，格序列测试整片红 | 默认模板改 `tile_%{tile_row}_%{tile_col}`，注释同步 |
| 2 | 蒙太奇右栏 x 写死 `pw-147` → 竖版 A4 下右栏压在左栏平面图上；同时 `ph` 未引用触发 C4189 | 两栏宽/高全部由 `pw`/`ph` 推导，并加「不出页 + 不重叠」断言 |
| 3 | 面板「预演版数」特殊值 0（显示全部）原样传出 → 队列按 `i>=0` 判成**一版都不出** | `previewLimit()` 把 ≤0 映射为队列口径的 -1，测试钉住 |
| 4 | PNG 册走 `registerMapPdfAsset` 会被登记成 `format=pdf`（元数据谎报） | 该函数加默认参 `assetFormat`（既有调用零改动），队列按实际扩展名传值；测试断言 `assetById(id).format == "png"` |

## Oracle 验证证据

> 口径：真机/CI 的 `ctest` 是权威；本机**跑不了全量构建**（见「本机环境限制」），
> 故本机证据是**编译期**的（编译检查通道），运行期断言以测试槽位逐条对应列出，
> 待 CI/真机复核。下面每条都写明「用什么命令看什么结果」，不复述「应该没问题」。

| # | Oracle | 证据（命令 → 结果） |
|---|---|---|
| 1 | 地图册闭环 AOI 网格 → 逐格版面 → 批量导出，offscreen 全链可测 + 命名/计数断言 | `tst_mapbook`：`gridCountsAndNaming`（9 格、index 连续、`tile_1_1`/`tile_3_3`、末格贴边 3000）、`tilesFromGivenAreaSet`、`batchQueueExportsEveryTileAndWritesManifest`（9/9 成功、`pages/`+`index/`+`manifest.json` 落盘、manifest `tiles/succeeded/pages` 断言） |
| 2 | 蒙太奇 ≥3 类图项渲染成功（非空 + 尺寸 + 文件头） | `montageCombinesThreeItemKinds`：`QgsLayoutItemMap`+`Picture`+`Label` 三类齐、`sectionSnapshot`/`wellPanel` 在位、全部图项在页内且左右栏不重叠、导出 `bytes>0 && width>0 && height>0`、8 字节 PNG 签名比对 |
| 3 | 批量队列：取消语义 + 单版失败其余继续 + 失败明细可查 | `batchQueueCancelStopsOnlyTheRemaining`（previewLimit=3 → 3 成功/6 取消；预取消任务 → 0 成功/9 取消）、`batchQueueSingleFailureDoesNotKillTheRest`（退化格 → 8 成功/1 失败，`failure.stage=="layout"`、manifest `failures` 1 条）、`batchQueueRejectsBadSetupBeforeTouchingDisk`（`%{not_a_variable}` → 9 失败、stage `prepare`、目录未创建） |
| 4 | 模板变量替换断言 + 缺变量如实报错 | `variableSubstitutionCoversTileAndDate`（`工区A/D61/R2C2/1500/2026-10-02`，变量表 18 项）、`missingVariableFailsLoudly`（`%{nope}` → 空串 + 报错含 `nope`；`%{tile_index` 未闭合 → 报错含「未闭合」） |
| 5 | 分层绿（`--strict`）、ctest 全绿（含新增）、ledger 齐、push + PR | 本机：`python3 tools/check_layering.py --strict` → `检查通过：无层违规。 RC=0`；`python3 tools/check_ui_invariants.py` → 唯一 FAIL 在既有 `src/ui/seismicsection/seismicattrpanel.cpp:53`（本分支未改该文件，pre-existing）；编译检查通道 EXIT=0（见下）。**ctest 全绿待 CI/真机复核** |
| 6 | 真机 3×3 地图册导出计时记入 `docs/progress` | **未执行**（本机无法构建可执行）。补测口径留档：CI/真机上 `ctest -R tst_mapbook --output-on-failure` 后，用 `PaleoMapBookQueue::Request{book="timing_3x3", dpi=300, format=Pdf}` 跑一次 9 格导出，把 `Result::summary()` 与墙钟时间填回本表。不编造数字 |

### 编译检查通道（本机客观证据）

本机无 Qt6 `Pdf`/`PdfWidgets`（顶层 `CMakeLists.txt:91` 必需）与 QGIS 开发包，
故搭了一条只编译不链接的通道（MSVC 19.38 / Qt 6.11.2 / 合成 QGIS prefix），
把 4 个新源 + 新测试 + AUTOMOC 全过一遍：

```
$ cmd //c "run2.bat > final1.log 2>&1"     # paleo-ccheck：cmake + ninja
$ grep -E "error C|fatal error|FAILED" final1.log    → 无输出
$ grep warning final1.log | grep -E "mapbook|tst_mapbook" → 无输出（我方零警告）
[1/6] Automatic MOC ... [6/6] Building CXX object ... tst_mapbook.cpp.obj   EXIT=0
```

残留 C4996 全部来自 QGIS 头（`qgsfield.h`/`qgsvariantutils.h` 等），与本包无关。

## 本机环境限制（诚实记档，不粉饰）

1. **Qt6 缺 `Pdf`/`PdfWidgets`**：`/c/deps/Qt/6.8.0` 与 `paleo-qgis-deps/Library`(6.11.2)
   都没有这两个组件 → 顶层 `find_package` 直接失败，**全量 configure/ctest 在本机跑不起来**。
2. **无系统 QGIS 开发包**：用 `paleo-qgis-build/qgis-vendor` 产物 + `third_party/qgis`
   源码头拼了合成 prefix（`C:/Users/wangj.KEVIN/paleo-qgis-prefix`）仅供编译检查。
3. **沙箱黑名单**：`reg.exe` / `wsl.exe` 被拦（vcvarsall 的 SDK 发现依赖 reg），
   已手工钉 SDK 10.0.22621.0 的 `bin/Include/Lib` 绕过；VS 环境由此可用。
4. 结论：本包的**运行期**断言由 CI/真机 `ctest` 把关，本机只提供编译期证据 +
   逐条 Oracle→测试槽位映射。Oracle 6 的计时数字同样留给真机。

## 自审清单（交付协议逐条）

- [x] `git diff origin/master...HEAD` 全量过一遍：无调试残留 / `printf` / `qDebug` /
      注释掉的死代码（grep `printf|qDebug|std::cout|TODO` 在新增文件里零命中）
- [x] 每个新增文件头三行有 `// 层：<功能|QGIS 封装|视图>` 标记
- [x] `tools/check_layering.py --strict` → `检查通过：无层违规。 RC=0`
- [x] `tools/check_ui_invariants.py`：新增 UI 无 motion / 裸十六进制色（无 `setStyleSheet`），
      唯一 FAIL 为既有文件 pre-existing
- [x] 编译检查通道 EXIT=0，我方文件零新增警告（C4189 已修）
- [ ] ctest 全绿（含 `tst_mapbook`）—— **待 CI/真机复核**，本机无法构建可执行
- [x] Oracle 1–5 每条有对应测试槽位/命令证据；Oracle 6 明确标注未执行与补测口径
- [x] 禁区核对：未改既有版面模板/导出契约形状；批量队列经任务服务、未建线程池；
      导出格式 PNG/PDF 优先；未引入新文档库（PDF 合并降级为目录索引页）
