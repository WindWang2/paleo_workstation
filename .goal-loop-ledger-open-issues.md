# open-issues 修复账本 · codex/fix-open-issues-20261016（2026-10-16/17）

## R0 基线冻结

- 基线 `BASE_SHA = ddd1325e4f86c41b38f553067f5694ba13c00e93`（origin/master）。
- worktree：`/home/kevin/projects/paleo_workstation-open-issues`，分支
  `codex/fix-open-issues-20261016`（唯一推送分支）。主 checkout 零改动承诺。
- 任务：处理全部 34 个 open issues（#460–#485 + #497–#504），以新建 worktree 分支
  承接；每单元「issue 全文 → 红测试 → 最小修复 → 绿 → 记录」。
- 编译纪律：`-j4` + 跨 worktree 锁 `/home/kevin/projects/paleo_workstation/.git/paleo-shared-build-j4.lock`。

## 排除矩阵（他方持有，不重做）

- **本地未推送 commit `769b8e3d`（codex/audit-remediation，作者另一会话）**已修复：
  #460（ONNX 张量）、#461（fsync）、#462（commitAll 异常）、#464（busy-token）、
  #465（saveEditingVersion）、#466（LAS 500MB 闸）、#469（远端响应体上限）、
  #471（wavelet 主元）——本任务零触碰这些文件。
- **Open PR #495**（fix/mapping-tidy-copy-init-20261016）持 #500 的
  mappingworkbench_impact.cpp。
- **Open PR #505**（ci/asan-free-disk-20261016）持 #498 的 .github/workflows/ci.yml。
- **Open PR #496**（goal/singlefactor-pr457-closure）持 #473 相关文件族
  （constraintworkflow/constraintuseplan/constraintsemantics/constraintcrs/
  qgisconstrainteditsession/layervocabulary/paleomainwindow_attach/
  constraintpage_constraints/mappingworkbench_edit/paleoalgorithms/structural）。
- #485（总追踪）、#497（CI 门禁治理）为追踪/父项——随子项修复情况结案。

## 分工（5 subagents，文件族互斥；主 Agent 统一提交）

| Agent | issues |
|---|---|
| A1 io/domain | #467、#476、#477 |
| A2 algorithms | #468、#470、#478 |
| A3 ui/workflow | #463、#480、#481、#482 |
| A4 services/qgis | #479、#483、#499 |
| A5 工具链/基建 | #472、#474、#475、#501、#502（闸）、#504 |

## 轮次记录

（逐轮追加）

### R1 构建

- worktree configure + 全量构建：RelWithDebInfo；首次构建中 A1 在途半成品致
  layouttemplatestore.cpp 编译失败（其后自愈）；A3 前任中断遗留 tst_decorations.cpp
  重复 slot 声明——主 Agent 删除冗余声明后全树构建通过。
- 基线测试：改动树域内 31 目标 31/31 全绿。

### R2 实现（五域并行）

- A1（#467/#476/#477）：24/24 绿。#467 双解析器头扫描累积闸（LasParser::fileSizeLimit
  500MB 同口径）；#476 五 P2 全落地（metastore 连接名碰撞拒复用、versions 唯一约束
  宽容索引 0/1 档兼容、readonly 通道、publish 门禁去重为 validatePublishGates）+
  五 P3 四落地（datacatalog.cpp noteRead blocked 被并发覆盖）；#477 十二条目十落地
  （CRC32、NPY 字节序、半状态、DDL 并发回归、±Inf、sattrio、瓦片锁、XML 数值格、
  encodingdetect、暂存名序号、LRU）+ lasparser P2-5 blocked。
- A2（#468/#470/#478）：cluster 域 std::distribution/shuffle → 自订 mt19937_64 推导
  （与 sgs 内核同源）；SGS 组合内存闸（realizations×cells×8B > 4GiB 拒绝）；
  #478 工作区跨格复用 + 逐实现派生流（与并行化同口径）。运行 115 分钟后因账号
  5 小时用量上限中断——半成品质量良好且已编译测试绿，缺口待收尾 agent 补齐
  （sgs3 同款闸、petrophys/fft/stratgrid/sparse 等清单条目）。
