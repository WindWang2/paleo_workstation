# 井震联动 · 智能预测 · 单因素 · 后台执行深度完善（C 方向，2026-10-13）

分支 `goal/well-seismic-ai-compute-deepening`，基线 origin/master `8e963cd7`。
目标：只深化已有模块（预测链/井震数值链/单因素计算核/后台任务语义/验证反馈），
准确、可复现、可解释、取消安全、性能有证据。

## 基线快照（O1）

- master `8e963cd7882db6337121547574fd328549600d79`；Open PR：#432
  （`goal/ci-mappingpages-20261013`，只改 `tests/tst_mappingpages.cpp`——本方向避开该文件）。
- Open Issues 70 项（#352–#421）全部进排除表；与 C 管辖直接相关的排除指纹：
  - 预测链：#402（ONNX 销毁竞态）、#405（aichat 取消观测窗）、#407（tst_wellfacies 竞态）、
    #409/#410（预测覆盖缺口立案）。
  - 井震：#354（resumeScan 方言）、#364–#367/#365/#366（剖面 LOD/纹理/增益/点击/反演守卫 #371）、
    #370（最近井有符号比较）、#391（切片 LRU 键）、#372（空轴越界）、#389（LAS 深度轴单调）、#414。
  - 单因素：#352（协克里金线程）、#393（FaultPathMetric 预算）、#392（fieldcontours 空 boundaries）、
    #377（演化 CSV）、#415/#417（低危清单）。
  - 后台：#353（RealizationWorkflow commit 崩溃）、#379（constraintfactorjobs 裸捕 this）、
    #390（previewdoc worker）、#378（retryFailed）、#416（services 低危清单）。
- #425/#427/#429/#430/#431 均已合入，不重做。

## R0 普查结论（4 个并行勘察 + 主 Agent 核读）

调用链矩阵与候选清单见各轮记录。选定工作面（按价值排序，全部避开排除指纹、
不引入新算法家族/新业务面/新 schema）：

1. 合成记录违反 #125 逐井时深口径（dock_wells.cpp:224 传画布全局常速模型给
   computeSyntheticSeismogram，逐井校准 twtMs 被丢弃）——井震对齐核心 QC 失准。
2. AC/DEN 值单位不设防（sectionworkbench 裸存 values.unit；slice.cpp:962 硬设
   µs/m；ft 基井速度 3.28× 误差）。
3. WellPredictionService::codeFor 对 schema 外相标签静默自增编码（顺序依赖，
   违反「宁可拒绝不预测」）。
4. 单井预测缓存键缺模型版本/标签（wellfaciesworkflow.cpp:385）。
5. tile 产品 provenance 缺 tile 形状/halo/模型 sha256（可复现性缺口）。
6. suggestHorizonTracking 无取消钩子（aiassistworkflow.h:79 自述）。
7. 深度换算 compute 段无中段取消（convertGridToDepthFile 循环不查取消位）。
8. 单因素比值值域门生产未接线（outOfRangeCount 恒 0）+ 冲突井仅计数不可见。
9. ValidationWorkflow 异步丢弃守卫零测试。
10. geostat 核（kriging/variogram）零冻结参照对拍。

## 逐轮记录

### 轮 1：井震数值链（R2）——commit 5b5777f4

- 合成记录 #125 违规修复 + AC/DEN 单位防御 + 跨文件限定名查找（详见提交说明）。
- 负向验证：旧路径下 `syntheticSeismogramPerWellTd` 的平移断言必然失败
  （常速模型抹掉 +40ms 平移）；`curveValueUnitsConvertedOrRefused` 在单位
  换算缺失时 55 µs/ft 原样进曲线（值 55 ≠ 180.446）。
- 开发中发现并修复：限定名 `AC@文件基名` 绕过族判定（先在测试里实测暴露，
  再修 `baseCurveMnemonic`，dock 合成/反演/子波三处查找同修）。

### 轮 2：预测链诚实性（R1）——commits 44b2906e / 4201254c

- codeFor schema 突变废止（纯函数 faciesCodeForLabel + 整井拒收）。
- 缓存键钉模型版本/标签（loopback 换版场景回放断言）。
- tile provenance 补 tile_rows/tile_cols/halo/model_sha256（三件产品
  PALEO_PROVENANCE 元数据断言）。

### 轮 3：协作取消（R4）——commit 84b040d9

- horizonsuggest 逐道取消（计数闸门，非 sleep）；深度换算逐行取消
  （半途深度场不落盘、未转换行 NaN、统计不完整语义钉死）。

### 轮 4：单因素可解释性 + 验证守卫（R3/R5）——commits 2678cf2f / 50dcb1e1

- 砂地比物理域门（维护值组合绕过单字段校验的真实缺口，实测暴露）。
- conflictRows 进 QC issues（两引擎）。
- ValidationWorkflow 异步三重守卫确定性测试（此前零驱动）。

### 门禁与测试（O3/O4，随轮记录）

- `cmake --build build --target paleo -j4`：成功（初始 637 目标 + 增量）。
- 触及套件全绿：tst_seismic_welltie / tst_wellfacies / tst_aiassistworkflow /
  tst_ai_horizonsuggest / tst_velocitymodel / tst_wellfileparsers /
  tst_sections_alignment / tst_algorithms / tst_workflows / tst_wellattributes。
