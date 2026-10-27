# 方向 42：数据中心规模治理

## 工作区与实现

- 启动日期：2026-10-04；验收日期：2026-10-05；工作区 `.worktrees/datacenter-gov`；分支 `goal/datacenter-gov-20261004`；基线 `origin/master` / `24daf8f`。
- 用户提供的 catalog 查询与写路径规模事实作为基线，不另开后端规模勘察。
- 已读 DESIGN.md、docs/UI_LAYER_PLAN.md、docs/PALEO_QGIS_PLAN.md、BUILDING.md。依赖使用主工作区 vendored prefix 符号链接；所有构建/测试并行度不超过 8。
- 资产表超过 200 项时按视口分页；表、树、图标、分组的实物行限制在当前页。原高速模型继续持有全量逻辑行。过滤与排序作用于全量内存快照，选中按资产身份跨页保持；小目录保留旧插入顺序。
- 未决实体选择使用共享值模型，避免每个 cellWidget 复制整份实体词表。每页控件与视口行数同阶。大目录刷新不读取各版本文件大小，未知大小为 -1。
- 存储查询归 services，编排归 workflow，对话框只展示快照与发出意图。扫描、SHA 分块复验、文件身份复核、物理清理在 worker；catalog mutator/事务只在 owner 线程执行。
- 目录覆盖：`raw/derived/intermediate/output`（catalog 实际生成路径），兼容 `artifacts/{RAW,DERIVED,INTERMEDIATE,OUTPUT}` 与其小写形式。metadata、.paleo、其他工程目录不推断为受管数据。符号链接、不可读目录与其他受管版本目录明确列为未覆盖；不完整扫描禁止回收。
- 统计为版本逻辑字节：按类型每版本一次；按实体重复角色去重，多实体共享分别归属。外链大小纳入逻辑统计，外链源永不删除。
- orphan 仅表示未被任何版本引用，不推断用途。精确删除选中 stale DERIVED 版本，保留同资产 RAW、链接与资产身份；有保留子版本引用即阻断。孤儿文件先获得临时 unknown 身份，再在同一 catalog 事务中经批量 mutator 移除，复用既有 purge 文件阶段。
- 预览列数量、拟释放体积、影响版本与路径；确认前后台复核 canonical 路径/大小/mtime、owner 复核 mutationSeq。事务失败/确认前取消均零物理动作；已提交文件阶段不再提供取消。清理租约防止 worker 期间增加 catalog 引用；失败残留如实报告。

## Oracle 验证证据

| 验收项 | 测试与证据 |
| --- | --- |
| 1. 10k 规模与语义 | `tst_assetpaging::scaleRenderingFilteringSortingSelection`：10,000 资产，渲染 10 行，pageSize=10；刷新/旧全量基线=0.0347595，过滤/基线=0.0030782，定位/基线=0.0169928。基线实际 GUI stat=120,000，刷新=0，预览=0；探针覆盖 stat/stat64/lstat/lstat64/statx，并验证基线命中。过滤清空、全局降序、跨页身份定位、刷新选中保持分别断言。300 个未决资产夹具另验证实际 cellWidget 数量≤pageSize、所有实体下拉共享同一模型、缩放后仍受视口限制。性能门只使用比率/数量，不使用绝对毫秒。 |
| 2. 体积汇总 | `exactAggregationOrphansAndReasons`：逐项 stat 合计 92 B；类型 well_log=79 B、seismic=13 B；实体 well=79 B、未挂接=13 B；同一实体双角色去重。 |
| 3. 未引用文件 | 同一测试插入 `raw/unregistered/file.dat`，唯一命中 17 B；外链用 `raw/asset/raw/../raw/a.las` 指向已登记 RAW，经 PathCanon 归一化后零误报。共享文件与符号链接未覆盖另有专项。 |
| 4. stale 与批量回收 | 三版本 RAW→DERIVED→child 衍生链；两条 stale 均带“上游源字节改变”。单选父版本被保留子引用阻断；两个 stale+一个 orphan 预览 3 文件、2 影响版本、83 B，真实控制器执行实际释放 83 B；RAW/资产/链接保留，重开与索引健康断言通过。显式事务 abort 恢复批前数据。 |
| 5. 线程与取消 | `scanScaleHeartbeat`：10,000 版本后台扫描，提交/完整扫描=0.0000497554，GUI 心跳 45,264、进度信号 261。32 MiB 外链 SHA 复验期间 GUI 心跳持续；分块第 3 次取消返回未完成。扫描取消、确认前取消、预览后 catalog/文件变化均无清理动作；物理阶段租约拒绝新引用，释放后恢复。 |

## 验证运行

