# Swarm 深度多维 Review：全仓分片逐行审查与修复

日期：2026-10-27。审查基线：`9fdc7bcf`（#790 合入后的 master HEAD）。

## 方法与覆盖

- 20 个并行审查分片（swarm agent）对 `src/` 全仓 33.2 万行 C++ 按「模块 × 8 维度」
  逐行审查：io×5、algorithms×3、workflow×4、services×3、qgis、domain、
  catalog+metadata+linkage、ai、ui×5、tools/CMake/tests 设施×1。
  每片要求：范围内文件逐行读完、每条发现给出 file:line 与代码摘录、
  与 `AUDIT_ISSUES.md`（40 条既有编目）/`TODOS.md`/上轮 holistic review
  （R01–R15）去重、只读不改。
- 账号配额致 8 片中断：3 片（io-segy / wf-constraint / wf-mapping）重试成功；
  其余 5 片（io-wireline 全文、io-welltables 部分全文、io-rest 大部、
  algo-singlefactor 全目录）由主审以 **#788/#790 新代码差异逐行审 +
  缺陷家族模式排查** 补充（见「补查记录」）。
- 全部 P0/P1 发现由主审**逐条读源码复核控制流后才确认**；确认后当日修复
  并过闸门。本文档同时是修复清单与遗留问题登记。

## 核心结论

1. **最系统性缺陷是一个家族：「catalog commit 成功后，失败/取消清理路径把
   已入库（只读在册）的受管产物文件删掉，留下悬垂 catalog 版本」**——
   全仓共确认 **8 例**（Linux 全可触发；Windows 因只读文件删不动而部分
   侥幸无害）。修复口径统一为「入库哨兵旗标 + 清理只删未入库 staging」。
2. 真实崩溃/数据损坏级（P0/P1）共 17 条，全部确认、全部修复；另修复
   高置信 P2 14 条。多数 P1 集中在「失败路径的善后语义」，而非主路径计算。
3. 新代码质量整体偏好：#788/#790 新落地的 welllogtext/welltabletext/
   dlis/lis 有界探测均带预算闸与单位纪律，仅 1 处列预算 off-by-one（已修）。

## 已修复（P1 ×17）

### 家族：commit 后删已入库受管文件（8 例）

| # | 位置 | 机理 | 修法 |
|---|------|------|------|
| F1 | `src/io/dataimport_document.cpp` startNextConversion | mkpath 失败早退 → finishConversion 用**上一单残留的 m_convOutFile** 删掉已入库 DERIVED 产物；m_convTimedOut 残留误报超时 | 出队即清两字段 |
| F2 | `src/io/dataimport_session.cpp:322` + `importrecovery.cpp:79` | `contains(".partial")` 子串误杀合法文件（如 `draft.partial.las`），journal 已入册后整文件被丢 → 版本指向缺失字节 | 后缀判定（.partial / .partial.tif） |
| F3 | `src/workflow/depthconversionworkflow.cpp` commitDepth + cb.cleanup | depth commit 成功后 declare 失败仍删 depth staging；cleanup 无条件删两个 staging（连已入库模型一起） | DepthJob 加 modelPublished/depthPublished 哨兵，fail/cleanup 双守卫 |
| F4 | `src/workflow/compositionworkflow.cpp:582` cb.cleanup | publishArtifacts 成功、declare 失败 → layerId 空 → cleanup 删已发布 gpkg | FaciesPolygonJob.catalogPublished 哨兵 |
| F5 | `src/workflow/registration.cpp:165` | addVersion 成功后 declare 失败仍 discardDerived() 删已入库版本文件（与紧邻 instantiate 失败分支自相矛盾） | declare 失败分支不再回删 |
| F6 | `src/workflow/constraintfactorjobs_factor.cpp:1436` runFactorAlgorithm 外层清理 | publish 半程失败（commit 成功、declare 失败）→ 清理删 staging + qc/structural/support/samples 四件套 | publish 包装以指纹 sha 查册（versionBySha256）置 catalogCommitted，清理只删未入库 |
| F7 | `src/workflow/constraintfactorjobs_factor.cpp:1073` + `_localdirection.cpp:601`（surfer/localdirection 清理孪生） | publishLocalDirectionJob commit 成功后 declare 失败 → 外层清理删已入库 tif+qc+support | LocalDirectionJob.catalogPublished 哨兵（publish 函数 const→非 const 置位） |
| F8 | `src/workflow/constraintfactorjobs_geostat.cpp:1551` | 同型；且 SGS publishSet/deriveStatistics 失败路径发生在 factorGenerated 已发、图层已声明之后 | GeostatJob.catalogPublished 哨兵 |

