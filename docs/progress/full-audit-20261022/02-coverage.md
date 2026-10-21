# Full Audit 2026-10-22 — 02 审查覆盖清单

覆盖口径：增量窗 093346e6..b301f2e8（20 PR，247 文件 +15766/−3621）逐文件/调用链
深审；全仓其余部分按接口/契约层覆盖（历轮 20261019/20/21 深审 + review-from-zero
已建立逐行基线，本轮以增量为主战场）。各代理的文件级覆盖清单（审查方式/风险级/
测试对应/未核查区）见 03-review-{A,B,C,D,E}.md 第 1 节，本文件只做汇总对账。

## 增量窗覆盖（按目录）

| 目录 | 增量文件数 | 深审方式 | 覆盖代理 |
|------|-----------|---------|---------|
| src/io | 23 | 全部逐行 + 调用链（#603 主体） | A（主）+ C（契约面） |
| src/ui/wellcomposite | 24 | 全部全文/逐行（#626/#614/#615/#619） | D |
| src/ui/pages | 15 | 新改文件深读 + 既有面接口级（#618/#616/#621） | C+D |
| src/ui/datapreview | 14 | 全部全文（#616/#619/#625） | D |
| src/workflow | 13 | 全部全文 + 六页主线核查（#603/#618/#610） | C（主） |
| src/qgis | 9 | #624/#625 增量逐行 + 排除面（PR #627 文件）契约级读取 | D+B |
| src/services | 8 | 全文（welllogset/paleotaskservice/jobrunner/batch 面） | B |
| src/domain | 8 | 新文件全文 + 变更段逐行 | A |
| src/ui/dialogs、seismic3d、help 等 | 12 | 逐行/逐段 | D |
| tests | 43 | 逐文件断言计数 + 抽读（弱化/永真/mock 绕过扫描） | E |
| cmake / .github / tools | 19 | 逐行等价对拍 + 检查器实跑 | E |
| src/app、src/metadata、src/catalog | 6 | diff 深读（#622/#623） | A+D |
| vendor（sbm 头） | 1 | diff + 消费点 + PATCHES 对账 | E |

## 全仓接口/契约层复覆盖（无增量模块）

- src/ai（零 diff）：tileinference/onnxpredictionservice/remotepredictrouter/
  wellfaciesservice/llmclient/aichatcontroller 契约+线程面复读（C）。
- src/linkage（零 diff）：四件全文复读（C）。
- src/metadata（零 diff）：store 族事务/生命周期签名面抽查（A）。
- src/algorithms 主体：8 个数值核逐式推导复核对（B）；大文件按 #589 已录条目覆盖。
- 并发/任务系统四件（PaleoTaskService/JobRunner/BatchQueue/futuretask）全文（B）。

## 未核查区（如实申报，继承自各分报告）

- dlisparser/lisparser/lasparser 内部（无增量；上轮在册）。
- segyreader/sfpkg/ziparchive（#582/#587/#588 排除族）。
- workflow 未变更大家族（evolution/realization/datagraph 等）——继承上轮「未逐行」。
- tst_panels 全量用例逐条、Windows 侧（注册表/OSGeo4W）——环境不可达。
- 双主题暗色实测（只读约束；token 出口静态核对替代）。

## 主代理交叉复核

- importrecovery/welllogread 接缝（调用方完整性）、#617 手势、#625 底图委托、
  datalist_logsummary 树刷新不触发解析——证实或减轻疑点。
- B-06 证伪复核（neighbourhood 严格比较 + 既有平坦用例）。
- tst_onlinebasemap/tst_datapreview/tst_assetpaging 基线红复现与归属判定。
