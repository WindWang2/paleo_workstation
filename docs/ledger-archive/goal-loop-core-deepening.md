# goal-loop ledger — core-deepening（任务 A：现有业务内核深化）

- 分支：`goal/core-deepening-20261012`（基线 `origin/master` @ `d230a5e7`，2026-10-12）
- worktree：`.worktrees/core-deepening-20261012`，独立 `build/`，共享 vendored prefix symlink
- 职责图（本任务写）：`src/domain/ src/catalog/ src/metadata/ src/services/ src/algorithms/` + 确认归属的
  `src/workflow/`（mappingworkbench/appcontext 拆分）+ 必要 `src/app/`（appcontext）+ 新测试 `tests/`
- 只读（任务 B / 他人所有）：`src/ui/**`、`src/qgis/**`、样式、图层树与页面交互
- 资源纪律：编译 `flock -x /tmp/paleo_workstation_compile.lock … -j4`；测试 `ctest -j2`

## EXCLUDED_EXISTING_ISSUES（71 个 open issue 排除指纹，2026-10-12 实查）

任何与下列同文件+同函数/行为签名的修复直接跳过。全量正文快照 `/tmp/paleo-issues/*.md`。

### 直接落在本任务目标目录的 issue（最高优先排除）
| Issue | 文件:行 | 函数/行为 |
|---|---|---|
| #415 | workflow/mappingworkbench.cpp:1337-1340 | mergeIntervals 成功但 mergeFormations 失败的半更新域状态 |
| #415 | workflow/mappingworkflow.cpp:608-612,72 | 井点 IDW 兜底 cols/rows 无 cell 预算 |
| #415 | workflow/batchjobqueue.cpp:857-865 等 3 处 | startOnSlot null-task 重入队不 pump；retryFailed 熔断（#377/#378 同族） |
| #415 | workflow/constraintfactorjobs.cpp:248-251 | commit 回调丢 publish 失败原因 |
| #415 | workflow/faciesmappingworkflow.cpp:501-507 | commitComputed 失败 lambda 空 computed 崩 |
| #415 | workflow/depthconversionworkflow.cpp:65,331 | cols*rows 无溢出闸 |
| #415 | workflow/propertymodelworkflow.cpp:317-321 | WKT 环闭合自引用 |
| #415 | workflow/sectionworkbench.cpp:28,42-48 + projectlayerrefresh.cpp:47,57-61 | 合并请求后 finished 无条件再请求 |
| #415 | workflow/mapversioncontroller.cpp / evolutionworkflow.cpp / aiassistworkflow.cpp / officepreviewsession.cpp | 见 issue 正文 |
| #416 | services/paleotaskservice.cpp:218 | shutdown() clear() 丢弃排队 runnable 无终态 |
| #416 | services/seismictaskservice_slice.cpp:427-439 | coherence 分支几何校验不对称 |
| #416 | services/seismictaskservice_interp.cpp | 见 issue 正文 |
| #416 | services/crashreport.cpp:387-419 | ≥4096 字节路径 clearRunningFlag 早退 |
| #416 | services/previewdoc.cpp / petrophyscomputeservice.cpp / jobrunner.h | 见 issue 正文 |
| #417 | algorithms/singlefactor/structural.cpp:451,354 | 取消路径 GEOSPreparedGeom 泄漏 |
| #417 | algorithms/singlefactor/fieldcontours.cpp:270-274,353 | 三角化失败不 destroy preparedDomain |
| #417 | algorithms/singlefactor/support.cpp:376-380 | BarrierGrid 空 component 越界读 |
| #417 | algorithms/inversion/lowfreq.cpp:358-367,230-234 | tops[64] 无 nL>64 守卫 |
| #417 | algorithms/seismicattr.cpp:341-349 | semblanceCoherenceWeighted 先乘后校验 UB |
| #417 | algorithms/geostat/cokriging.cpp:615 vs .h:157-158 | usedSamples 契约矛盾 |
| #417 | algorithms/geostat/faultpath.cpp:63 | pointInRing 空环回绕 |
| #417 | algorithms/gridsolver.cpp:761-780 | crossValidate 不过滤非有限 z |
| #418 | domain/arearules.cpp:80 | toDouble 范围检查前 static_cast<int> UB |
| #418 | app/crossplotcontroller.cpp:24-25 | bottomTabs null 时 CrossplotPanel 泄漏 |
| #351 | metadata/faultsetstore.{h,cpp} | 换工程后断层投绘 UAF（FaultSetStore 悬垂） |
| #395 | metadata/metastore.h / mapversionstore.cpp / faultsetstore.cpp | 三 store 读路径 read-write 开 project.sqlite |
| #394 | metadata/mapversionstore.cpp | publish() 缺只读守卫 |

