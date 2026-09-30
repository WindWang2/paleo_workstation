你在 paleo_workstation 项目执行一轮大体量「继续开发 + 性能完善」wave
（预期工作量为多子任务并行 × 多轮迭代，按 ZCode teamwork-preview 模式执行）。
本文件自包含，但仓库内文档是权威细节来源——动手前先读
AGENTS.md / docs/PALEO_QGIS_PLAN.md / DESIGN.md / TODOS.md /
docs/perf/{ARCHITECTURE,BASELINE,BENCHMARKS}.md / docs/progress/*.md。

【执行方式】teamwork-preview：lead 负责拆解、分派、冲突裁决与验收。
建议并行 worker 划分（可调整，避免同文件写冲突）：
  A) 地震链路性能深化（src/qgis + src/services + src/linkage，不碰 ui）
  B) IO/缓存/目录性能（src/io + src/services + src/domain）
  C) 编图域功能深化（src/workflow + src/ui/pages + src/ui/edittools）
  D) 井综合/连井剖面补全（src/ui/wellcomposite + src/ui/seismicsection）
  E) 测试与回归门扩展（tests/ + docs/perf）
  F) 收尾集成：进度文档、TODOS 对账、PR 提交（lead 独占）

【环境】仓库 /home/kevin/projects/paleo_workstation。C++20，Qt 6.11.2，
QGIS 4.2.2（headers /usr/include/qgis，libs -lqgis_core -lqgis_gui；
qgis_app 一律不链——app-only 类（QgsVertexTool/QgsVertexEditor 等）证据见
src/ui/edittools/vertexeditortools.cpp 头部注释），moc=/usr/lib/qt6/moc，
CMake+Ninja+ccache，构建目录 build/，测试 QT_QPA_PLATFORM=offscreen + QTest。
CMakeLists.txt 顶层慎动：新测试一律 `add_paleo_test(name [LIBS ...])`。

【基线注意】本 wave 以当前 master HEAD 为基线。父 worktree 可能有未提交
的收尾改动（约束线词表 direction_line/break_line、顶点吸附、相纹理填充、
quiet 任务标志、编图器 setLayout 修复等）——worktree 从 HEAD 切出，
不包含那些 WIP；若依赖其中某项，先在主 worktree 落 commit 再同步。

【第一步：建 worktree（lead 执行，子任务共享）】
cd /home/kevin/projects/paleo_workstation
git worktree add ../pw-deepen -b wave/deepen-perf
cd ../pw-deepen && cmake -S . -B build -GNinja -DCMAKE_BUILD_TYPE=RelWithDebInfo && ninja -C build

【第二步：建立可测量基线（先于任何优化，写证据）】
QT_QPA_PLATFORM=offscreen ./build/paleo_selfcheck perf --json /tmp/baseline.json
ctest --test-dir build -j1   # 记录全绿基线；已知 -j2 QSettings 竞态见 TODOS
真工区口径：docs/perf/BASELINE.md §1 + docs/seismic/BASELINE.md 已有数字，
优化项必须给出 改前/改后 对照（合成夹具 selfcheck + 真工区复测各一份）。
**禁止无基线的「感觉快」优化**——每项性能交付附带可复现数字。

【产品边界（不可违反）】
- 分层契约（AGENTS.md 表）：每个 src/ 文件头三行 `// 层：<…>`；
  ui→io 白名单仅 lasdoc.h + metadata 四头；功能/数据层无 QtWidgets；
  检查器 `python3 tools/check_layering.py --strict` 必须绿。
  新顶层模块先 `scripts/new_module.sh` 登记词表。
- 视觉/UI 决策服从 DESIGN.md；地图域颜色属 QGIS 样式域例外。
- 交互内嵌取数任务一律标 quiet（PaleoTaskService::start 的 quiet 参数，
  约定见 src/services/paleotaskservice.h）——短任务不得触发任务中心弹出；
  用户主动长任务保持 loud。
- 顶点/绘制手势一律先 `e->snapPoint()`（canvas snappingUtils 已配
  AllLayers 顶点+边 10px），吸附反馈用 QgsSnapIndicator。
- vendor/ 目录原则上零改动；确需改上游先写 vendor/sbm/PATCHES.md 条目。
- 失败如实报错，不静默回落、不造假数据、不伪造进度。

【交付物 — Track A 地震链路性能（worker A）】
A1. SBM 引擎剩余入口接线（TODOS P2「SBM Engine 剩余入口」）：
    QuickOpen 秒级首屏、ReadTimeSliceTiled 瓦片渐进、progressiveLod +
    SetActiveLod、ReadVoxelWindow。接 SeismicTaskService 现有模式
    （quiet 任务 + CancelToken↔PaleoTask 桥 + 条目锁单线程独占 + LRU）。
    文档：docs/seismic/{ARCHITECTURE,SECTION,3D}.md。
A2. 三维拖动链路取数合并：拖动期 LOD 粗层 + 松手 ~0.35s 升层的现契约下，
    审视 startSliceExtraction/startVoxelWindow/startLodSwitch 的防抖与
    在途取消是否仍有冗余请求；3D 视图 fps 基线入 docs/seismic/BASELINE.md。
A3. 剖面条拖动与时间切片瓦片的统一渐进策略复核：sf3c/sf3p/直读三后端
    一致的空态/取消/回落语义。
A4. 966MB 真工区按 docs/seismic/BASELINE.md 手册复测并刷新数字。

【交付物 — Track B IO/缓存/目录（worker B）】
B1. 大 LAS `lasAt` 同步解析的 UI 线程悬崖（TODOS P3）：correlation/连井
    剖面链路改 PaleoTaskService quiet 异步 + 结果回填；大文件不再阻塞
    GUI 线程。带 59MB 级 LAS 夹具的阻塞时间对照数字。
B2. 文件夹导入真进度条（TODOS 已定「忙碌光标+状态栏」契约的升级项）：
    文件计数/ETA/可取消，复用现有 ImportQueueItem 状态机。
B3. 金字塔瓦片消费侧落地：导入侧 ensureRasterPyramidVersion 批量接线 +
    预览/编图栅格大图瓦片化渲染（RasterPyramidService 路径面已备）。
B4. catalog.sqlite 查询索引（TODOS P3，ADR 0056）：先做 10k/100k 资产
    夹具实测——打开/列表/链接查询若未劣化则记录「未达触发条件」收档，
    若劣化则落 sqlite 可重建索引（catalog.json 仍为权威源）。
B5. ctest -j2 跨二进制 QSettings 竞态根治（TODOS P4）：定位共享落盘点
    （便携 ini 路径见 TODOS wave/wellcomposite 决策记录），隔离后 -j4 全绿。
B6. SEG-Y 变道长文件支持评审 + 坏道跳过放宽（TODOS：现仅固定道长生效）；
    变道长布局的索引契约变更需 docs 记录。

【交付物 — Track C 编图域深化（worker C）】
C1. 约束线语义落地：单因素词表已切 direction_line/break_line，但
    paleo_constraint_idw 目前不区分 type——本项落地语义差分：
    break_line 作为贯通屏障（ROI 裁剪/距离绕行），direction_line 作为
    各向异性权重方向场。算法改动必须有 docs/ALGORITHM_AUDIT.md 条目更新
    + algorithmbase harness 用例；若评审结论是「暂不分语义」则记档收工。
C2. 相界地质语义类型（TODOS P2）：相界线 kind 字段（整合接触/尖灭/
    相变/断层切割），先单类型跑通：属性 schema → 编辑工具行为钩子 →
    符号映射（规范图式，resources/geology/ 现有 SVG 体系）。
C3. 井类别符号扩展：Q/HS 1011—2016 表 K.1 的 12 类探井符号已有通用
    「探井」落地（外细环+实心盘，0.73 外径比）；本项扩展为数据字段驱动
    的全集映射（预探井双环/评价井单环/正钻井半填/发现井星/工业油流绿/
    工业气流红斜线等），无类别字段时保持通用符号。
C4. layerId↔assetId 关联（TODOS P2）+ 图层创建时间（P3，schema 变更
    需在报告里标注评审点）：LayerPropertiesDialog 业务页激活。
C5. 非 IDW 单因素引擎（TODOS P3）：welldist 距离变换 / confidence
    预测直取 / strathick isopach 双栅格链——注册表「待接入」标签逐项
    实装或记档为什么不接。

【交付物 — Track D 井综合/连井剖面补全（worker D）】
D1. wellcomposite 壳侧接线递延（TODOS wave/wellcomposite 决策记录）：
    derivedDocumentReady → catalog DERIVED 版本登记；
    parseDeviationSurvey/parseTimeDepthTable 接入装配链。
D2. 连井剖面生命周期回归打磨：清除剖面连线/对话框/dock 显隐语义刚改过
    （attachSections），本项做边界情形穷举测试（切体/换层位/关工程/重开
    对话框期间收到事件）。
D3. 打印对话框原生接线（TODOS 递延）或记档为何继续走 QPdfWriter。
D4. 简化版编图 composer（TODOS P3，触发条件=完整设计器实测过载）：
    先做一轮真实可用性评估写结论；未过载则不建，记档。

【交付物 — Track E 验证扩展（worker E，贯穿全程）】
E1. 每个新功能子任务：TDD 红→绿；测试进对应 tst_* 或新 add_paleo_test。
E2. 性能项：selfcheck perf 组新增/更新基准 + 预算断言；
    tst_perf_regress 比率门保持绿（必要时更新 ratios.json 并注明理由）。
E3. 已知坑位（勿踩）：QgsMapCanvas 析构序（PreviewMapCanvas 决策①②）、
    隐藏画布 30px 假几何（D2.8 守卫先例）、QLatin1String 中文乱码
    （一律 QStringLiteral）、QPointer 前向声明需完整类型处落 .cpp、
    便携 QSettings 路径绕过 XDG（测试内独立 projectName 隔离）、
    QgsPointLocator 索引惰性构建（测试需 qWait 探就绪）。

【收尾 — Track F（lead 独占，全部子任务合并后）】
F1. 全量验证：ninja 全量构建零警告新增；ctest -j1 全绿；
    check_layering --strict 绿；selfcheck perf 全绿无新增 OVER BUDGET。
F2. 文档：docs/progress/*.md 更新本 wave 交付；TODOS.md 对账——落地项
    移入 Completed（含 commit 号），未达触发条件的项注明评估结论；
    新增递延项按现有格式登记（What/Why/Pros/Cons/Context/Effort/
    Priority/Depends on）。
F3. worktree 分支分批 commit（每 worker 一串语义化 commit，仓库惯例
    trailer：Generated with [Devin] / Co-Authored-By 行照抄现有 log），
    最后：
      gh pr create --title "wave/deepen-perf：域深化 + 性能完善" \
        --body "## Summary\n<分轨交付要点>\n\n#### Test plan\n<checklist>"
    PR 描述引用本 prompt 文件名。

【完成定义（全部满足才算完）】
□ Track A–D 各交付物：实装 / 有据记档（未达触发条件的附实测数据）二者其一
□ ctest 全绿、tst_perf_regress 比率门绿、selfcheck perf 无新增超预算
□ check_layering --strict 绿；无新 include 方向违规
□ docs/progress 更新、TODOS 对账完成
□ PR 已创建，body 含分轨摘要与测试清单
□ worktree 内无未提交残留（git status 干净）

【汇报格式】每 worker 收尾报：commit 列表、测试数（新增/通过）、
改前/改后性能数字（性能项必附）、给 lead 的集成注意、未竟项+理由。
lead 终报：PR 链接、全量验证结果、TODOS 对账 diff 摘要。