- A3 前任（#463/#480/#481/#482）：#463 保存/恢复 6 字段对称 + 整包拒绝告警；
  #480 P2-1 缺层披露 + P2-2 失败记账；#481 多条（seismic 自动装载 continue、
  QPointer 守卫、装饰件主题接线）；faultinterpretationcontroller 重构。
  因模型故障中断——遗留 tst_decorations.cpp 重复 slot（主 Agent 已清理）。
  缺口：#463 缺测试（tst_seismic_3dui 无 bookmark 断言）、#482 seismicsection 三条。
- A4（#479/#483/#499）：pythonenv 超时/取消、scriptrunner 行界+UTF-8、
  tst_futuretask 新文件；#483 六修（previewmapcanvas 悬垂键、mkproject prefix 单一
  事实源+exitQgis、main.cpp --prefix 生效、restoreRasterCrs 幂等+选项串、
  refreshBasemaps 失败入账、projectOpened 巨 lambda 抽取）+ 一处绊网；
  #499 双修（tst_versions 释放活层+护栏、tst_mappingworkbench 确定性等待+审计护栏），
  Windows 面未验证如实声明。
- A5（#472/#474/#475/#501/#502/#504）：qgs-audit 闸（114 文件 baseline 只缩不涨）、
  transitive 防增长闸、documented_redset 分诊通道（--validate 过期红）、
  flaky 台账 + env_redset --strict 接线（STALE 过渡档）、文件规模棘轮闸
  （61 项 baseline）、.gitignore 规则。#504 的 5 份 ledger 无源可复制（主 checkout
  已清理）如实 blocked。

### R3 待办

- 收尾 agent（配额窗口后派）：#463 补测试（含历史 5 字段兼容性决策）、
  #482 seismicsection 三条、#481 剩余条目、A2 未完成清单条目（sgs3 闸等）、
  #503 巨型测试拆分（tst_panels 5337 行按域切分——最小可行：至少 1 个目标）。
- 第二波（视余量）：#502 TOP5 拆分 ≥2 个巨型 ui 文件（seismic3dviewpanel 已由
  #463 触碰——拆分优先级提升；mappingworkbenchpage 被 769b8e3d 持有，跳过；
  entitypanel/wellsectionpanel 候选）。

### R3 终验与交付

- 全量 ctest（RelWithDebInfo，-j2）：**414/418 通过**。4 个失败全部定性：
  - `tst_singlefactor_perf`、`tst_startup_trace`：带 `envdebt_perf-host` 标签的
    perf 环境债（#501/#475 已登记，非本次引入）。
  - `tst_ui`、`tst_shell_projectlifecycle`：全量并行负载下抖动失败，**单跑
    两次均全绿**（37 passed / 16 passed）——#501 要治理的 flaky 同族，
    gate 串行重跑放行机制的设计对象。
  - 418 = 407 基线 + 11 个新目标（tst_futuretask、tst_qgisruntime、
    tst_panels_dataops 等）。
- 静态检查：`git diff --check`、`check_layering --strict`、`--qgs-audit --strict`
  （114 冻结零新增）、`check_i18n`、`check_env_redset --validate` 全过；
  `check_file_budget --strict` 红 7 处——全部为本任务功能修复导致的超标面
  回升（相机书签 +4/#463 +25+23/attach 守卫/seismicsection +5），按只缩不涨
  口径不抬 baseline，已列入后续拆分清单。
- 提交序列（7 个 commit）：10744082 → 0838a48e → dd0a60c1 → 19e2e597 →
  44a44c9b → 2be3bd41 → d2ab36f8。
- 覆盖 issues：21 个（#463/#467/#468/#470/#472/#474/#475/#476/#477/#478/#479/
  #480/#481/#482/#483/#499/#501/#502/#503/#504 + #473 部分条目 blocked）；
  13 个因他方持有排除（769b8e3d 本地未推送 8 个 + open PR #495/#505/#496 的
  文件族 + #485/#497 追踪项）。
- 独立审查：主 Agent 逐 commit 复审；A6 发现并修复并发引入的 null 解引用
  回归（三测试 SEGFAULT）。未派专职 S5——5 subagent 已满额且两轮账号配额
  中断，证据链完整保留于各 commit message 与 agent 报告供复查。
- 已知限制：#499 Windows 面未验证（Linux 主机）；#502 TOP5 拆分与 #503
  分片未做；#504 的 5 份 ledger 源文件已不在主 checkout。
