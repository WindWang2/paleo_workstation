# 发现登记（Phase 1 五路审查 + Phase 2 主 Agent 逐项复核）

基线 `0b9c0a7a`。审查方式：5 只读 subagents（A 数据层 / B 算法服务 / C workflow-ai /
D qgis-ui / E #815 修复横切）+ 主 Agent 对全部拟修发现逐行复核代码坐实。
排除面：80 个开放 Issues、swarm 遗留 L-01…L-23、主 checkout 在途修复文件
（见 01-conflict-matrix.md）。

## 进入修复池（全部经主 Agent 复核坐实）

| ID | Sev | Conf | 位置 | 概要 | 来源 |
|----|-----|------|------|------|------|
| F-01 | P1 | VERIFIED | services/paleotaskservice.cpp:374 | #815 L-02 修复不完整：同层 N≥2 任务时，先终态的 N-1 个任务的 busy 条目永不释放（计数归零闸 + 只摘自己）→ 层门永久锁死直到重开工程 | B-01 |
| F-02 | P2 | HIGH | workflow/aichatcontroller.cpp:502 | #815 去重修复把空 id 的相异工具调用误判重复静默丢弃 → 恰好复现其想消灭的「每 call 一答」400 | C-01 |
| F-03 | P3 | VERIFIED | workflow/aichatcontroller.cpp:517 | 同一修复顺手把「快照迭代」防御删成活容器迭代，与相邻注释直接矛盾 | E-01 |
| F-04 | P3 | VERIFIED | ui/pages/dataopsimportui.h:219 | #815 停拍修复的反向缺口：驱动拍唯一重启点在 enqueuePaths，队列清空后点「重试」→ 异步失败落 RetryWait 永久滞留 | D-03 |
| F-05 | P3 | VERIFIED | qgis/mappingartifactwriter.cpp:137 | L-10 修复缺口：GeoTransform/Projection/RasterIO 失败路径不删 .partial（仅 replace 失败分支删） | D-02 |
| F-06 | P3 | VERIFIED | ui/wellcomposite/trackops.cpp:37-131 | CSV 转义修复只盖 3 处：Curve 头/Core barrelNo/Symbol label/Strat 三段/Facies 三段仍裸拼 | D-01 |
| F-07 | P3 | VERIFIED | io/constraintstore.cpp:543 | copyAudit 以「页短即终页」判终，readHistory 跳过损坏行使页变短 → 快照静默丢后续事件 | A-03 |
| F-08 | P3 | VERIFIED | algorithms/singlefactor/localidw.cpp:383 | 512MiB 矩阵预算闸只盖 maxPoints==0 分支，显式 maxPoints 可绕过（脚本/批处理无界） | B-03 |
| F-09 | P3 | VERIFIED | qgis/coordinateeditor.cpp:122 | setMapCanvas 非 null 分支不复原功能 tooltip，启用按钮永远带「画布未就绪」禁用文案 | D-04 |
| F-10 | P3 | VERIFIED | ui/paleomainwindow_coordinates.cpp:72 | WA_DeleteOnClose 只删内层 QDialog，QObject 包装每次开对话框泄漏一个（预览宿主同型） | D-05 |
| F-11 | P3 | VERIFIED | 两入口共用 m_sitingPickTool | 顶替旧工具只 deleteLater 不发 cancelled → 隐藏中的坐标对话框永久孤儿化 | D-06 |
| F-12 | P3 | VERIFIED | qgis/wellattributestore.cpp:770 | L-12 索引 insert 覆盖使重复 (well,horizon) 行命中从首行变末行；同批重复 updates 走 addFeature 落重复行（旧行为是命中刚加行） | E-02/D-07 |
| F-13 | P3 | VERIFIED | workflow/mappingworkbench_polygonize.cpp:240 | 裸 m_processing 指针捕获进 worker 回调（#768/#769 家族新站点，无 QPointer） | C-02 |
| F-14 | P3 | VERIFIED | workflow/constraintfactorjobs_factor.cpp:1151 | 同步路径 publish 失败不调 cleanup（异步路径有 Pending 析构兜底），staging 五件套滞留 | C-03 |
| F-15 | P3 | VERIFIED | ai/chat/llmclient.cpp:203 | SSE 同事件多条 data: 行无 \n 直接拼接（W3C 要求换行连接），规范后端拆行载荷即损坏 Malformed | C-04 |
| F-16 | P3 | VERIFIED | services/previewdoc_xml.cpp:30 | 64 表预算实现为 visitor return false → 整预览硬失败 + 内部语错误文案（同函数 1000 行是软上限+issues 留痕口径） | B-02 |
| F-17 | P3 | HIGH | algorithms/faciesmapping/faciesqa.cpp:297 | L-11 包围盒预筛漏掉检测器 5（缺井覆盖）：单元×井全对 GEOSIntersects+geosDistance | B-04 |
| F-18 | P3 | VERIFIED | algorithms/stratgrid/upscale.cpp:87 | PW-128 域界外静默 return，格线穿越丢失无任何留痕（fallback 如实记录口径） | B-05 |
| F-19 | P3 | VERIFIED | services/previewwellbases.cpp:40 | #815 后置验身修复的快路径退化：错版/无 sha 文件从「0 解析快失败」变「两次全表解析+哈希后失败」 | B-06 |
| F-20 | P3 | VERIFIED | io/rasterpyramid.cpp:507 | L-25 修复不完整：锁内 ensure() 刷新 meta 后 lw/lh 不重算（LRU 命中分支 width 与 samples 可不一致）；磁盘直读路径插 LRU 无新鲜度复验。生产消费面为空（selfcheck/测试） | A-01 |
| F-21 | P3 | SUSPECTED | qgis/coordinateeditor.cpp:132 | setPoint 固定 9 位小数，投影坐标回显噪声（DESIGN 数值口径/placeholder 3 位） | D-08 |
| F-22 | P4 | VERIFIED | tools/qgis-spread-baseline.txt:1 | 头注「Qis::」拼写 | E-04 |

## 仅登记不修（低价值/信息级/接口级）

- A-04 P4：dataimport_document 临时目录失效文案张冠李戴（仅文案）。
- A-05 P4：inflight.h 离表→set_value 微窗口同键双跑（幂等，设计已知接受面）。
- E-03 P3：composition/depthconversion 同步路径 commit 失败不删未入库 staging
  （修复前既有口径分叉，涉两处架构对齐，登记待后续）。
- A-02 P3：sfpkgreader 嵌套 npz 跨数组无合计内存闸（生产调用面为空，接生产导入前再收口）。
- #815 修复零新增测试（结构性短板，本轮修复将补对应回归测试反哺）。

## 命中排除项（未进池）

全部 80 开放 Issues、L-01…L-23、主 checkout 在途文件（含 wellsection.cpp
orderWellsNearestNeighbor、welltable.cpp、lasparser.cpp、propfill.cpp 等）
——见各 Agent 报告排除段与 01-conflict-matrix.md。
