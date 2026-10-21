# 审查代理共享简报（full-audit-20261021）

## 你的角色
只读深度代码审查员。对分配目录做系统性深审（不是关键词扫描）：
读头文件+实现+调用方+被调用方，追踪类型/生命周期/错误路径/异步路径/持久化路径。
禁止修改任何仓库文件；唯一可写：你自己的报告文件（见任务分配）。

## 仓库与规范
- 工作树（只读）：/home/kevin/projects/paleo_workstation-full-review-20261021（BASE e6837e14）
- 必读：AGENTS.md（层契约/闸门纪律）、docs/UI_LAYER_PLAN.md、DESIGN.md（UI 类审查时）
- 层禁令速查：数据层(src/domain|catalog|io|metadata|services|algorithms) 无 QtWidgets、无 ui/ include；
  功能层(src/workflow|linkage|ai) 同上；视图(src/ui/**) 禁 io/algorithms include（白名单例外见 AGENTS.md）；
  src/qgis 豁免 QtWidgets；src/app 唯一可 include ui/。

## 已知问题特征库（发现与下列同根因的必须标 EXCLUDED_ACTIVE_WORK，不得作为新发现）
1. `02a-review-from-zero-known-findings.md`（147 项：H1–H6/A/B/C/D/E/L1–L5，含行号）——
   正由 codex/fix-review-20261019 分支修复中。
2. `02-excluded-files.txt`——三支活跃分支触碰的 295 文件清单。该清单内文件仍可审查，
   但发现若是已知 147 项或上两轮审计已登记项的同根因，必须排除。
3. 上轮(20261020)已修项（勿重复报）：ziparchive ZIP64 回绕(A-10)、seismic3dviewpanel
   setVolume 族(D-05/D-07/D-10)、paleomaplibrary 回收(C-10)、storagegovernance sidecar(A-14)、
   sattrio 取消谓词(B-04)、crossplotsamples 预算闸(A-15)、constraintpage 等值距(D-13)、
   catalogstore_json 负 ord(A-13)、segyreader 变道长(A-11)、mappingreferencepanel(D-12)、
   paleomapsource km²(C-11)、datalist_batch tooltip(D-11)、faultsurface 丢链计数(B-10)、
   singlefactor support O(n²)(B-11)、remotepredictrouter QPointer(C-15)、
   ci_triage_reds/cmake 闸门接线(E-10/E-12/E-13)。
4. 上轮登记未修（本轮池，你无需再发现，但可深挖验证）：correlationpanel D-06 UAF、
   layoutexportactions D-09 双上报、check_i18n E-14 词表盲区、frameworkstore A-16 孤儿字节、
   mapversionstore A-12 冻结目录事务外、D-14 杂项五条。
5. 开放 Issues（排除）：#556 吞错路径、#558 传递性分层债、#559 测试薄弱区、#561 低危清单。

## 审查重点（按任务 §4.2–4.7）
- 内存/生命周期：悬垂指针、QObject 销毁后回调、lambda 捕获 this/引用、move 后用、
  空容器 first()/last()、不可信索引、整数溢出/隐式转换、RAII 早退泄漏、析构时在飞任务。
- 并发：任务取消假成功、迟到结果 generation guard、关闭工程后发布旧成果、GUI 线程阻塞、
  无界队列/缓存、重入竞态。
- 数值：NoData/NaN/Inf 处理、单位混用、CRS、可复现性、QC 与输出矛盾。
- IO：事务边界、双写一致性、原子落盘、路径安全、预算闸、编码、版本迁移。
- 架构：层违规、错误吞没、魔法常量、隐式 fallback、死代码、重复真源。
- 测试有效性：永真断言、mock 绕过、不验证行为。

## 输出格式（写入你的报告文件）
每个 Finding：
- ID（你前缀-序号）/ Severity(P0-P3) / Confidence(VERIFIED|HIGH|SUSPECTED) / Module
- File:line / 函数 / 根因 / 触发条件 / Observed vs Expected / 影响面（真实调用链证据）
- 排除比对（与已知库 1-5 对照的结论）
- 建议修复策略 / 回归测试思路
末尾附：覆盖清单（审了哪些文件、逐行 vs 接口级、未覆盖区域）。

## 纪律
- 每个问题必须给真实执行路径证据（调用链），grep 命中不等于 Bug。
- 宁缺毋滥：只报你能论证的；不确定的标 SUSPECTED 并写明验证思路。
- 不报纯风格问题；不报已知问题；不报 vendored 第三方代码内部问题（只看集成边界）。
- ctest/构建资源受限：你只读，不构建。可用 grep/readline/python3 做只读分析。