### 其余 issue（按模块归类，均排除）
- io（#381 ziparchive、#382 sfpkgreader、#383 partialread、#384 dlisparser、#385 lisparser、#386 lasparser、#387 dataimport_file/geodata、#391 lascache/segyreader、#414 低危清单 5 项、#354 segyreader resumeScan）
- qgis（#355 qgiscanvascontroller、#352/#371 workflow 侧守卫、#419 低危清单 5 项）
- ui（#356-#390、#362-#376、#411-#413 低危清单等，全部任务 B / 他人领域）
- 测试夹具（#356-#360、#397-#408、#410、#421）
- ai（#402 onnxpredictionservice）
- selfcheck（perfgroup.cpp）
- #393 FaultPathMetric 预算（= TODOS 递延专题）、#389 petrophys 合并路径单调校验、#388 exportPicksCsv

### open PR 占用（不得重复/冲突）
- #422 goal/ci-green-20261012：domain/faciescatalog.{h,cpp}、ui/attributetablepanel.cpp、tests/{tst_mappingworkbench,tst_ui,tst_attrpanel,tst_sanitizer_budget}.cpp、CMakeLists.txt、tools/ui-token-*
- #423 goal/master-tail-20261012（基于 #422）：同上 + tst_singlefactor_asynccontour.cpp
- #424 codex/data-file-graph：src/services/datagraph*、src/ui/{datapreview,pages,paleomainwindow_attach_data,ribbonpanels}、src/workflow/datagraphworkflow*

结论：**`src/catalog/` 全目录无 issue/PR 占用**；`src/app/appcontext.cpp` 无占用；
`mappingworkbench.cpp` 仅 :1337-1340 排除（拆分时该函数保持原样平移）。

## 基线
- origin/master HEAD `d230a5e7`（= 本任务基线；任务书快照一致）
- 构建：worktree build/，RelWithDebInfo，QGIS_PREFIX=vendor/superbuild/prefix（symlink）
- [进行中] 基线构建 + ctest -N 清单 + 相关基线测试 → 记录已知红项（#422 已列 master 红：tst_mappingworkbench SEGFAULT / tst_ui / tst_attrpanel / contour 链接 / lint 形参名）

## 轮次记录

### 轮 0 基线（2026-10-12）
- `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DQGIS_PREFIX=<vendor>/superbuild/prefix`（二次 configure 补 vendor include，BUILDING.md 已知项）
- `flock -x /tmp/paleo_workstation_compile.lock cmake --build build --target paleo -j4` → exit 0
- 已知 master 红（#422 记档，非本分支引入）：tst_mappingworkbench SEGFAULT（#346 scheduleRefresh 测试未等刷新）、tst_ui/tst_attrpanel、tst_singlefactor_contours 链接、lint formationPrediction 形参名——本分支不修（#422/#423 领地），跑测时如实比对。

### 轮 1 A-R1 catalog 契约（实现 → 测试分提）
实现（先于测试提交）：
- frameworkstore.cpp：save 全部失败路径 batch.abort() + 孤儿文件清理（managedPath/mkpath/写失败/sha 失败/addVersion 失败/addLink 失败六口）——旧行为经 BatchSave 析构 flush 提交「零版本幽灵资产/半截链接」。
- catalogstore_json.cpp：跳过的坏段版本行先吸收 ver-N 序号（防 nextVersionId 重发 → upsert 覆盖坏行 → 旧 ver-N 血缘被静默改接）；mapFromJson/parentsFromJson/cornersFromJsonText 非空解析失败 qWarning（写侧恒为对象/数组或空，非此即损坏）。
- datacatalog_versions.cpp：updateVersionExtra(map) 与单键重载同口径拒空键/'#' 保留字（防批量通道污染 #history/#source 审计面）。
测试：tests/tst_catalog_integrity.cpp（新，5 用例：失败无幽灵/失败后恢复/序号吸收/损坏告警/保留字拒收）。