- `git diff --check` ✓；`check_layering --strict` ✓；`check_ui_invariants --strict`
  ✓；`check_i18n` ✓；`check_tidy`（14 个改动 TU）✓。
- 远端指纹复核（轮 4 后）：master 仍 `8e963cd7`，Open PR 仍仅 #432
  （只改 tst_mappingpages.cpp，本方向未触碰）。

## 独立 Code Review（O6）与处置

独立 reviewer（未参与实现）逐文件审查全部 31 个变更文件。结论：**无 P0；
1 个 P1；7 个 P2**。

- **P1（已修，commit 55ac7299）**：writeTileProducts 的 model_sha256 在主线程
  经 loadModelMeta 获取——该调用无条件整读模型文件做 SHA-256，池未命中还会
  重建会话跑 warmup，大模型每次分类完成冻 UI 数百毫秒且与 worker 重复哈希。
  修复：TileInferenceResult.modelSha256 由引擎随加载顺手带出，两个调用点改读
  结果字段，助手删除。
- **P2（已顺手修 2 条，同 commit）**：合成记录「不回退默认速度」注释与上游
  逐井轴「常速近似+标注」口径的偏差——注释精确化；syntheticCore 搬移的死
  代码清理。
- **P2 可接受残留（5 条，如实记录）**：
  1. 单位词表为白名单：KG/M³（Unicode 上标）等变体会被拒（拒绝显式且注明
     原因，不产错误数字）；按需扩表。
  2. `startsWith("DT")` 隐式把 DTST/DTCS 横波声波一并换算（口径恰好正确）。
  3. faciesCodeForLabel 以 code≥1 为合法域的前提未写入头注释（当前
     FaciesCatalog 码制从 1 起，见 faciescatalog.cpp:118）。
  4. 换版后旧键缓存文件成磁盘孤儿（不回放，仅残留）。
  5. Surfer QC 新增 issues 键（schema_version 仍 1）——若外部按 v1 严格解析
     需知悉；仓内无此消费者。
- reviewer 同时确认：四个改签名点全部调用方已更新；拒收路径无半组装泄漏；
  取消检查点位置正确、半途结果不发布；重构核与旧实现位级等价（逐行搬移）。

### 轮 5：全量回归红线核对（干净 master 对照，O3）

全量 ctest（HEAD=55ac7299，`-j4`）：388/392 通过。四个红项逐一核对——
**必须先重建 master 二进制再对照**（主 checkout 的 build/ 过期一天，过期
二进制给出的「master 绿」是假阴性，两度误导归因）：

1. **tst_mappingworkbench（3 slot 败）**：本分支真实回归——C3 首版「schema
   外标签整井拒收」破坏了既有的词表扩展契约（默认词表仅 3 条，远端真实
   标签「河口坝/分流河道」依赖追加条目路径；tst_ui:1830 还显式把 Mock 的
   label 放进请求 schema，本就按「远端可扩词表」设计）。**已修**：改为
   确定性全局扩展——finish() 先收集全部结果未知标签、排序后统一分配
   max+1 连续码，同标签集任意重跑码一致（旧实现的漂移缺陷仍被消除）；
   faciesCodeForLabel 语义注释同步改写。修复后 39/39 通过，连续两遍。
2. **tst_mappingpages（5 slot 败）**：新鲜 master 构建复现同 5 败
   （FactorPageTests 家族）——master 既有红，PR #432（OPEN，外部 Agent）
   标题即「修复 tst_mappingpages」。按所有权排除，本方向未触碰该文件。
3. **tst_ui（300s fatal 挂起）**：新鲜 master 构建复现同挂起。机制（栈
   证据）：mappingWorkbenchCanvasRibbonAndReferences 完整跑完后留下绑定在
   已销毁 QTemporaryDir 的 manifest/catalog → 下一 slot openPath →
   bindCatalog 延迟 singleShot → synchronizeCatalogLayers 的 tryDeclared
   失败 → errorOccurred → paleomainwindow_workbench.cpp:1012 模态
   QDialog::exec 在 offscreen 下无人可点 → QtTest 300s fatal。属主窗
   UI 壳（B 方向领地）+ tst_ui（非本方向测试面），既有问题如实登记，
   不在本 PR 修复。
4. **tst_startup_trace**：串行重跑通过（17.3s）——已知并行负载抖动
   （#109/#423 同款，envdebt 豁免名单在案）。

### O5 计算基准（同输入可复现）

- tst_seismic_welltie::syntheticSeismogramOverloadsAgree：20000 采样输入
  （变声波+变密度，密集反射），TimeDepthModel 入口 vs 逐井时深对入口
  **位级一致（max_abs=0）**，输出 8100 样本；耗时观测 model_ms=1544 /
  table_ms=1539（RelWithDebInfo，本机 CachyOS；只记录不设阈值——重构核
  与旧实现逐行等价的直接证据）。

### 验收状态

- 关键套件连续两遍通过：tst_mappingworkbench / tst_wellfacies /
  tst_wellattributes / tst_seismic_welltie（另 tst_algorithms、
  tst_workflows、tst_aiassistworkflow、tst_ai_horizonsuggest、
  tst_velocitymodel、tst_wellfileparsers、tst_sections_alignment 全绿）。
- `check_tidy`（含轮 5 改动 TU）✓；`git diff --check` ✓。



