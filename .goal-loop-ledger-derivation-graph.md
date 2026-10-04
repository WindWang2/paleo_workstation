# 方向 36：版本衍生血缘图谱

日期：2026-10-04（Asia/Shanghai）。基线：origin/master `24daf8f`。
分支：`goal/derivation-graph-20261004`；工作树：`.worktrees/derivation-graph`。

## 决策与契约

- 挂点选实体页 `secDerivation` 折叠段，与 `secVersion` / `secTopo` 同一宿主。
  理由：资产/实体上下文、版本时间线和预览信号链已在此汇合；数据页额外侧栏会重复上下文。
- 图形为原生 QGraphicsView；RAW 正方形、DERIVED（以及其余受管产物阶段）圆形、
  !managed 外链菱形。箭头口径固定 parent → child。环的回边走上方，自环/同列边绕行。
- 所有图事实经 `services/derivationgraph` → DataCatalog 既有 API；下游复用
  downstreamClosure，上游读取 parentVersionIds。没有增加任何 DataCatalog 接口。
  过滤、深度、缺失源计数、选中双向闭包均在服务层；图仅消费 DTO 和发信号。
- 默认上/下游各 2 层，原生滑块范围 0–12，默认最多 80 节点（服务硬上限 160，
  渲染再去重/限额）。折叠与过滤分开计数；环成员双向重合时计入较近一侧，同距优先上游。
- staleOnly 语义：过期节点 + 当前谱系中的源版本（控件显式说明「含源版本」）。
  体检新增过时版本分类，detail 与图 tooltip 使用原样 staleReason，版本 id 可双击定位。
- 元数据缺失不补造节点；说明缺失源数量；无关系仍画孤立节点并说明「该版本无衍生记录」。
  无 catalog/未开工程禁用控制，隐藏空白画布、显示原因。
- 颜色/字体/控件间距取 PaleoTheme，双主题即时跟随；长标签两行截断，完整内容在 tooltip。
  保留用户缩放；只有显式「适配视图」调用 fitInView。

## 批次与环境事实

1. 图服务、图部件、实体页挂点、通用版本预览、时间线预览和体检定位为一个功能批次。
2. 首次 RelWithDebInfo 全量链接因磁盘用尽失败；仅 strip 本工作树新建可执行文件，并
   配置 `-DCMAKE_EXE_LINKER_FLAGS=-Wl,--strip-debug`，对象/静态库仍保留调试信息。
   曾误启动重叠 build，立即停止，之后每次仅一个 `cmake --build ... -j8`。
3. 第一轮 `tst_derivationgraph` 全绿（1.83 s，测试时长是观测值，非性能断言）。
   场景：真实历史 PNG 颜色进预览、分叉合流、stale/体检、240 链、201 分叉、环/自环、空态、实体/外链。
4. 第一轮全套 279 项：244 通过、2 基线失败、33 未执行。运行中 build 目录被移除，
   后续项目无法切工作目录（日志 `/tmp/paleo-derivation-ctest-all.log`），不能算通过。
   基线失败：ui_tokens 的 35 个新方向遗留字面量；tst_services2 的无活编辑态 busy 标记。
5. 基线修复按原子提交隔离：控件参数取主题 token，既有连井/断层数据符号色精确登记例外，
   不改变地质数据色；无活编辑态只清 `edit` 所有者的孤立 busy，处理任务 busy 保留。

6. 第二次完整 279 项跑完：277 通过，singlefactor_perf 在 QtTest 默认 300 秒
   函数看门狗处中断（CTest 本来设 1200 秒）；perf_catalog 在既有
   absorbSeqs → QString::mid 处 SIGSEGV。目录测试独立重跑通过（20.58 s），
   未改 CatalogStore、不把一次重跑写成「已修复崩溃」。
7. 最后联动修正后增量构建成功；图谱 QtTest 10/10 通过（含真实主窗），
   7 个 layering/i18n/ui_tokens 门禁及 health/staleness/services2/previewdoc 通过。
   14 项相关复验中，剩余 datapreview 阻塞在 jbd2_log_wait_commit；主动中止，
   不计全绿，panels 尚未执行。I/O pressure some≈44%、full≈20%。
8. 最终全套采用本工作树独立沙箱的 /tmp tmpfs 后端：保留原 build/ctest-home
   为 build/ctest-home-disk，symlink 到 /tmp/paleo-derivation-ctest-home-final，
   重配后 build 无需再编。每个测试的 HOME/XDG/TMPDIR 隔离口径不变；
   QTEST_FUNCTION_TIMEOUT=1200000 与已有 CTest 超时对齐，未改性能断言。