### 轮 2 A-R4 派生登记契约
实现：
- derivedassets.{h,cpp}：stage 由「find-or-create」改「find-or-reserve」——新资产 addAsset 延迟到 commit；同注册器同名复用预留 id；commit 期发现外部已建同名资产 → 迁文件归并（versionNumber 按既有资产口径重算、指纹按新路径解释）。取消/失败不再留零版本资产（propertymodelworkflow:1071 注释声明的契约成为事实）。
  - 行为边界（记档）：成功路径最终态与旧实现逐字段一致（同资产 id/版本号/revision 计数）；唯一 delta = 「同名两次 stage 且都 commit」的并发窗口由「同资产两版本」变「归并到先建资产」——测试 commitMergesIntoAssetCreatedMeanwhile 钉住归并语义。
- mapexport.cpp：addAsset 失败清受管副本；addVersion 失败 removeAsset 回滚 + 清副本（零版本资产无血缘引用，removeAsset 不被拦）。
测试：tst_derivedassets.cpp 追加 3 用例（abandonedStageLeavesNoAsset / rerunAfterAbandonedStageYieldsSingleAsset / commitMergesIntoAssetCreatedMeanwhile）；既有 7 用例零改动预期全绿（stage 字段/复用递增/sha/missing-file 失败均保持）。

### 轮 3 A-R3 性能（等价优化）
- constraintwellfactors.cpp：LithoLexicon::fromProject 提出逐井循环（W 口井 W×读盘+解析+词表重建 → 1×）；词表为循环不变量，输出逐字段等价。
- litholexicon.cpp：defaultLexicon 函数级 static + COW（30 条 addRule 只构造一次；QVector 不可变共享跨线程安全）。
- realizationset.cpp：statSurfaces 排序比较器 versionById×2/比较（哈希查+CatalogVersion 含 QVariantMap 按值深拷贝）→ 预取 QHash 一次；全序由既有 tst_realizationset::statistics* 用例钉死。
- 验证：tst_factorworkflow（井因素行为）+ tst_realizationset（全序）+ 计量（见终验记录）。
- 否决项：entityview 逐种子 BFS 合并会改 derivedProducts 发现序（顺序=行为），仅做 contains QSet 化收益太小——整项放弃，记档。

### 轮 4 A-R2 行为守恒拆分
- mappingworkbench.cpp 2408 → 主 TU 943 + _predict 844 + _edit 575 + _compose 277（函数体逐字平移，脚本切分+verbatim 断言；finishPrediction 含 #415 领地 merge* 块原样平移验证 True）。
  - 主 TU 匿名助手裁剪：uid/isLocalGridCrs/acceptsLocalGrid/wellAttributeUpdates 随所属函数移出（主 TU 剩 fail/filePath/writeJson 均仍有调用）；QUuid include 移除。
  - 卫星 TU 各持私有副本（_polygonize.cpp 先例）；helpers_common={fail,uid,filePath}，edit 另+crs 门+wau，predict 另+wau。
- appcontext.cpp 1024 → 主 TU 847 + _layers 192（restoreProjectLayers + refresh{Wells,WellTrajectories,Survey}Layer——projectOpened 顺序敏感段按方向 83 注记留在主 TU）。
- include 面：卫星 TU 用裁剪清单；CMakeLists paleo_workflow/paleo_app 各 +3/+1 源。
- 对拍：函数级 verbatim 断言（切分脚本内嵌）+ 伞式 tst_mappingworkbench 拆分前后 PASS 集合逐项一致（见轮 7）。

### 轮 5 A5 独立 review + 修复
- A5（未参与编写的子 Agent）逐文件 diff-to-base review：拆分保真 47 函数逐字节
  ALL-IDENTICAL、helper 齐全、C/D/E/F/G 面核实通过。
- **P1（已修）**：预留态只在 registrar 私有成员 → 任务中心 prepare（局部
  registrar stage 后消亡）/publish（全新实例 commit）跨实例结算被跳过，首算
  single_factor_raster 必败（constraintfactorjobs_localdirection.cpp:172→380
  /_geostat.cpp:248→863 生产形态）。修复 = 预留三字段进 DerivedStaging 本体
  （21eb39a4 后续修复提交）+ 回归用例 crossRegistrarCommitSettlesReservation
  （13/13 绿）。
- **P2（已修）**：结算 addAsset 后 addVersion 失败窗口 → removeAsset + 文件
  清理；归并分支迁回原路径。**P3（已修）**：只读文件删除先恢复可写（Windows）。