### 其余 P1（9 条）

| # | 位置 | 问题 | 修法 |
|---|------|------|------|
| P1-1 | `src/algorithms/inversion/lowfreq.cpp:310` | 守卫条件含 `!out`，命中后却向空指针写 `out[i]`（头注承诺「不崩」） | `!out` 单独提前 return |
| P1-2 | `src/catalog/catalogstore.cpp:757` | 主库文件缺失但 `.migrated`/`.bak` 在场时闸在 `schemaLessPrimary` 上短路 → **静默重建空工程**，且后续 rotate 把好数据挤出保留链；只读分支同病（读/写实例同一磁盘状态答案矛盾） | 放宽为 anyRecoverySource 即 recover；readOnly 分支补同款恢复链 |
| P1-3 | `src/ai/chat/llmclient.cpp:522` | 非流式应答 tail 已并入 m_body 后又拼一次（残余字节成双）→ 合法回复判 Malformed | 只传 m_body |
| P1-4 | `src/ui/wellsection/wellsectiondialogs.cpp` rowsMoved | 拖动其他道经过表单所在行时 m_formRow 不平移 → storeTrack 把表单值写进别的道 | remapFormRow 全量区间映射 |
| P1-5 | `src/ui/seismicsection/seismicsectioncanvas_render.cpp` 三路径 | AGC 显示值已无量纲（RMS≈1），绘制仍叠除原始体 absMax → 真实振幅体整幅塌成中间色（密度/LOD/wiggle 三处） | amplitudeBaseScale()：AGC 生效时基准 1 |
| P1-6 | `src/ui/edittools/editingtoolbar.cpp` | 画布先亡路径：destroyed 钩子只 release 高亮带，析构仍 `delete tool`——QgsMapTool 以画布为 QObject 父已随之销毁，其析构又二次释放橡皮带/捕捉标记（TODOS 2026-09-29 决策①同型崩溃实锤） | 钩子内置空 mActiveEditTool，析构只走画布仍在支 |
| P1-7 | `src/workflow/aiassistworkflow.cpp` startClassification | 缺 m_layers 判空（同步入口有）→ 数分钟推理收尾空指针崩溃 | 补守卫（与 classifyTiles 对称） |
| P1-8 | swarm 复核补充：`seismictaskservice_transcode`/`compositionworkflow`/`depthconversionworkflow` 三个 JobRunner cleanup 在 commit 失败后必调（jobrunner.h:372）——F3/F4/F6–F8 的共同契约依据 | — | — |
| P1-9 | 补查（#788 新码）`welllogtext.cpp` split | 末格绕过 kMaxColumns 预算检查（恰超一格漏检） | 末次 append 后补预算判定 |

## 已修复（P2 ×14）

