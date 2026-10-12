# core-workflow-deepening（goal/core-deepening-20261012）

任务 A：现有业务内核深化——契约修复、行为守恒拆分、有证据的性能等价优化、
服务一致性。基线 `origin/master` @ `d230a5e7`（2026-10-12）。排除边界与逐轮
证据见 `.goal-loop-ledger-core-deepening.md`（71 个 open issue 指纹表 + open
PR #422/#423/#424 占用区）。

## A-R1 catalog 契约完整性

**问题**（均未被既有 Issue/PR 占用，指纹见账本）：
1. `SequenceFramework::FrameworkStore::save` 失败路径经 `BatchSave` 析构
   flush 提交「零版本幽灵资产」/半截链接（格架目录创建失败、写失败、sha
   失败、addVersion 失败、addLink 失败六口全中）。
2. `CatalogStore` 装载跳过坏段版本行时不吸收其 ver-N 序号：`nextVersionId`
   重发同号 → 内存 dup 检查看不见盘上坏行 → upsert 覆盖 → 其它版本
   `parentVersionIds` 指向旧 ver-N 的引用被静默改接到新版本（静默血缘损坏）。
3. 装载侧 JSON 列（extra/parents/corners）单格损坏静默返回空——「有父版本」
   被读成「无父版本」无任何可见痕迹（写侧恒为对象/数组/空，非此即损坏）。
4. `updateVersionExtra` 批量 map 重载不校验 `#` 保留字——批量通道可静默污染
   `#history/#source` 审计面（单键重载自方向 79 起即拒收）。

**修复**：失败路径 `batch.abort()`（快照回滚含序号/索引）+ 孤儿文件清理；
跳过坏行前吸收序号；三个 JSON 解析失败 qWarning（文本截断 120 字符）；
map 重载保留字/空键同口径拒收。

**测试**：`tests/tst_catalog_integrity.cpp`（新）5 用例；负向验证（对改前
代码跑 → 幽灵资产/序号重发/无告警/不拒收四类红）记录见账本轮 1。

## A-R4 派生登记契约（失败/取消不留假在场）

**问题**：`DerivedAssetRegistrar::stage` 即刻 `addAsset` 落库——计算取消/
失败留下零版本幽灵资产（`propertymodelworkflow.cpp` 取消路径注释声明
「未 commit 的 staging 随请求作用域释放」，实现不满足自己的契约）；
`mapexport` addVersion 失败不清已落位受管副本、不撤刚加资产。

**修复**：stage 改 find-or-reserve（同注册器同名复用预留 id）；commit 期
结算——预留资产此刻入库；窗口内外部已建同名资产则迁文件归并（版本号按
既有资产口径重算，指纹按新路径解释，防线不弱化）。mapexport 失败路径
removeAsset + 清副本。

**边界**：成功路径最终态与旧实现逐字段一致；唯一语义 delta =
「同名产物两次 stage 且都 commit」并发窗口由「同资产两版本」变为
「归并到先建资产」（`commitMergesIntoAssetCreatedMeanwhile` 钉住）。

## A-R3 性能（等价优化，均有结构证据）

| 位置 | 证据 | 优化 | 等价性锚 |
|---|---|---|---|
| workflow/constraintwellfactors.cpp | `LithoLexicon::fromProject`（读盘+JSON 解析+两遍默认词表重建）在逐井循环内——W 井 = W×全套 | 循环外一次构造 | tst_factorworkflow 井因素行为 |
| domain/litholexicon.cpp | defaultLexicon 每次重放 ~30 条 addRule | 函数级 static + COW 共享 | 词表内容不可变 |
| catalog/realizationset.cpp | statSurfaces 排序比较器内 `versionById`×2/次（哈希查 + CatalogVersion 含 QVariantMap 按值拷贝） | 预取 QHash 一次，比较器纯 int | tst_realizationset 全序用例 |

