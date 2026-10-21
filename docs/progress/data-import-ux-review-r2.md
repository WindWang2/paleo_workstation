# 统一数据导入定向复审 R2（终审后增量，2026-10-20）

## 基线与范围

审查基线 master `e6837e14`，工作树 `codex/data-management-import-ux`，
全部改动未提交。本轮只审终审报告（docs/progress/data-import-ux-review-final.md，
2026-10-20 22:33，结论静态放行）之后的增量改动：dataimport_file.cpp 的
well_log 去重补挂路径删除文件名猜测、dataimport_folder.cpp 与 welllogset.h/.cpp
注释、tests 增量（tst_import_ux.cpp 新测试、tst_import_performance.cpp 新增、
其余夹具补齐）、cmake 登记、docs、tools/ui-size-baseline.txt 收缩。
本轮为静态审查：重读当前源码、跑了三个 Python 源码闸门（layering --strict /
ui_size / file_budget），未编译、未运行 qtest。工作树未提交，无法对 22:33
前后做逐行 diff，结论基于当前文件状态与真实调用链。

## 逐条结论

### 1. well_log 去重路径（dataimport_file.cpp）——OK，无漏洞

`validateDuplicateLogTarget`（src/io/dataimport_log.cpp:204-225）行为：

- 未指定目标（choice 空）→ 直接放行，随后
  src/io/dataimport_file.cpp:288 `wellLogInput ? 0 : attachResolvableLinks(...)`
  跳过补挂，结局 AlreadyStored +「没有新的关联」。catalog 零写入
  （tst_import_ux.cpp:281 `duplicateShaAcrossNamesDoesNotGrowEntitiesOrVersions`
  断言 mutationSeq 不变、资产/版本/链接/RAW 计数不变）。安全跳过成立。
- 显式 targetWellId 与已有资产归属冲突 → 拒绝：「这份数据已经存在，所属井与
  本次选择不同。未更改归属，也未新建井」。`new:<名>` 先解析名字，歧义拒绝；
  解析不到既有井也落入同一条拒绝（去重不隐式建井）。
- 显式目标与已有已决链接一致 → 放行，AlreadyStored。

调用点两处且在跳过判定之前：单文件去重段 dataimport_file.cpp:275；
批量 produceItem dataimport_folder.cpp:501-513（在 decision==skip 与幂等
跳过之前，批量默认 skip 决策同样受校验）。测试
`duplicateShaCannotSilentlyIgnoreDifferentTarget`（tst_import_ux.cpp:308，
existing-other-well / new-other-well 两路）覆盖冲突拒绝、零副作用、以及
匹配目标后的幂等重导。

「不再补挂也不校验」的情形只剩「未指定目标」一种——此时什么都不改，
正是设计意图；旧未决关联的修复出口在数据属性关联操作，
docs/import-well-logs.md:59（「旧未决关联继续使用现有属性关联操作，
不会因重导相同文件自动按文件名改挂」）与代码一致。

### 2. attachResolvableLinks 调用方——OK

全仓 grep 仅三处命中：定义 dataimport_file.cpp:457、唯一调用
dataimport_file.cpp:288、声明 dataimportservice.h:336。函数体现存
well_stratification / time_depth / well_head 三个分支，其余类型（含
well_log）走 `else return 0`（dataimport_file.cpp:512-513），且
466-467 行有显式注释：测井归属只能来自验证内容或显式选择，改成参考类型
命中旧资产也不能经此入口恢复文件名猜测。无其它调用方依赖被删分支。

### 3. retypedReferenceCannotAttachLegacyLogByFilename（tst_import_ux.cpp:920-963）——OK，断言有效

夹具：A1.las 的 ~W WELL 头写 B1（内容井名无对应井），既有井名 A1
（仅文件名主名可匹配），旧 well_log 资产带未决链接。以
forceType=reference 重导同字节 → 断言 AlreadyStored、linkAttached=false、
链接仍为未决且 entityId 为空、井数恒 1（不隐式建井）。旧行为（按文件名
主名 A1 补挂）会使 linkAttached=true、entityId=legacy-well，断言全部翻转，
非恒真。

附注：任务描述称旧测试 `oldLasProjectWithoutNewMetadataRemainsReadable`
被替换——当前文件中它仍存在（tst_import_ux.cpp:965），与新测试并存。其断言（旧 LAS 工程重开可读、
导入内容井名 A1 的 CSV 挂到既有井）不依赖被删的文件名猜测，保留无害。

### 4. 注释与代码一致性——OK（一处轻微不精确）

- dataimport_folder.cpp:329「扫描段取消：零行执行零行入库；执行段取消也由
  owner 丢弃整批暂存」——与实现一致：扫描段 plan.cancelled 在
  produceFromPlan 之前返回（零行）；执行段取消见
  src/io/ingestplan.cpp:946-952（progress 返回 false → session.cancelled、
  全批留暂存不提交），owner 提交面 src/io/dataimport_session.cpp:426-433
  对 cancelled session 走 failSession 不入库。
