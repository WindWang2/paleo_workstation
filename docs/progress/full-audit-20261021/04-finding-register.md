# Finding Register（full-audit-20261021）

来源：5 只读 subagent 深审（03-review-{A,B,C,D,E}.md）+ 主 Agent 复核（#598 新代码面 +
20261020 遗留池）。基线 e6837e14。排除核对已完成：147 项已知库、上两轮已修清单、
10 个开放 PR 文件/hunk 级冲突、开放 Issues #556/#558/#559/#561/#582-#597。

## 状态总览

- 新发现立案：23 项（P2×8、P3×15）
- 交叉排除：5 项（E-1→#580 已注册；E-3→#581 已修；E-4→上轮 E-11 裁定 #501 后续；
  A-03→#603 重做同函数 BLOCKED；E-5→#604 交互风险 SKIP 仅记录）
- 遗留池复核入册：3 项（K-1/K-2/K-3 = 20261020 的 D-06/D-09/A-16）
- 主 Agent 复核放弃：petrophys EXPR 单位（P3 边际）、D-14 不可定位子项、
  AppContext 回调契约洞（不可达，仅记录）

## P2（8 项）

| ID | 置信 | 位置 | 一句话 | 冲突核对 |
|----|------|------|--------|---------|
| A-01 | VERIFIED | src/io/shacache.cpp:153-187/241-258 | ShaCache 每 miss 持锁全表 JSON 序列化+同步落盘（65536 条上限 ≈8-13MB/次），万级文件导入 ≈数十 GB 写放大 | 无冲突 |
| B-1 | VERIFIED | src/services/seismictaskservice_slice.cpp:1084-1093 | 合成记录 Ricker 子波系数 2π 应为 π²，主频 ≈0.798×请求值（25Hz→20Hz），与 inversion/wavelet.h 正确公式矛盾 | 无冲突 |
| B-2 | HIGH | seismictaskservice_slice.cpp:199-215 + .h:768-770 | 任意线剖面 columnDistances 是 il/xl 线号单位，头注承诺「真实道距绝不合成」，消费端按米用；缓存命中还有均匀合成分支 | 无冲突 |
| C-1 | VERIFIED | src/workflow/aichatcontroller.cpp:325-356 | retryLast() 重发漏带 tools()，重试轮静默失去全部领域工具调用能力（注释自述设计意图相悖） | 无冲突 |
| C-2 | VERIFIED/HIGH | src/workflow/mappingworkbench_consistency.cpp:230-285 | 后台一致性报告写默认 /tmp 再 QFile::rename 进工程目录——跨文件系统必 EXDEV，常见 Linux 布局上功能 100% 失败 | 无冲突 |
| C-3 | VERIFIED/HIGH | mapexport.cpp:287-290 / horizonbatchexport.cpp:527-534 / mapbookqueue.cpp:282-287 | 三条导出链以开始时 mutationSeq 作发布闸——窗口内任一无关 catalog 写入（导入/预测/另一导出）即整批判废「工程数据已变化」；#480 P3-3 已认定该模式为缺陷并从单因素链删除 | #580 hunks 在 :85/:315/:492——不重叠 |
| C-4 | VERIFIED | src/workflow/inversionworkflow.cpp:672-683 + seismicsectiondock_inversion.cpp:199-279 | 地震反演整幅阻抗体（数百 MB）写 /tmp/paleo-inv-*，commitExternal 只拷不删源，任何结局均永留 /tmp，无清理钩子无清扫器 | 修复点在 dock 文件（无冲突） |
| E-2 | VERIFIED | tools/check_layering.py:707 / check_qgis_spread.py:48 | qgs 直连/散布三闸只扫 workflow+ui——algorithms 16 + linkage 4 + services 1 文件的 qgs* 直连全部隐形，实跑证明数据层 qgs include 零违规 | 无冲突（政策面见修复计划） |

## P3（15 项）

