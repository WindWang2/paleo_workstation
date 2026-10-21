# 优先级与修复计划（full-audit-20261021）

## 优先序（按 §5 规则：无 P0/P1 → P2 → 高影响 P2/P3 → 已证热点 → 结构 → 细节）

### 批 1 — P2 正确性（每项独立 commit + 红绿测试）

1. **B-1** Ricker 公式（io 数值正确性，一行修复 + 频域回归）
2. **C-1** retryLast 漏 tools（功能缺陷，对齐 sendRound + 请求体断言）
3. **C-2** 一致性报告 EXDEV（暂存目录进工程 + 失败路径回归）
4. **C-4** 反演 /tmp 残留（dock 终态清理 + 回归）
5. **C-3** 导出 mutationSeq 误判废（三链对齐 #480 P3-3 口径 + 无关写不废回归）
6. **A-01** ShaCache 写放大（落盘去抖/节流 + 写盘次数回归）

### 批 2 — P2 契约/闸门（需设计判断）

7. **B-2** columnDistances 单位：优先「米换算」（SgyCoordinateMapper 仿射）；
   若换算面过大则退「头注如实 + 删合成分支」最小诚实修复
8. **E-2** qgs 闸扫描面：数据层 qgs include 词表规则 + algorithms/linkage/services
   初冻基线（对齐 qgs-audit 初冻机制，不弱化任何既有闸）

### 批 3 — P3 + 遗留池（小步快跑，按域分簇）

9. K-3 frameworkstore flush 孤儿；K-2 双上报；K-1 correlation UAF 守卫
10. C-5 map 重载 no-op 短路（含 wellsection 查询路径止血）
11. A-02 dlisparser pad 钳制；A-04 转换队列 RAW 复核；A-05 cachecore 长度不变量
12. D-01/D-02/D-03 UI 三小件；D-04 extent 守卫
13. B-3 相干 slab 读；B-4 进度账本
14. C-6 llmclient 非流式 tail；E-6/E-7/E-8 检查器三小件

## 纪律

- 每簇 commit 前先写红测（可测的），修复后转绿 + 同域回归
- 不放宽断言/不改 golden 迎合错误输出/不吞错
- 基线文件只在 E-2 初冻（新扫描面）时新增，不动既有冻结值
- 文件所有权：修复点均避开 10 个开放 PR 的 hunk（04 表已核）
