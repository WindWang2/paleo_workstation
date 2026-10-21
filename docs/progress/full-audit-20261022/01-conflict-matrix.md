# Full Audit 2026-10-22 — 01 外部工作排除矩阵

## Open PR（1 个）

| PR | 分支 | 范围 | 处置 |
|----|------|------|------|
| #627 | codex/wells-during-basemap-refresh | `src/qgis/onlinebasemap.{cpp,h}`、`src/qgis/previewmapcanvas.cpp`、`src/qgis/qgiscanvascontroller.cpp`、`tests/tst_onlinebasemap.cpp`、DESIGN.md、docs/progress/wells-during-basemap-refresh.md | EXCLUDED_ACTIVE_WORK——瓦片慢/失败时前景先绘井点井名。这些文件的同类问题只登记不修改 |

## 未合并远程分支（1 个）

| 分支 | 说明 | 处置 |
|------|------|------|
| fix/master-green-20261021（c23427be） | 标题与已合 #611 相同但内容不同（94 文件差异），系上一会话遗留 | EXCLUDED_ACTIVE_WORK（视为陈旧外部分支）——只读，不重放、不修改 |

## Open Issues（16 个，外部所有）

来源两批：review-2026-10-01（#556/#558/#559/#561）与 review-2026-10-19（#582–#597）。
正文全文快照：`/tmp/open-issues-bodies.md`。特征摘要（用于去重，凡与下列根因重合的新发现一律标
EXCLUDED_EXISTING_ISSUE）：

| Issue | 标题特征（根因指纹） |
|-------|----------------------|
| #582 | ziparchive.cpp ZIP64 directoryOffset+directorySize 加法回绕穿闸 |
| #583 | 空 sqlite 库被当权威空库放行，恢复链不可达 |
| #584 | 井剖面派生 XML 相别层级写死微相读回塌缩 |
| #585 | 导航树测试索引断言未更新（tst 二进制段错误） |
| #586 | master 源码闸 11 项全红、合入缺门禁兜底 |
| #587 | catalog 开库先写后验、索引对账放行缺项、幻影链接行回流、只读降级未接线、原子写不 fsync 目录 |
| #588 | IO 大文件无大小闸整读、写入不校验非原子、登记失败留孤儿、临时目录清理前缀错判、SHA 比对大小写敏感 |
| #589 | 算法：空值哨兵三套口径、协克里金缺总基台闸、分箱 int 强转、取消谓词契约反相、ONNX 锁外读模型名 |
| #590 | UI：同步重活阻塞 UI、悬垂指针裸捕获 this、裸 QMessageBox 绕统一通道、3D 换体不清 GL 状态（24 项） |
| #591 | 构建与 CI：闸门红单登记与实测不符、基线超标未收敛、检查器脚本缺陷（9 项） |
| #592 | 数据层中文文案无 tr 入口、i18n 闸门只扫控件入口（5 项） |
| #593 | 领域模型空态/哨兵口径：空棒返回 0.0、文档哨兵与实现不一致、时深表单点零时间（4 项） |
| #594 | 算法健壮性与死代码：8 位计数回绕、KD 树注释与实现相反、后退弧两分支同体、零消费模块（13 项） |
| #595 | IO 健壮性残留：int 减法溢出 UB、失败不回滚、析构不断信号、schema 注释漂移（8 项） |
| #596 | UI 交互细节与测试缺口（37 项）：索引式断言、坏数据形状、绘制细节与文案行为不符 |
| #597 | 外链资产重定位后预览指向旧死路径并重复追加版本 |
| #556 | mappingworkflow 产物四级失败仅 qWarning 静默；ErrorHub 采用率低 |
| #558 | 传递性分层债：paleomainwindow.h/workflows.h 扇出根 |
| #559 | 测试薄弱区：主窗 attach、ui/pages 32 页、dsp 无测试、裸墙钟断言 |
| #561 | 低危清单：fencewidget 线性扫、井表 readAll 峰值、ShaCache mtime-only、superbuild 无 CI、ASAN 观察档、io 错误文案语言混用 |

## 已确认本任务可修范围

- 上述清单之外的**新发现**问题（含新合入 #603–#626 代码引入的缺陷）。
- 已知 Issue 文件的**外围调用点**可以审查，但不得对已分配根因实施重复修改。

## 交付前复查（2026-10-22 Phase 8）

- origin/master 仍为 b301f2e8（未前进）。
- 新开 PR #628（codex/indexed-data-mgmt-section-snapping，索引式数据管理 +
  连井剖面吸附）：与本轮 diff 共享 3 文件（dataimport_folder.cpp /
  sectionworkbench.cpp / file-budget-baseline.txt），`git merge-tree`
  零冲突标记，改动 hunk 不相交，语义面（导入外链默认/命名/吸附）与本轮
  （行序契约/嗅探收敛/闸门收缩）不重叠——标记 EXCLUDED_ACTIVE_WORK，
  本轮未触碰其根因面。
- 开放 Issues 集合不变（#556–#597 外部所有）。