- `cmake --build build -j8`：通过。
- `QT_QPA_PLATFORM=offscreen PALEO_CATALOG_SCALE=10000 ctest --test-dir build -j2 -R '^tst_(assetpaging|storagegovernance)$' --output-on-failure`：2/2 通过（`build/gov-scale-complete.log`，详细值保存于 `build/gov-scale-assetpaging.txt` / `build/gov-scale-storagegovernance.txt`）。
- 分层 strict、i18n/i18n_selftest、ui_tokens/ui_tokens_selftest：独立检查与全套均通过。
- 最终全套 `QT_QPA_PLATFORM=offscreen ctest --test-dir build -j4 --output-on-failure`：279/280 通过（`build/gov-all-verified.log`）；唯一失败的存量 startup 比率项以原断言 `ctest --test-dir build -j1 --rerun-failed --output-on-failure` 复验 1/1 通过（`build/gov-rerun-final.log`）。280 项均已验证通过，无未解决失败。
- 治理台与确认预览通过真实 QWidget 截图核对（`build/gov-storage.png` / `build/gov-preview.png`）：沿用 Noto Sans SC/JetBrains Mono、主题色、token 间距；表格明细完整，取消/确认与可访问名称齐备。
- 全套首轮发现基线 QGIS 非编辑会话提交失败仍保留 busy 标记：仅修复无活动会话分支，活动失败会话继续保留重试/回滚能力；单独原子提交。
- 全套首轮存量 catalog 比率预算受并行竞争失败（80 ms 对预算 77 ms），不放宽阈值、不改变后端；最终全套原预算通过：reopen=44 ms，jsonLegacyOpen=17 ms（预算 71 ms）。
- 最终全套 startup 比率发生小幅波动：service assembly share 0.1687 > 0.1675，注劣化倍数 2.9；首轮同项通过。串行原断言复验 9 个 QTest case 全通过，健康 qgis 份额 0.155、注劣化 0.586（3.8×），绝对注劣化门亦触发；不改变断言、基线或注入量。
- 基线 ui_tokens 已有 35 项违例（基线归档独立执行证明）：间距/字体/QSS 改用 DESIGN token；9 个已有地质数据色按 DESIGN 数据符号例外精确登记，不改变色板。独立原子提交。

## 五维审查

修复轮 R0：默认排序改变旧测试顺序、逐 combo 填全量实体、预览 GUI 路径归一化 stat、扫描遗漏实际 raw/derived 布局、搜索清空被旧过滤条件写回、无引用保护的后台清理窗口、相对路径租约身份不一致、批量删除反复移动数组尾段、逐文件进度通知过密等已修复，并有上述回归证据。

以下两轮均在最终测试与复验全绿后进行，审查代码指纹一致：`7a79ec673cce62ba31a2665cfa567a8fdbd99081fceeccb74a3aacc440500ac0`（35 个实现、测试、CMake、翻译与门禁文件；账本文字不计入代码指纹）。

| 轮次 | 分层 | 线程/生命周期 | 预览与删除保护 | 过滤/排序/选中 | i18n/设计/a11y | 结论 |
| --- | --- | --- | --- | --- | --- | --- |
| R1 | strict 全零，检测/汇总在服务、动作在 workflow、对话框发意图 | worker 只捕获值；owner 提交；每条目检查取消、限频通知；GUI stat 探针零 | 扫描完整性、catalog 序号、文件身份、子引用、共享路径、外链源、事务 abort 与 purge 租约逐项核对 | 10k 全局过滤/降序/定位、清空搜索、刷新/跨页身份、未决控件共享模型与视口限制均验证 | 两门通过，翻译骨架仅本方向新增；两张真实界面截图符合 token，操作控件名称齐 | High=0，Medium=0 |
| R2 | 重新核对新增头文件与 include 依赖，无 ui/QtWidgets 泄漏 | 按关闭窗口、目录变更、扫描/SHA 中断、确认前取消、提交后清理顺序做逆向审查；捕获对象与租约释放边界清晰 | 按无效/空预览、RAW 混合、保留子链、共享文件、路径别名/符号链接、文件变化、保存失败逐项追踪，没有绕过 catalog 的视图删除 | 当前页重建与全量逻辑匹配分离；小目录旧插入序、跨页选中和全局排序一致；分组配置也受当前页限制 | 重新核对取消/确认文案、未知大小/未覆盖/未完成声明与精确数据色例外；diff whitespace 零问题 | High=0，Medium=0 |

连续两轮均零 High/Medium；两轮之间与之后无实现代码改动。

Low（记入 PR body）：全量内存过滤/排序仍 O(N)/O(N log N)；分页消除实物控件增长，未引入 catalog 查询 API。体积为版本逻辑归属计量，非磁盘分配块/硬链接去重；未覆盖目录如实报告并阻断回收。原高速模型大目录大小列保持未知，完整大小在治理扫描中提供。存量启动/目录比率门的负载波动与原断言复验结果同样记入 PR。

## 提交

按原子范围依次提交：`fix(ui): 对齐存量面板与设计 token 门禁`；`fix(qgis): 清除无编辑会话遗留的忙碌标记`；`feat(data): 实现资产分页与预览式存储治理`（包含本账本、Oracle 证据与两轮审查）。PR 以当前分支创建草稿，Low 明列于 body。