否决：entityview 逐种子 BFS 合并（会改 derivedProducts 发现序=行为）；
io/datalist 侧 N+1 消费方在 src/ui（任务 B 领地，只读）。

## A-R2 行为守恒拆分（TODOS.md 拆分候选清单项 2/3）

- `mappingworkbench.cpp` 2408 → 944 主 TU + `mappingworkbench_predict.cpp`(844)
  + `mappingworkbench_edit.cpp`(575) + `mappingworkbench_compose.cpp`(277)。
  函数体逐字平移（切分脚本 verbatim 断言；#415 领地 `mergeIntervals/
  mergeFormations` 块原样平移）。既有卫星 TU 先例（_polygonize/_consistency/
  mappinghierarchy/wellpredictionreview）模式：同 class 多 TU、头文件不动
  （公共 API/ABI/moc 零变化）、匿名助手各 TU 私有副本。
- `appcontext.cpp` 1024 → 847 主 TU + `appcontext_layers.cpp`(192)
  （restoreProjectLayers + refresh{Wells,WellTrajectories,Survey}Layer）。
  projectOpened 巨 lambda 的顺序敏感段（锁接管/journal/manifest/catalog
  九连绑）按 TODOS.md 方向 83 注记**留在主 TU**——本轮只拆无顺序耦合的
  叶子段。

## 验证

**计量（A-R3，本机 RelWithDebInfo，临时基准不入库 `/tmp/bench_stat.cpp`）**：
- statSurfaces 排序预取（3000 统计面 ×4 token 交错）：旧比较器 4–6ms → 新 4–5ms；
  12000 面：旧 52–67ms（高方差）→ 新 33–38ms，**1.4–2.0× 且方差收窄**；
  全序 identical=1（新旧逐元素比对）。保留。
- 井因素词表出循环：结构证据（W 口井 W×读盘+解析+词表重建 → 1×，projectDir/
  catalog 为循环不变量），行为等价由 tst_factorworkflow 全绿钉死。

**测试（ctest 沙箱）**：
- 新增/更新：tst_catalog_integrity 7/7、tst_derivedassets 13/13（11 slots）。
- 负向验证：对 d230a5e7 版源码重建——tst_catalog_integrity 4 用例红（幽灵资产/
  序号重发/无告警/不拒收各就位）、tst_derivedassets 2 用例红（幽灵 + 归并语义）；
  恢复后全绿（真实失败检出能力）。
- 拆分对拍：tst_mappingworkbench 拆分前后 PASS 集合逐项一致（9/9，同点崩溃于
  #422 记档的 master 夹具红 tst_mappingworkbench.cpp:799）；A5 逐字节对拍
  47 个成员函数 ALL-IDENTICAL 双保险。
- 检查器：check_layering --strict / check_i18n / check_ui_invariants --strict 全过；
  `git diff --check` 干净。
- 全量 ctest（-E 已知链接红两个目标）：见下方「全量结果」。

**A5 独立 review**（未参与编写的子 Agent，逐文件 diff-to-base）：
- P1（已修）跨注册器实例预留不可见 → 任务中心首算必败；P2（已修）结算后
  落盘失败幽灵窗口；P3（已修）Windows 只读删除。P3（记档）：frameworkstore
  flush 失败留 payload（预存在）；**#415 的 mappingworkbench.cpp:1337-1340
  行号锚点因拆分漂移至 mappingworkbench_predict.cpp（mergeIntervals/
  mergeFormations 函数体逐字未动）**。

## 已知边界

- derivedassets 语义 delta：同名产物「两次 stage 且都 commit」的并发窗口由
  「同资产两版本」→「归并到先建资产」（commitMergesIntoAssetCreatedMeanwhile
  钉住）；catalog 重开后未 commit 的预留 id 序号缺口合法（不重发）。
- 卫星 TU 仍携带较全 include 清单（行为守恒优先；进一步收窄属后续机械项，
  不改语义）。

## 全量结果

（全量 ctest 数字与红项比对表见 .goal-loop-ledger-core-deepening.md 终验节）