| ID | 置信 | 位置 | 一句话 |
|----|------|------|--------|
| A-02 | VERIFIED | src/io/dlisparser.cpp:1186-1213 | 跨段 LR trailer/pad 剥离按累计缓冲校验非本段 bodyLen，损坏 padcount 可切掉前段字节（lisparser 同族为正确口径） |
| A-04 | VERIFIED | src/io/dataimport_document.cpp:295-321 | 转换出队端缺 RAW 存在性复核：排队期间源被删→空参数起 soffice+生成 /.pdf 隐藏名；m_convRawVersionId 跨工程脏状态 |
| A-05 | VERIFIED(不可达) | src/io/cachecore.cpp:356-361 | putStr >64KB 截短长度前缀却写全部字节，破坏长度==实写不变量（加固） |
| B-3 | VERIFIED | seismictaskservice_attr.cpp:399-454 | 相干时间切片逐线读整剖面（nS 全样）只用 ±tH 小窗，IO 放大 ~300×，交互拖动路径 |
| B-4 | VERIFIED | seismictaskservice_attr.cpp:396/451/479 + _slice.cpp:749-758 | 进度账本失真：相干恒 50%→100%；剖面属性 compute 段只计 0 号分片（4 线程封顶 62%） |
| C-5 | VERIFIED | wellsectionworkflow.cpp:845-854 + datacatalog_versions.cpp:376-414 | 岩性装配（查询路径）写 catalog extra，map 重载无 no-op 短路——反复抬 mutationSeq（C-3 放大器）+刷 changed |
| C-6 | SUSPECTED | src/ai/chat/llmclient.cpp:427/478 | 非流式应答未并入 finished 时刻 tail 字节（流式分支有兜底、非流式没有），可能截断 JSON 误报 Malformed |
| D-01 | VERIFIED | src/ui/pages/entitypanel.cpp:712-721 | recycle 分支不过滤 unresolved 链接→空 id 幽灵回收条目（往返后消失）；多角色同资产产生重复撤销命令 |
| D-02 | VERIFIED | src/ui/horizonchipbar.cpp:109-122 | reloadHorizons 的 deleteLater 窗口内 findChildren 命中新旧两代，chipNames() 双计数 |
| D-03 | HIGH | src/ui/paleomainwindow_workbench.cpp:221-247 | 一致性报告每次完成 new 无模式对话框，重复运行堆积不复用 |
| D-04 | SUSPECTED | src/qgis/composeattribution.cpp:80-91 | gridOf 首栅格重建路径缺 extent 非空校验（另两路都有），退化栅格下游除零/UB |
| E-6 | VERIFIED | tools/check_qgis_versions.py:59-66 | URL_HASH/补丁名正则绑到文件尾而非 qgis 块尾，追加工程后静默错绑（当前潜伏） |
| E-7 | VERIFIED | tools/ci_triage_reds.py:268 + :155-171 | Test.xml 跨 tag 并集把重跑洗白的 flaky 重新算红，与 gate 裁决矛盾（误红方向） |
| E-8 | VERIFIED | tools/check_ui_invariants.py:107/215-217 | 400 字符窗口截断漏检长样式表 + 基线粒度吸收同规则多处违规 |
| K-pool | — | （20261020 遗留复核）见下 K-1/K-2/K-3 | |

## 遗留池（K 系，主 Agent 本轮复核仍成立）

| ID | 原号 | 位置 | 一句话 |
|----|------|------|--------|
| K-1 | 20261020 D-06 | src/ui/correlationpanel.cpp:104/126/839-853 | m_dragColumn 裸指针跨事件回合，rebuildScene 删列项后 mouseMove UAF（面板常驻隐藏=latent） |
| K-2 | 20261020 D-09 | src/ui/layout/layoutexportactions.cpp:397-401 + :289-297 | 任务启动失败双重上报（两条不同文案连闪） |
| K-3 | 20261020 A-16 | src/catalog/frameworkstore.cpp:259-265 | batch.flush 失败是唯一不清孤儿受管文件的失败路径（对照 :198/:210/:251） |

## 排除/放弃明细（证据）

- E-1 tst_horizonbatchexport_diag 未注册 → #580 diff 已注册
  （CMakeLists +add_paleo_test(tst_horizonbatchexport_diag LIBS paleo_workflow)，其 E-04）
- E-3 triage fail-open → #581 classify 已对 conclusion is None 打显式 NOTE、
  parse_jobs 解析失败返 None→exit 2（空 jobs 载荷下所有车道 NOTE 可见）
- E-4 flaky 台账 CI 跨 run 聚合 → 上轮 E-11 裁定「归 #501 后续」维持
- A-03 dedup readAll 二次解析 → #603 正在重做 attachResolvableLinks
  （wellLogInput 跳过 + 分支重写），BLOCKED_BY_EXTERNAL_WORK
- E-5 lint 缺 ui_tokens → 与 #604（master lint 转绿）直接交互，且加闸即红
  （ui_tokens 现存 1 violation），SKIP 仅记录
- petrophys EXPR 输出单位 v/v（petrophyspanel.cpp:290）→ P3 边际，语义可辩，放弃