9. 软链路径导致 6 个既有路径等值测试红（mappingworkbench、faciesmapping_workflow、
   depthconversion、wellcomposite_shell、cache_io、onnxsessions）；属于本次本地接线影响。
   停止该轮，将 build/CTestTestfile.cmake 的 HOME/XDG/TMPDIR 改为沙箱的物理路径，
   再跑完整 279 项；源 CMake 与测试断言均未修改。六项在物理路径轮均通过。

## 自审修正轮（非零问题轮）

| 维度 | 发现 | 修复/覆盖 |
|---|---|---|
| 分层/旁路 | 选中闭包不能由 UI 遍历边或查版本 | Service::selectionClosure 复用 catalog，两向结果与可见节点取交 |
| 内存所有权 | graphics item 点击中重建会销毁正在处理事件的节点；排队后 id 可跨工程复用 | 延后发信号 + scene generation 守卫；同 id 重建回归 |
| 预览正确性 | 旧入口只开当前版本；地震读者/SHA 缓存按资产；已有标签切换丢所选版本 | openVersion + versionForPreview；释放缓存/取消世代；versionIdAt 恢复实际版本 |
| 环安全/可读性 | 同行回边穿过中间节点会视觉伪装成链 | 回边上绕、同列右绕、自环弯线；迭代访问集与渲染去重 |
| 复审联动 | 重选会回 latest；跨资产残留旧实体；外部同 id 定位误保留旧 stale 事实 | 恢复页实际版本；服务转发实体 id、同步列表；保留仅覆盖图点击的同步信号栈；真实主窗覆盖跨实体、同 id stale 原因更新、标签切回 |
| 诚实空态 | 缺失 parent 不应写「无记录」；过滤不可算折叠；同名文件版本不可区分 | 缺失源明确文案；互斥计数；标签 `vN · fileName` |
| i18n/主题 | 服务翻译 helper 需可提取；翻译库无关漂移与工作树绝对 location | QT_TRANSLATE_NOOP；只合入本批新文案，location 归一相对 src；warning 像素/双主题截图 |

## Oracle 证据

| Oracle | 自动化证据 |
|---|---|
| RAW→D1→D2 / parent→child / 指定版本预览 | chainAndExactVersionPreview：3 节点 2 条边；点击 RAW 后 PNG 红像素，D2 蓝像素；previewVersionId 正确 |
| 分叉与合流 / 共享父去重 | forkMergeSelectionAndDedup：4 节点 4 边；人工重复 DTO 仍 4；无关分支 opacity=0.25 |
| stale 与体检口径一致 | staleReasonFiltersAndLiveTheme：markDownstreamStale；HealthIssue 同 reason/id；双击 jumpToVersion；双主题 warning 描边真像素 |
| ≥200 规模防御 | largeGraphHonestCountsAndDepthControl：240 链默认 3/折叠237，3层4/236，中部双向5/118/117；201分叉80/121；抽样 ns，无绝对时间门 |
| 人工环 / 自环 | cyclesMissingSourcesAndEmptyStates：3环3节点3边、自环1节点1边、闭包终止；generation 拒陈旧点击 |
| 空态 / 缺失源 / 禁用 / 孤立群 | 同上及 entityScopeAndExternalKind：无关系明示、未知parent不补造、no catalog禁用、实体两孤立版本分行 |

图谱 QtTest 共 10 项（8 场景 + init/cleanup），截图与测试日志见 docs/evidence/derivation-graph/。最终全套结果与两轮零 High/Medium 自审结论在下方记录。

## Low（PR body 同步）

- 使用确定性层列布局，密集多父图的交叉边没有自动消交叉；节点上限使开销有界。
- 宽分叉超过 80 后，用实体/资产/kind 过滤定位被折叠版本；本批未新增完整图库分页。


## 图谱冻结快照自审 1（后被完整快照取代）

前置：最后联动/所有权修正后的 86 项相关测试全部通过（40.15 s），包括整个
76 项 UI 测试集与服务/三层门/两 i18n 门/两 token 门。未修改独立算法代码与
其性能测试；该项旧绝对预算的复验另记验证节。
代码快照 SHA256：`bedbf7b901f5681e722f9273f9a7d4de447ccf92ad8d31c0c19c42ad2a0f9548`。

| 维度 | 核查结论与依据 |
|---|---|
| 分层 | 新源文件头前三行词表正确；服务无 QtWidgets/ui include；图/面板在 ui，分层三门全绿 |
| 旁路读数 | DerivationGraph/Panel 不含 DataCatalog/QSql/QFile/BFS；parent/closure/过滤/计数/选中闭包均由 Service 回答；版本/实体定位经 PreviewDocService |
| 内存所有权 | scene 拥有 item；重建前清借用映射；排队点击绑定视图 + generation；定位信号后 QPointer 守宿主；切版本作废旧 SHA/解码/LAS 世代；销毁宿主回归通过 |
| 环安全 | 上游迭代访问集，下游复用既有闭包；渲染节点/边去重；自环与三环/陈旧同 id 点击回归通过 |
| 空态 | 无关系孤立节点 + 明文；过滤/深度/缺失源文案互不冒充；no catalog 控制禁用 + 说明，缺失 parent 不补节点；折叠与过滤计数互斥 |
| i18n/设计 | 新串 tr/QT_TRANSLATE_NOOP；TS 仅任务文案、相对 location；字体/颜色/控件间距取 PaleoTheme；双主题实图与 warning 真像素已核对；两门各全绿 |