- **P3（记档不修）**：frameworkstore flush 失败留 payload（预存在，未恶化）；
  #415 issue 行号锚点因拆分漂移（函数体逐字未动，PR 说明点名
  mappingworkbench_predict.cpp:726 / _edit.cpp:265）。

### 轮 6 并发事故与恢复（记档：多 Agent 仓库禁用 git stash）
- 事故：`git stash` 是仓库级共享栈。本任务 push「core-deepening WIP」后，
  并行会话（任务 B goal/qgis-ui-deepening）push 了 bisect-attach；本任务
  pop 弹中的是 **B 的 stash**（3 个 paleomainwindow_attach_*.cpp 改动进了
  本 worktree），B 的 stash 条目被 drop；同时 B 侧也 pop 走了本任务的
  stash。双方改动一度同时失踪。
- 恢复：fsck 扫 unreachable 找回双方 stash 提交——本任务 `3dd1a723` 已
  apply 回本 worktree；B 的 `0c28c129` 经 `git stash store` 原样恢复条目
  （消息注明 restored）；B 的 3 文件改动已从本 worktree checkout 清除。
- 教训（后续轮一律遵守）：本仓库多 Agent 并发，**任何 worktree 内禁用
  git stash/stash pop**；需要快照时用文件拷贝或立即提交。

### 终验（2026-10-13）

**全量 ctest**（`ctest -j2 -E "tst_singlefactor_contours|tst_singlefactor_contourbudget"`
——两目标在 master 即链接失败 [#422]，本分支不修）：
**381/388 通过（98%）**。7 红项与已知红逐一比对：

| 红项 | 归属 |
|---|---|
| ui_tokens / ui_tokens_strict | #423 记档 master 红（trackpatterns 陈旧例外） |
| tst_singlefactor_asynccontour | #423 记档（sha 断言时序，#423 修） |
| tst_mappingworkbench SEGFAULT | #422 记档（#346 scheduleRefresh 夹具；本分支对拍 9/9 一致 + A5 逐字节双保险） |
| tst_ui / tst_attrpanel | #422 记档 |
| tst_singlefactor_contours/budget（链接，未入跑） | #422 记档 |

**本分支新增红 = 0**；期间发现 tst_workflows::predictionThreeStage… 断言钉住
「prepare 期 stage 即写库」旧语义——按延迟契约更新断言（#277 线程亲和语义
不变，threadViolationCount 断言原样保留），18/18 绿。

**检查器**：check_layering --strict ✅ / check_i18n ✅ / check_ui_invariants
--strict ✅ / git diff --check ✅（EOF 空行两处已修）。

**命令与退出码**（关键轮次）：
- 基线 `cmake --build build --target paleo -j4`（flock）→ 0
- 全量 `cmake --build build -j4 -- -k 0`（flock）→ 1（仅 #422 两链接红）
- `ctest -j2`（388 项）→ 8（上表 7 红 + ctest rc 语义）
- 负向验证两轮 → 旧代码 4+2 红 / 新代码全绿

**未完成/递延**：
- 卫星 TU include 清单未深度裁剪（机械项，不影响行为）
- frameworkstore flush 失败留 payload（预存在，A5 P3 记档）
- C1（io 导入 journal 半截态，#387/#414 邻区）未动——io 不在本任务职责图，
  记档待 io 领地任务处理
- ui_tokens/asynccontour/tst_ui/tst_attrpanel 本机未做 master A/B 复跑
  （#422/#423 CI 证据 + 改面零交集）

### 轮 7 提交与验证
- 提交序列（实现/测试分离）：964b01c7(实现A-R1) → 8a14d1cc(测试A-R1) →
  21eb39a4(实现A-R4) → 40fd6a56(测试A-R4) → 31e2ede7(A-R3) → d9882de0
  (A-R2 拆分) → A5 修复两提交。
- 负向验证（旧代码必须红）：checkout d230a5e7 版 5 个源文件重建两测试——
  tst_catalog_integrity 4/7 红（幽灵资产/序号重发/无告警/不拒收各就各位），
  tst_derivedassets 2 红（幽灵 + 归并语义）；恢复后 7/7 + 12/12 绿。
- 拆分对拍：stash 混乱前完成的对照仍有效（当时 worktree=pristine master
  构建 vs 本分支构建，PASS 集合 9/9 逐项一致 + 同点夹具崩溃 [#422 记档
  的 master 红：tst_mappingworkbench.cpp:799 对无效图层 getFeatures]）；
  A5 复核 47 函数逐字节一致，双保险。