- welllogset.h:22-23「WellLogRead 分派 LAS / DLIS / LIS / SpreadsheetML /
  文本」与 src/io/welllogread.h 的 WellLogFormat 枚举及
  welllogread.cpp:61-73 formatTag 一致；indexBasis 取值集合与
  welllogset.cpp:188-189、280、294-299 的消费面一致。
- 轻微：welllogset.h:34 行内注释把 format 枚举写成 `"las"/"dlis"/"lis"`，
  实际 formatTag 还会产出 `spreadsheetml`/`text`/`unknown`
  （welllogset.cpp:189 直接透传）。同文件 22-23 行已写明五种分派，仅这处
  行内枚举不全，建议后续顺手补齐，不影响行为。
- 轻微：docs/progress/data-import-ux-audit.md:12「XML 依赖文件名主名回退」
  与当前实现不完全吻合——XML/文本井身份来自工作表名约定与 WELL 列
  （welllogtext_convert.cpp:144-148、58-60），无文件主名回退；无身份时
  要求用户选择的结论列与代码一致。属风险描述列措辞陈旧，建议后续修订。

### 5. cmake 登记与 tst_import_performance.cpp——OK

cmake/extra-import-ux.cmake:5
`add_paleo_test(tst_import_performance LIBS paleo_workflow paleo_services paleo_io)`
符合 CMakeLists.txt:967-991 契约（LIBS 最小链接集，首库决定 LABEL=workflow）；
链接集与包含头（workflow/folderimport.h、services/welllogset.h+
paleotaskservice.h、io/dataimportservice.h+welllogread.h）对应，与既有
tst_folderimport（paleo_workflow 伞下同族头）模式一致。依赖的
tests/perfbudget_relax.h 存在。

测试含真实断言（单函数 representativeXmlBatchQueriesAndCancellation 全程
QCOMPARE/QVERIFY，抽查）：XML 解析井名/MD 基准/曲线数/逐曲线样本数与
NaN 位（165-175 行）；批量 24 个 CSV 全 Imported、单井、链接数、并集
曲线数、采样间隔 0.37 与 NaN 位（215-231 行）；数据体缺失时 storedHeader
元数据读与字段一致性（245-258 行）；异步批回调在 owner 线程、事件循环
有心跳、threadViolationCount==0（300-330 行）；取消后 staging 目录删除、
catalog 四表全空、raw 不存在（353-365 行）。非恒真、针对行为。

### 6. ui-size-baseline 收缩——OK

src/ui/pages/datalist_tree.cpp 当前 1059 行，baseline 第 7 行冻结值 1059，
一致；1094→1059 为收缩，符合「只缩不涨」。`python tools/check_ui_size.py`
本地跑绿（clean，10 文件 / baseline 10 条）。

### 7. 层违规风险——OK（另有一项 master 既有闸门红，非本次引入）

- 改动文件头三行「// 层：」标记均在：dataimport_file.cpp /
  dataimport_folder.cpp / dataimport_log.{h,cpp} / welllogset.{h,cpp} 为
  「数据」，datalist_tree.cpp 为「视图」。
- io/services 改动文件无 QtWidgets、无 ui/ include（grep 零命中）。
- `python tools/check_layering.py --strict`：检查通过，无层违规。
- file_budget 附带发现：`python tools/check_file_budget.py[--strict]` 在本
  worktree 报 1 项新增超标——src/ui/pages/constraintpage_interpolation.cpp
  [fn:updateEngineRows] 230 行 > 200。经对 pristine master（git archive HEAD）
  复跑确认：master 提交态本身即带同一项新增红，外加 folderconfirm.cpp 与
  datalist_tree.cpp 三项回升红；本分支未触碰 constraintpage_interpolation.cpp，
  且修掉了后三项（拆分 folderconfirm、收缩 datalist_tree）。即该红为
  master 既有债、不是本次增量引入；但它会让本分支的 file_budget 闸维持红，
  处置（修 updateEngineRows 或按纪律在 master 侧处理）需在合并前知会作者，
  不能靠给本分支基线补登转绿。

## 总结论

放行。终审后的增量改动与终审结论一致：well_log 去重不再凭文件名补挂，
显式目标冲突在单文件与批量两路均前置拒绝，未指定目标安全跳过且零写入；
新测试断言有效；注释、文档、cmake 登记、规模闸收缩均与代码一致；无新
层违规。遗留三项非阻断事项：(a) welllogset.h:34 format 行内枚举不全；
(b) data-import-ux-audit.md:12「XML 依赖文件名主名回退」措辞陈旧；
(c) file_budget 的 updateEngineRows 230 行红为 master 既有（已用 pristine
master 复跑证实），需作者知悉而非本分支补登基线。本轮未编译、未跑
qtest，执行验证责任仍在作者。