结果：High=0，Medium=0；Low 见下列既知取舍与 PR body。本轮之后不修改实现。


## 图谱冻结快照自审 2（后被完整快照取代）

再次从边界场景反向追读相同冻结快照；与第一轮之间没有实现变更。

| 维度 | 独立复核路径 |
|---|---|
| 分层 | 从 extra-derivation-graph.cmake → 分层静态库 → EntityPanel 挂点追读，未引模块/第三方布局库；严格 baseline 仍为零 |
| 旁路读数 | 从体检 stale 行 → versionActivated → openVersion → 服务 path/实体门面 → focusVersion 回链；图查询全部 DTO，视图不遍历 catalog；未增加 DataCatalog API |
| 内存所有权 | 重读图 loadGraph 的 clear/generation、鼠标延后信号、QPointer 宿主销毁、openVersion 缓存释放与 pending 表；跨实体/同 id 事实刷新/同步销毁回归均通过 |
| 环安全 | 重读双向交叠节点分类、seed 排序/cap、QSet 边对；循环和自环不做拓扑递归；回边箭头落 child 边界；重复父/重复 DTO 只画一次 |
| 空态 | 从 filtered=全部、深度=0、missing parent、孤立群、无工程路径回看 summary；真实计数不凭空补关系；手动 fit 不改外层窗口或刷新时自动缩放 |
| i18n/设计 | 重读 tooltip HTML 转义、原样 staleReason、两行 elide 与完整 tooltip；XML 可解析；六张图核对浅/深、链、分叉、环、空态；token/i18n 两门各绿 |

结果：High=0，Medium=0。两轮均为本代理 diff 自审；Low 取舍记录在 ledger 与 PR。


## 性能追加前的阶段性验证（后被最终复验取代）

- 最新 RelWithDebInfo 全量构建成功，所有构建/测试并行度 ≤8。
- 完整 279 项跑完：278 通过，1 失败（402.79 s）；目录性能测试通过（3.59 s）。
- 最后跨实体/同 id 刷新/同步销毁修正后的 86 项相关复验全绿（40.15 s）。
  其中 UI 全部 76 项与所有分层/i18n/token 门均通过。
- 六项 Oracle 均通过，图谱 QtTest 10/10（8 个场景 + init/cleanup），无 skip。
  240 版本默认构图/建场景抽样 3604671 ns，80 节点真绘制抽样 11447606 ns；
  抽样读数不是断言，也不作为跨机器保证。
- 唯一全量失败：既有独立算法目标 tst_singlefactor_perf（最小链接集 paleo_algorithms，
  不链接图谱/页面）。原测试 L≤15000 ms；全套 L=15935 ms，独立复验 L=16027 ms，
  固定 CPU7 复验 L=16028 ms。cell/well 比例、取消响应、内存门通过。未修改算法或此
  测试/门限，`git diff origin/master -- src/algorithms tests/tst_singlefactor_perf.cpp` 为空。
  因此不能写「全套全绿」；本方向功能/Oracle/相关集完成，整库全绿验收仍受该门阻塞。
- 原始日志和六张截图已归档 docs/evidence/derivation-graph/。临时沙箱接线不提交；
  收尾恢复普通 build/ctest-home 与 CMake 默认测试环境。
- 本批不自行合并；以草稿 PR 提供可审查结果。若要求将独立单因素算法优化纳入本方向，
  需明确扩大范围；若按图谱相关集验收，需明确解除整库全绿约束。


## 全绿门追加修复（独立原子提交，先前冻结快照被取代）

单因素原 15 s 门在独立/固定核复验中稳定红，纳入为「每批测试全绿」前置修复。
未改预算/测试门限：localidw 默认 power=2、对平方距离的指数 -1，用 1/base
代替每查询×每井的通用 pow；保留同一 base 归一化顺序，其他幂走原路径。
已有四井解析解/恒等与退化测试，并补两井 1:3 距离的平方反比/非整数幂解析解。
此前两轮零 H/M 对应旧图谱快照；追加修复后需整库重验并重新进行两轮完整自审。

第一步倒数路径后原预算仍红（L=15553 ms，原 15 s 门未改）；继续把原有
三项张量二次型 tangentEnergy 原式放入头内联，移除 cpp 重复定义，接口签名与
算式顺序保持。目的：查询×井×方向热循环免跨翻译单元调用/临时数组落栈，并复用
已有 dx²/dy²。既有方向反转/张量能量回归覆盖该式。此追加范围仍独立为性能提交。