| 位置 | 问题 | 修法 |
|------|------|------|
| `src/metadata/atomicfile.h` paleoSyncDir | Windows 不用 `\\?\` 扩展路径——深路径目录 fsync 恒失败，原子写持久化承诺系统性失效 | 走 paleoWindowsExtendedPath |
| `src/services/seismictaskservice_attr.cpp:1099` | 属性体落名 QFile::rename（目标存在即败）且失败删掉刚写好的缓存——损坏缓存让扫描永久失败；与 ts 路径口径分裂 | paleoReplaceFile |
| `src/services/seismictaskservice_transcode.cpp:609` | 计划外兄弟 LOD 删除先于全部阶段校验——失败/取消时旧金字塔先被拆 | 清理移到校验全过后 |
| `src/io/lascache.{h,cpp}` | CONC-05 复审实锤：petroph 批算/previewdoc 预取已在任务池线程调 load，m_timings 无锁读写竞争 | m_timingsMutex 全程持有 |
| `src/algorithms/velocitymodel.cpp:177` | 井间 IDW 裸 1/d²——BIZ-13 同型（0<d2<1e-12 → ∞/∞=NaN），与已修处口径不一 | d2<1e-12 按命中井位 |
| `src/ai/chat/llmclient.cpp` SSE | 不认裸 CR 行尾（W3C 合法）→ 整轮流不出事件 | \r\n 先替换后孤立 \r |
| `src/ai/remotepredictrouter.cpp` onPredict | 缺 m_running 终态闸（同文件另两处都有）——自定义传输重复回包二次 completed | 补 || !m_running |
| `src/services/pythonenv.cpp:193` | Windows FailedToStart 在 start() 内同步派发 → m_proc 置空后 new QTimer(m_proc) 无父泄漏 + 编排层重入 | errorOccurred 改 QueuedConnection（scriptrunner 同款） |
| `src/workflow/wellsectionworkflow.cpp:601` | lithology 列在而值全空串时 hasMaintainedLithology 误真 + 空 segments 提前 return → 解释资产/岩屑回落全被掐断（岩性道空白） | 空串不算已维护；空段不提前 return |
| `src/workflow/realizationworkflow.cpp:123/897` | 成员栅格 GDALGetRasterBand 不查空（零波段栅格打开成功）→ readBandSlice 解引用空波段（差值路径 1203 已有同款检查） | 两处补波段判空 + GDALClose |
| `tools/check_ui_tokens.py` | 唯一无编码守卫的检查器：GBK locale 裸跑 UnicodeDecodeError 假红（本机复现） | encoding="utf-8" ×4 + stdout reconfigure |
| （自查）`src/io/welllogtext.cpp` | 同 P1-9 | — |

## 复核既有编目的状态更新（不改码）

- ARCH-01/ARCH-05/ARCH-07、MEM-01/02/03/04/06/07*（*07 本轮出新证据已修）、
  BIZ-01..14 中 BIZ-01/02/03/04/05/07/08/09/11/12/13/14、CONC-01/02/04/06、
  RUNTIME-01..04：各片核对为**已修状态**，未发现回归。
- CONC-05（LasCache m_timings）：原裁决前提「消费面全在 GUI 线程」已失效
  （petroph/previewdoc 任务池调用链），本轮按新证据修复。
- ARCH-02/ARCH-06（动态属性走私）：**复发一处新暗道** `paleo.constraint.snapshot.*`
  （mappingworkbench.cpp:312 等 6 处）——登记为遗留 L-01。
- TEST-01（构建目录漂移）：复核 434 个 tst_* 全部注册，无新增漂移。

## 遗留问题登记（确认未修，按优先级）

**P2（应尽快排期）**

- L-01 `mappingworkbench*.cpp` 约束快照动态属性暗道（ARCH-02/06 复发，6 处）→ 改 typed 访问器。
- L-02 `paleotaskservice.cpp:291/378` 层忙注册表与编辑会话共用键空间：markLayerBusy 覆盖/markLayerFree 无条件释放可击穿 §35 编辑门 → 改 releaseLayerBusy(layerId, taskId)。
- L-03 `src/ai/tileinference.cpp:545` classes×pixels 乘积无闸（64Mi 像元×255 类 ≈65GB）→ bad_alloc 终止任务线程。
- L-04 PW-128 同族 double→int UB 残留 5 处：stratgrid.cpp:197、upscale.cpp:87、wellsiting.cpp:218、geostat/faultpath.cpp:20/27、sgs3.cpp:203（x86 多静默错格）。
- L-05 `sgs3.cpp:68` 3D 去重只比 back()——PW-078 的 2D 修复未同步 3D。
- L-06 `geostat.cpp:385` cols/rows int 溢出绕过 4M 预算闸（极小 cellSize）。
- L-07 XML 实体扩展无防护（billion laughs）：previewdoc_xml / outsourceworkbook / welllogtext_xml 三处 QXmlStreamReader 均未设实体拒收——导入第三方文件面。
- L-08 `previewdoc_xml.cpp:24` SpreadsheetML 单格文本无截断（通用分支 4096 口径未覆盖）。
- L-09 `faciespolygonize.cpp:1514` CONSTRAINTS 层 CRS 不变换即消费（姊妹算法都有变换+校验）；`:1424` 无像元预算闸（12B/像元多份拷贝可 OOM）。
- L-10 `mappingartifactwriter.cpp:121` GTiff 原地重写非原子（#588 B.13 同族未同步）。
- L-11 `evidencesynthesis.cpp:89`、`faciesqa.cpp:192/395` GEOS 谓词无 bbox 预筛/PreparedGeometry——QA 热路径分钟级。
- L-12 `wellattributestore.cpp:770` seedFactors O(U×N) 全表线性扫（GUI 线程事务内）。
- L-13 `realizationworkflow.cpp:754` busy 拒绝返回 nullptr → UI 兜底**同步重跑**冻结 GUI；wellsiting saveScenario、attach.cpp:299 面运算、mappingevidencereview changeEvidence 同族 GUI 同步重活。
- L-14 `petrophyscomputeservice.cpp:1381` 产物文件名查重在写队列外——并发批互覆产物并错配登记指纹。
- L-15 `segyindexstore.cpp:244` loadForResume 缺 load() 同款 checkpoint 审计（损坏 checkpoint 可固化重复道）。
- L-16 `deviationsurvey.cpp:208` 掉头段极值方向按端点净差符号（浮点噪声）选择——tvdToMd「首次到达」可违（触发面窄，合成数据可构造）。
- L-17 `catalogstore` 只读实例与写实例恢复语义（本轮已对齐主路径；`recover()` 写 .read.tmp 临时文件的行为对真只读介质仍需确认）。
- L-18 ui/pages `datanavtree.h:334` 非法拖放红反馈永不清除；`dataopspanelextra.h:73` 受管版本 stat 恒缺（三元两分支相同）；`dataopsimportui.h:238` 全部取消后 1.5s 定时器永不停。
- L-19 `seismicsection` setColorMap/setColorMapInverted 不重烘卷帘 B 图（帘右色标陈旧）；computeAttributeOnCurrentSection 无在途去重（旧参数结果覆盖新结果并可登记）。
- L-20 AGC 生效时色标条仍标原始 ±absMax（本轮修了渲染路径，标签语义需 DESIGN 决策）。
- L-21 本机 Windows ctest 夹具环境全红（QTemporaryDir 在所有测试进程内 isValid()
  为假、QGIS AppData 库只读、离屏渲染对比失败）——与 *_styles.db 落 CWD 同族
  （TEMP/沙箱解析），CI linux 不复现；需专项修本机开发环。

**P3（择机）**：paleoprojectfile 死拷贝 georeferenceToJsonImpl 与 basemap 成员漏校验、atomicfile.h PaleoAtomicTransaction 生产零调用、litholexicon/arearules/sequenceframework JSON 形状错静默回落、sectionworkbench m_logs 无界缓存、aichatcontroller 工具帧重复 id、faciesmappingworkflow 草稿图血缘断链、wellFactorFields 吞错、trackops CSV 无转义、exportengine PDF 横向不分页、ft 单位只换词、trackcompound 地层表视图层硬编码（与 chronostratcolors 重复两份）、fencewidget 视图编排/直写库（R14 同族新站点）、sequenceframeworkpanel 报告落 CWD、showStartup 重置只读态、horizonReleased 立即重实例化、seismicslicerenderer int 截断阶梯、pythonenv PALEO_PYTHON 静默忽略、seismicmapping 非稳定去重、dataexport error 空指针防护漏三分支、previewwellbases 4×全量 IO、sourceprobe O(k²) 重识别、streaming QRegularExpression 热路径逐行编译 + GeoJSON Latin-1 键、sfpkgwriter f4 收窄 Inf、segyreader probe IO 混淆 + cancel 并发契约、segyindexstore span int 溢出、transcode Save 错误吞 + colMax 死参、interp 吞 runner 错误、startBounded taskService_ 判空、lowfreq 类外 velocitymodel 同型已修、remotepredictrouter health "ok" 子串判定（MAMCL 应答格式未确证，收紧有回归风险暂缓）、tileinference pIn 逐像元分配与 argmax 越类号、layoutexport 旋转栅格 extent、wellattributestore 内部槽签名重接、qgis 模块清单硬编码/check_layering qgis.h+Qgis:: 盲区（evidencerecord.cpp:9、paleomainwindow_ribbon.cpp:22 已在册外）、new_module.sh python3、基线头注计数漂移。

## 设施专项：仓库根/build 的 *_styles.db 垃圾文件根因（已定位，未修）

QGIS 4.2 `QgsProject` 默认启用 ProjectStyles 能力，`new QgsProject()`/读 .qgz 经
`createAttachedFile("styles.db")` 生成 `XXXXXX_styles.db`（autoRemove=false）；
当进程 TEMP 解析到不可写位置时 QTemporaryDir 失败返回空路径，文件以相对名落
CWD。Windows ctest 沙箱 env 恰不含 TEMP/TMP（cmake/paleo-test-infra.cmake:55-65），
故 build/ 累积 844 个、仓库根 64 个。修法（建议另开 PR）：① Windows 沙箱补
短路径 TEMP/TMP；② 不用工程样式库的 `QgsProject` 构造时关掉 ProjectStyles
能力（qgisprojectservice.cpp:59 + 各测试）；③ QGIS 启用入口统一设
QGIS_CUSTOM_CONFIG_PATH 入沙箱；④ 清理现存垃圾。

## 补查记录（配额失败片的覆盖声明）

- **逐行全文补查**：welllogtext{,_convert,_xml}.cpp、welltabletext.cpp（#788/#790
  新码全量）；dlisparser/lisparser/outsourceworkbook 的 #790 差异逐行（均为带
  预算的有界探测重构，质量好；zip 预检有 4MB/128 项预算，但 provided-archive
  路径对「声明尺寸造假」的 zip 炸弹防御未确证——登记待核）。
- **未做全文逐行**（留待下一轮或后续 review）：dlisparser/lisparser/lasparser
  主体（既有 BIZ-01..05 深审背书 + MEM 无涉）、outsourceworkbook 主体（#788
  契约面已经两片交叉审）、io-rest 之 constraintstore（MEM-01 已修背书）/
  ziparchive/rasterpyramid/horizonbinner/sattrio/cuttingsdoc、
  **algorithms/singlefactor 全目录**（BIZ-11 在册）。

## 第二轮（同日）：补全覆盖 + 遗留推进

上轮因账号配额中断的 4 个分片本轮补齐（dlis/lis/las 全文、outsourceworkbook
主体 + wellcompositexml + wellfileparsers、io-rest 之 constraintstore/ziparchive/
rasterpyramid/horizonbinner/sattrio/cuttingsdoc、algorithms/singlefactor 全目录
41 文件）——**src/ 全仓逐行覆盖至此完备**。同时对第一轮遗留清单继续修复。

### 第二轮新发现并已修复

- **[P1] dlisparser.cpp** EFLR 行区出现第二个 Set 族描述符（损坏文件一位翻转
  即可构造）→ 内层 break 不消费字节、外层 while 零消耗空转（100% CPU 挂起）；
  原「第二 Set」守卫在循环外不可达。就地拒绝并如实报错。
- **[P1] lisparser.cpp** 填充前瞻把 len∈{0x0020,0x2000,0x2020} 的合法 PR 头
  （LIS79 常用 8192B 物理记录）成对判为填充吞掉 → 整文件误报「PR 长度非法」。
  跳过前先做 PRH 先验（len ≥ attrs 最小长度、不越文件尾、attrs 非纯填充形态）。
- **[P2×2] singlefactor**（补覆盖新位点）：support.cpp cellOf / curvekernel 核点数 /
  partition lround / wellacquisition toInt / contouravoidance steps / corridor cells
  六处 PW-128 同型 double→int UB 全部改 double 域判界/夹紧；structural.cpp 补
  B-11 同款 20000 井闸（该路径 params 手工填绕过 resolveParameters）与空几何
  GEOSisEmpty 检查（empty→NaN 换算 UB）；localidw K=0 全样本模式按 512MiB
  矩阵预算封顶邻域数（原 20000 井 = 3.2GB bad_alloc 打穿任务线程）。
- **[P2] horizonbinner writeHorizonGeoTiff** 直接 GDALCreate 目标路径 → 改
  .partial + paleoReplaceFile 原子落名（#588 B.13 家族 io 侧实例）。
- **[P3×5] io-rest 分片**：ziparchive 中央目录条目数前置闸（10 万条封顶，堵
  「先建 GB 级条目结构才轮到后置闸」的纵深缺口）；outsourceworkbook
  previewXlsxWorkbook 三处 `*limited` 空指针防护 + 500/512MB 双标死区对齐。

### 第一轮遗留项本轮修复（L 清单收敛）

L-03（tileinference 类数×像元预算）、L-04（PW-128 五处：stratgrid/upscale/
wellsiting/faultpath×2/sgs3 bucketAxis）、L-05（sgs3 3D 去重窗）、L-06（geostat
网格 double 域夹紧）、L-08（SpreadsheetML 预览单格 4096 截断 + 64 表上限）、
L-12（wellattributestore 一次快照哈希索引替代 O(U×N) 全表扫）、L-15
（segyindexstore loadForResume 补 D2.8 审计）、L-18（datanavtree 红反馈记账
清除 / 版本时间线受管版本按工程目录解析后 stat / 导入队列取消到空即停驱动拍）、
L-19a（卷帘 B 图随换色标/反转重烘）。另修 P3：streaming 热路径静态正则、
check_file_budget 逐字符空转死代码、new_module.sh python3 探测兜底、
seismictaskservice startBounded 判空、transcode 索引缓存落盘失败留痕。

### 第二轮确认、暂不修（并入遗留清单）

- **L-22 [P2] wellcompositexml 写侧只序列化 6 类工作表**（测井曲线/离散曲线/
  砂层组/层序/取心/符号道无写回路径）——含这些域的派生版本落盘后重解析永久
  丢失；需补齐 writer 或显式契约拒绝，属行为设计决策。
- **L-23 [P2] dlisparser** 主帧含 UVARI/ORIGIN 表示码标量通道时整文件硬失败
  （readNumeric 本可解码，违反自家「白名单外默认必须解析」契约）。
- **L-24 [P2] lasparser** 三处：行数外推 reserve 可放大为每曲线 512MB 提交
  （Windows 大块堆分配即 MEM_COMMIT）；4MB 头扫描上限使 parseRange 与
  parseDoc 对同一文件结论矛盾（错误归因「无 ~A」实为扫描截断）；流式路径
  Ctrl-Z 只截断所在块，与整读路径行集不一致（#167 语义分叉）。
- **L-25 [P3] rasterpyramid tile()** 源新鲜度校验在 buildMutex 外、锁内无复验
  ——并发换源可把错代瓦片灌进 LRU（PW-106「进锁双检」未同步到 tile()）。
- **L-26 [P3] constraintstore**：copyAudit OFFSET 分页对并发追加不稳定可漏审计
  事件（应游标翻页）；migrateLegacyCrs 备份复制先于事务写锁（延迟 BEGIN），
  极窄窗口内外部合法提交可能被旧备份回滚。
- **L-27 [P3] 综合柱状图解析语义**：单元格级 issue 升格整文件拒收（底层契约
  是逐条列因照常返回）；坏深度格整行静默丢弃零报告；writeWellTopsText 不加
  引号破坏「parse(write(x)) 恒等」契约；sharedStrings 全量物化 ~2GiB 峰值；
  collectSheetRows 末张匹配表覆盖 + 不查 XML 截断错误。
- **L-28 [P3] singlefactor 性能族**：fieldcontours 网格加密线数无预算（数据相
  关可达 10^6 GEOS 线）；regionalcontours O(n²) 缝合；cartographicsmooth
  rdpSimplify 递归深度 O(n)；faultpath 逐查询点重建绕行数组 + O(井×段) 扫描；
  dlisparser 每帧每通道 obnameKey 键串分配；lasparser 流式逐行堆分配。
- lasparser.h 头注释「WRAP YES not supported」与实现矛盾（已实装）——文档漂移。

## 第三轮（同日）：P2 遗留主攻

### 已修复

- **L-01**：约束快照 `paleo.constraint.snapshot.*` 动态属性暗道（ARCH-02/06
  复发）全量转 typed 通道——实际调用点比首轮简报多（12 处字符串/14 个使用
  点：含 constraintfactorjobs_{factor,contours,interpretive,localdirection}
  的 7 处 property() 读），不一并转换会让写入移入 QHash 后读到空。
  `setConstraintSnapshotPath/constraintSnapshotPath/clearConstraintSnapshots`
  落在 ConstraintWorkflow；空串=移除，与旧 QVariant() 移除语义在读点
  （均判 isEmpty）不可区分。
- **L-02**：层忙注册表改多持有者——store 侧 `m_busy` 从单槽
  `layerId→(taskId,reason)` 改 `layerId→[(taskId,reason)]`，mark 幂等替换同
  taskId 条目、releaseLayerBusy 只摘自己；qgiseditingservice 四处终态与
  paleotaskservice 终态全部改按 taskId 条件释放——编辑会话与后台任务共用
  键空间时任一方终态不再连他人 §35 门持有一并抹掉。
- **L-07**：XML 实体防护（billion laughs）——SpreadsheetML 流式读面与
  预览通用 XML 分支按 `QXmlStreamReader::DTD` 令牌拒收（任意位置可命中，
  不依赖头部嗅探；合法 SpreadsheetML 不带 DTD）。
- **L-09**：faciespolygonize 约束层 CRS 变换 + 有效性前置（distancetransform
  同模板，BIZ-10 口径）与 1 亿像元预算闸（raw/grid/next/maskPx 平峰
  ≈13B/像元）。
- **L-10**：mappingartifactwriter 预测栅格改 .partial + paleoReplaceFile
  原子落名（#588 B.13 家族 qgis 侧实例）。
- **L-23**：DLIS 主帧 UVARI/ORIGIN 标量通道不再整文件失败（readNumeric 本可
  解码，违反「白名单外默认必须解析」契约）；多维时按逐元素 uvari() 走位跳过。
- **L-24**：lasparser 三处——外推 reserve 按全部桶合计 ≤1GiB 封顶（Windows
  大块堆分配即 MEM_COMMIT，原口径可放大数十 GB 提交）；4MB 头扫描截断与
  「真没有 ~C/~A」错误分归因（此前与 parseDoc 对同一文件结论矛盾）；流式
  路径补 Ctrl-Z 截停（此前完全不认，与整读路径行集不一致——比首轮简报
  所述「只截断所在块」更严重）。
- **L-25**：rasterpyramid tile() 锁内新鲜度复验（ensure 同款双检）——锁外
  检查通过后等锁期间并发换源时，不再以旧 meta 对新源读瓦并把错代瓦片
  灌进 LRU。
- P3 十一项：showStartup 只读态现取 store 真源（不再无条件重置）、3D 贴面
  线号 NormalizeF（去整数吸附台阶）、sfpkgwriter f4 收窄 Inf 拒写、
  dataexport companions 三处 *error 判空、pythonenv PALEO_PYTHON 失效
  qWarning（不再静默换解释器）、tileinference argmax 按实际类数设界 +
  pIn 行外复用、layoutexport 旋转栅格拒算（走 extent 兜底）、
  paleoprojectfile 死拷贝 georeferenceToJsonImpl 删除 + basemap 两成员
  并入 PW-075 路径校验。

### 仍未修（维持登记）

L-11（GEOS 热路径预筛）、L-13（GUI 线程同步重算族）、L-14（petrophys 文件名
竞态）、L-16（deviationsurvey 掉头段极值方向）、L-17（recover 只读介质行为）、
L-20（AGC 色标标签语义，需 DESIGN 裁决）、L-21（本机 ctest 夹具环境）、L-22
（wellcompositexml 六域写侧缺口，需设计决策）、L-26/L-27/L-28（constraintstore
分页/备份时序、综合柱状图解析语义族、singlefactor 性能族）。

## 第四轮（同日）：竞态/性能/闸门盲区

### 已修复

- **L-14**：petrophys 产物文件名查重挪进单写者队列内（enqueueWrite 同步互斥
  执行）——两个同 (井,公式,输出名) 批次并发在飞时不再互覆产物、先完成批
  的 sha 登记不再张冠李戴。
- **L-13a**：realization 统计/差值的 async 入口被 busy 拒绝时，壳层不再兜底
  同步重跑（旧兜底 = GUI 冻结 + 与在飞任务并行同算 → 同 token 双版本），
  busy 只提示并解除面板忙态。
- **L-11**：三处 GEOS 热路径加包围盒预筛（四次 double 比较挡掉不相交对再
  进 GEOS）——evidencesynthesis 证据投票（单元×井点）、faciesqa 两两重叠
  与整合接触检测（单元×单元）。语义不变（包围盒不相交 = 几何必不相交）。
- **L-26a**：constraintstore copyAudit 分页从 OFFSET 改 fid 游标翻页
  （readHistory/historyForHorizon 增带默认参数的游标面，既有调用零改动）——
  导出快照与编辑并发时不再整页漏审计事件。
- **闸门盲区**：`<qgis.h>` 与 `Qis::` 此前对 QGS_HEADER_RE/QGS_SYMBOL_RE
  双双隐形——两检查器正则补口径后浮出 3 个存量直连文件
  （evidencerecord.cpp、paleomainwindow_ribbon/_seismic.cpp），按 #539
  盲区扩面先例入册两基线（139/174）并头注归因。
- P3 五项：aichat 同帧重复 tool_call id 去重（原违反「每 call 恰好一答」
  协议致每轮 400）、sectionworkbench LAS 缓存 64 条上限逐出（原无界）、
  trackops CSV 单元格转义（层名/岩性名/结论文本含逗号引号换行不再错位）、
  previewwellbases 双份全量哈希改单次后置验身、interp 追踪核失败 qWarning
  留痕（原被 Q_UNUSED 吞掉谎报「部分覆盖」）。

### 验证

闸门全绿（layering --strict / qgs-audit 139=基线 / qgis-spread 174=基线 /
file-budget 回升 0 / i18n / ui_tokens）；MSVC 全量构建通过。

### 仍未修（维持登记）

L-11 的 GEOSPreparedGeometry 进一步提速、L-13b/c/d（wellsiting saveScenario
同步评估、attach 面运算任务化、mappingevidencereview 分块）、L-16
（deviationsurvey 掉头段极值方向）、L-17、L-20（DESIGN 裁决）、L-21（本机
ctest 环境；Defender 事件日志无拦截记录，EDR 假设未证实）、L-22（设计决策）、
L-26b（migrateLegacyCrs 备份先于写锁）、L-27 其余（单元格级 issue 升格/
坏行静默丢弃/sharedStrings 峰值）、L-28（singlefactor 性能族）、
faciesmappingworkflow 草稿图血缘 parentPaths（调用方管线未携带源资产路径，
需上游配合）、paleomainwindow horizonReleased 重实例化、fencewidget/
trackcompound/sequenceframeworkpanel 的 R14 同族视图越界、ft 单位换算、
exportengine PDF 横向分页、checker 模块清单硬编码、基线头注计数漂移。

## 验证

- 源码闸门（本机直跑）：layering --strict / --qgs-audit / qgis-spread / file-budget
  （修复中两处函数体/文件规模回升，已抽 helper 与收敛注释压回冻结值）/ ui_invariants /
  ui_size / ui_tokens（编码修复后在 GBK 本机裸跑通过）/ i18n / docs_drift /
  cpp_mainline——**全绿**。
- 构建：MSVC 2022 + Ninja 全量 1844 目标**全绿**（中途 paleo_seismic_demo.exe 一次
  link.exe Access violation 为增量链接偶发崩溃，重试即过，该目标不含本次改动）。
- 本机 Windows ctest：61 个触面测试 16 过 45 红已判定为**本机夹具环境既有问题**，
  与本次改动无关——证据：①全部失败签名为夹具级（`QTemporaryDir::isValid()`/
  `writeTestSegy`/`exportPng`/QGIS AppData 只读库/离屏渲染对比），未触及任何
  修改路径的断言；②`git stash` 基线（无本次改动）重建后运行同一测试失败数与
  签名**逐一相同**（tst_metadata_atomicfile 基线同样 3 过 15 红）；③未改动模块
  （tst_constraintstore/tst_pythonenv/tst_test_sandbox 等）同样全红；④CI linux
  junit（artifacts/review-647）同一测试面绿（仅 tst_panels/tst_shell_chrome 两个
  已知离屏红）。本机直跑 `tst_wellsection_ui::dialogs()`（本轮修改的道配置对话框
  的直接用例）**通过**。本机 Windows ctest 夹具全红的根因（疑似 TEMP 解析/杀软
  拦截一族，与 styles.db 根因同族）登记为 L-21 待专项排查。
- 修复中两处为过 file-budget 冻结闸做的等价收敛（editingtoolbar 注释并入既有行、
  wellsectiondialogs 抽 remapFormRow 匿名 helper），无行为差异。
