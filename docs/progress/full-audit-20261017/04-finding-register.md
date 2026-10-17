# Full Audit 20261017 — 04 Finding Register

原始报告：`findings/agent-{a..e}.md`（ID 前缀 = 报告 Agent）。去重收敛：
C-01≡D-03、B-01≡C-02、D-05≡E-03。主 Agent 亲自复核列 = ✅。

## 进入修复池（FIXABLE_FINDING）

| FID | 源 | Sev | Conf | 主审 | 模块 | 一句话 | 处置 |
|---|---|---|---|---|---|---|---|
| F1 | C-01+D-03 | P1 | VERIFIED×2 | ✅ | workflow/qgis | attributionLayer 8 格缓存任意驱逐使用中图层 → 归因/影响分析 UAF（#494 新代码，≥9 输入用户可达） | Fix 1 |
| F2 | B-02 | P1 | VERIFIED | ✅数学推导+数值对拍 | algorithms | velocitymodel fitV0k 雅可比 ∂T/∂k 第二项缺 1/k（v0·v 应为 k·v），带噪拟合系统性偏差 | Fix 1 |
| F3 | A-03 | P1 | VERIFIED | ✅ | catalog | addVersion supersede 级联把新 DERIVED 版本自身标 stale（registration/assetops 两生产触发点，removeStaleVersions 可删当前版本） | Fix 1 |
| F4 | A-01+A-02 | P1/P2 | VERIFIED | ✅ | io | constraintstore CRS 迁移还原路径：checkpoint 返回值丢弃 + 还原不删 -wal/-shm + 校验连接未关（Linux 嵌合库/Windows 恒失败） | Fix 1 |
| F5 | B-01+C-02 | P2 | VERIFIED×2 | ✅ | workflow | surfacegridding buildBarrierMask 半格采样+半开 floor 未随 #488/#496 统一到 supercover——陡斜屏障漏格、贴边回执与三引擎相反 | Fix 1 |
| F6 | D-01+D-02 | P2 | VERIFIED/HIGH | 抽查 | ui/workflow | #494 影响分析/反查/重算三按钮零门控（busy/层位/选中态不判）+ GUI 线程同步重 IO 无进度无取消 | Fix 1（门控+busy 守卫为主，异步任务化为最小可行） |
| F7 | B-04 | P2 | VERIFIED | ✅ | services | seismictaskservice_slice.cpp:760 workers 异常路径未 join → std::terminate（remediation 修了 attr 漏了 slice） | Fix 1 |
| F8 | B-03 | P2 | HIGH | — | algorithms | SGS 邻域截断 d²-only 比较器等距并列是 stdlib 实现定义，违背同 seed 逐位复现契约 | Fix 1 |
| F9 | A-04 | P2 | HIGH | — | io/workflow | geojsonaffine 临时配准路径 readAll+全量 DOM 无大小闸（registration.cpp:107 生产可达） | Fix 1 |
| F10 | E-01 | P2 | HIGH（CI 实证红） | — | tests/cmake | tst_wellsiting_kernel 比率断言无 RUN_SERIAL/perf 标签，CI 并行轮实证红、串行洗白 | Fix 1 |
| F11 | C-07 | P3 | VERIFIED | — | workflow | wellfactor layer_id 双插死代码 | Fix 1 快胜 |
| F12 | C-08 | P3 | VERIFIED | — | workflow | 反演统计均值分母含失败道（QC 高估稳定性） | Fix 1 快胜 |
| F13 | B-06 | P3 | VERIFIED | — | services | 层位聚类 nodata float==double（#165 族残留） | Fix 1 快胜 |
| F14 | B-08 | P3 | VERIFIED | — | services | PaleoTask percent 可 >100%、ETA 负值显示「约 1s」 | Fix 1 快胜 |
| F15 | E-02 | P3 | VERIFIED | — | tests | #496 edgeAligned 测试名与断言相反、注释域错、左边缘缺测 | Fix 1 快胜 |
| F16 | E-09 | P3 | VERIFIED | — | tools | run-target-tests.sh 固定 /tmp/<test>.log 互踩 | Fix 1 快胜 |
| F17 | D-05+E-03 | P3 | VERIFIED×2 | — | qgis | storeFidIndex 每要素全表扫描 O(K·N)（#496 F4 性能面） | Fix 1（循环外提一次） |
| F18 | A-08 | P3 | VERIFIED | — | domain | receiptsemantics 空 id 入 consumedIds + missing 进 accepted | 视容量 |
| F19 | B-09 | P3 | VERIFIED | — | algorithms | 显式软边界不走域可达过滤（与停线口径不一致） | 视容量 |
| F20 | D-08 | P3 | HIGH | — | ui | 自定义序 flush 不复查开关 + 悬垂指针集合 | 视容量 |
| F21 | D-07+D-09 | P3 | VERIFIED | — | ui/workflow | 影响报告落盘失败静默/「（详见 ）」残句 + 重算逐条失败被吞 | 并入 F6 簇视容量 |

## 记录不修（本轮）

- SUSPECTED 未验证类：A-05（copyAudit 分页跳事件）、A-06（lascache ragged 地雷）、B-05（semblance 守卫溢出）、B-10（lowfreq 闸）、C-09（预测回退）、D-10（相点索引重建）——留档给后续方向。
- 维护性/文档类：C-03（旧 IDW 回执形状双轨）、C-04/A-07（停线计划保守口径——#496 自注取舍）、C-05（屏障开关 displayName 匹配）、C-06（citations 失败混 committed）、D-06（factorcontour 英文串 i18n，量大）、E-04..E-08/E-10/E-11（工具/接线盲区族）——超出本轮容量或与 #484/#504 族整改重叠，如实留档。

## 排除（EXCLUDED_EXISTING_ISSUE / EXCLUDED_ACTIVE_WORK）

- D-04（constraintpage_constraints 裸 QMessageBox）：文件在主 checkout 活跃未提交簇内 → EXCLUDED_ACTIVE_WORK。
- 其余 33 issues + PR#506 + audit-remediation 已改文件根因重合项：见 01-conflict-matrix.md；各 agent 报告的排除核对记录共 16+ 项。
- B-04 特注：remediation 分支修了同根因的 attr.cpp 两处但未覆盖 slice.cpp——master 上该文件无人持有，本分支修（PR 中注明合并时可能有轻微重叠）。