追加数值修复的重链接曾遇 GNU ld 单项 SIG11（tst_uxtheme）与重定位 error 4
（tst_evolutionplayer）；没有把链接器异常解释为业务错误，按 -j4/-j1 重试，
所有并行度均不超过用户上限。构建完成前不运行该次测试。


## 最终复验与代码自审 1（完整快照，High=0 / Medium=0）

完整 279 项执行完毕：277 通过、2 失败（437.81 s）；LAS 位一致性检查与
XML 预览的 SIGSEGV 各单独复验通过（合计 15.97 s），最新逐项状态 279/279
为绿；不宣称「单次全套一次绿」，保留原始日志与最后状态汇总。未修改 LAS/XML
实现或测试门限，未给不能复现的崩溃虚构原因。
独立单因素原 15 s 门通过：L=13810 ms，S=3082 ms，比例/取消/RSS 均绿。
两井解析解（默认平方反比/非整数幂）、方向反转/能量恒等回归通过。
六项 Oracle 与图谱 QtTest 10/10 通过，所有门禁绿，无跳过。
最终代码快照 SHA256：`ba9ac53c1fa58124cad8138eb1ebc61bfc896f4b154e42346b608ec5ae84a0cf`（42 文件）。

| 维度 | 本轮核查与证据 |
|---|---|
| 分层 | 新文件层标识、服务无 QtWidgets/ui、图归 UI；三层门全绿；数值快路径在算法数据层，既有函数签名与算式不变 |
| 旁路读数 | 图只消费 DTO；所有 parent/闭包/过滤/计数读 DataCatalog 既有 API；版本/实体/path 经服务转发，没有新增 Catalog API |
| 内存所有权 | scene 拥有 item；generation 延后定位与 QPointer 同步销毁宿主；切版本释放/作废缓存与 pending；头内联无新状态/分配 |
| 环安全 | 服务迭代访问集/既有下游闭包，渲染节点/边去重；三环、自环、重复父、同 id 旧点击全部有证据 |
| 空态 | 无关系/缺失源/过滤/深度/无工程区分明确；孤立群平铺；折叠/过滤互斥计数；未知版本不补造 |
| i18n/设计 | 新串可提取且 TS 相对 location；主题 token、双主题/警示真像素、tooltip 转义、长标签 elide；两 i18n 门与两 token 门全绿 |

结果：High=0、Medium=0。Low：确定性多父图未自动消交叉；宽分叉用过滤定位，
无图库分页。单次测试不稳定现象作为验证事实记录，不归咎于未经证实的业务改动。


## 最终代码自审 2（完整快照，High=0 / Medium=0）

轮间实现未变，42 文件 SHA256 与第一轮一致；验证快照包含性能前置修复。

| 维度 | 第二轮反向核查 |
|---|---|
| 分层 | 由目标清单回查新源头标识/静态库归属、view→service→catalog 链与算法头内联；无 Widgets 进入数据/功能层，严格门无残留 |
| 旁路读数 | 由体检版本行/时间线按钮/图点击三入口回查同一服务预览与实体/path 查询；Service 的候选/计数与已知 catalog 事实相符，没有直接文件或 SQL 探测 |
| 内存所有权 | 再查 scene 借用清理、timer 上下文/代际、信号回链销毁守卫、旧解码句柄取消；图内保留仅覆盖同步点击栈；内联函数只有原纯算式，无新共享状态 |
| 环安全 | 再查 seed/双向距离交叠计数、重复父/边 QSet、回边/自环箭头端点；闭包均迭代，头内联不会改变图拓扑 |
| 空态 | 再查同 id 更新 staleReason、历史 Office 原件降级、缺失 parent、过滤空集、no catalog；不得显示其他版本字节；默认80/服务160界限与240/201计数证据完整 |
| i18n/设计 | TS XML有效；中文源串、完整 tooltip 转义、标签截断、token配色、主题即时刷新；复核6张截图与原始 Oracle10/10记录；未添加绝对墙钟断言 |

数值前置修复额外核查：base 仍为同一平方距离/nearest归一化，power=2 指数 -1
等价倒数；其他幂保持 std::pow。tangentEnergy 原算式/签名与 max(0) 顺序不变。
默认/非整数幂解析解、原有方向/边界/恒等测试与原性能门均通过。
结果：High=0、Medium=0；Low 与第一轮相同并写入 PR。原子提交顺序为主题门、
QGIS 孤立占用、单因素数值热路径、图谱及证据。所有源码在两轮之后保持冻结。


归档整理：证据文本仅规范化行尾空白；原始失败内容、状态与调用栈均保留，源码冻结未变。
