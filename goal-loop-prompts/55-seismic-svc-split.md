# Goal-Loop 方向 55：seismictaskservice 拆分——5,041 行 God-service 解体

## 背景（实测事实，勿再勘察；行号为 2026-10-05 master `adf2be7` 口径）

- `src/services/seismictaskservice.cpp` **5,041 行**（头 834 行）——
  **全仓最大文件**。19 个 `start*` 公共入口（h:259-767：索引/切片/
  剖面/转码/QuickOpen/瓦片/体窗/LOD/探测/追踪/体传播/属性切片/
  属性体化 + startBounded/startPagedTranscodeImpl 内部基座），
  外加解释工具（拾取/断层/会话）、内存治理、≤4 并发闸、LRU、
  3D 体视预览——至少五个正交职责挤一个类。
- **拆分先例已验证**：方向 20 把 `workflows.cpp` 4,086→153 行、
  `datapreviewtabs.cpp` 5,249→1,690 行（含 internal.h 1,573）——
  「按 start* 族/职责切 TU + 聚合头」模式可复制。
- **行为红线（拆分必须原样保留）**：任务世代号+请求号双守卫、
  supersede/cancel 语义、`SemaphoreGuard` RAII（cpp:5026-5030）、
  `m_evictionZeroCond` 清零等待（io/cachebudget.cpp:25-36 同族）、
  `deleteLater` 口径（`:635` 的同步 delete timer 是 RUNTIME-04
  已知问题——属方向 48 修复面，本方向拆分时**顺带迁移不改语义**，
  记协调注记）。
- **配套面**：attr-volume（PR #208 +1,456 行落此文件）、体视预览、
  属性切片族是近期增长极——拆分要给这些族留继续生长的独立 TU。
- **消费方扇入**：seismicsectiondock/seismic3d/mappingworkbench/
  datapreview 等全站地震消费面 include 此头——**公共 API 不变**
  是硬约束（聚合头转发，消费方零改动）。

## 环境接线（Windows 本机实测口径，源 goal/sf-kriging 账本 R0）

```powershell
cd C:\Users\wangj.KEVIN\projects\paleo_workstation
git fetch origin
git worktree add .worktrees\seismic-svc-split -b goal/seismic-svc-split-20261006 origin/master
cd .worktrees\seismic-svc-split
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo `
  -DQGIS_PREFIX=C:/Users/wangj.KEVIN/paleo-qgis-prefix `
  -DCMAKE_PREFIX_PATH="C:/deps/Qt/6.8.0/msvc2022_64;C:/Users/wangj.KEVIN/paleo-qgis-deps/Library;C:/deps/qscintilla-install" `
  -DQSCINTILLA_PREFIX=C:/deps/qscintilla-install
cmake --build build -j8
```

坑位同方向 48。拆分是纯移动+接线，构建轮次多但每轮增量小。

## 预算（开发量）

- **Agent tokens**：上限 3 亿，预计 1.2–1.8 亿（大体量代码搬迁
  +守卫语义逐条对拍 + 全量回归）。
- **执行花费**：15–20 轮构建+测试（地震域测试面大：tst_seismic_*
  族 + sectionui 23 例 ×3 遍先例），预计墙钟 3–4 个工作日会话。

## 目标形态（建议按序）

1. **R0 结构勘察**：按 start* 族+内部状态依赖画依赖图（哪些族
   共享 m_cache/m_sessions/m_gate），定 TU 切分线（预计：core
   基座+索引转码族+切片剖面族+属性体化族+解释会话族+体视预览族
   六 TU）；切分线记 ledger 定案后再动刀。
2. **基础设施先行**：共享私有状态提取（pimpl 或 SharedState
   结构体，同文件族内可见）；聚合头 `seismictaskservice.h` 保持
  公共 API 字面不变（19 个 start* 签名逐条 diff 对拍）。
3. **逐族搬迁**：每族一提交——先搬无状态工具族（探测/LOD），
   再搬有状态族（切片 LRU/属性缓存）；每步全量地震测试绿再
   下一族。
4. **并发语义对拍**：拆分前后并发行为等价性——世代号守卫、
   supersede/cancel、闸释放路径逐条断言（先例：seismic-runtime-
   closure 的 23 用例矩阵）跑三遍。
5. **收口**：主文件 ≤800 行（聚合+基座）；CMake 源清单更新；
   内部 include 纪律（族间经聚合头，不互 include 实现细节）。

## 通用纪律（方向内全程有效）

- **分层**：全部留在 `src/services/`（数据层）；新文件头三行
  `// 层：数据`；`check_layering.py --strict` 绿。
- **行为保留红线**：公共 API 字面不变（消费方零改动零重编——
  除链接面外）；任务/取消/世代/闸语义逐条不变——任何「顺手
  改进」都是违规，改进想法记 TODOS 递延。
- **测试口径**：地震域测试每批全绿；关键并发矩阵（sectionui
  23 例）收口时跑三遍（先例口径）。
- **资源**：`-j8`；ctest 串行；全量构建后再 ctest（陈旧链接
  假红教训——seismic-runtime-closure 轮1 踩坑②）。
- **无人值守**：切分线自行定案记 ledger；与方向 48 的
  RUNTIME-04 修复若并行在飞，语义迁移按「谁先合谁为准，后者
  rebase」处理，不丢弃对方改动。
- **性能断言**：拆分后地震读面性能门不回退（比率门对照 R0
  基线跑 tst_seismic_perf/budgets）。
- **ledger**：`.goal-loop-ledger-seismic-svc-split.md`。
- **多轮 review（硬要求）**：每批 → 测试全绿 → diff 自审（API
  字面等价/守卫语义/共享状态封装/TU 边界/构建面 五维）→ 修复
  → 再 review，至少两轮零 High/Medium；Low 记 PR body。
- **提交**：原子提交，中文 conventional 前缀。

## Oracle 验收（逐条需验证证据）

1. API 等价：`git diff` 对拍头文件公共区零变更（19 个 start*
   签名+信号+公共类型逐条一致）；消费方文件零改动（rg 证据）。
2. 体量：seismictaskservice.cpp ≤800 行；新族 TU 各 ≤1,200 行；
   无新文件 >1,500 行（wc 证据入 ledger）。
3. 并发矩阵：sectionui/地震邻域测试三遍全绿；supersede/cancel/
   世代号/闸释放回归用例原样通过（用例文件零改动即通过）。
4. 性能门：tst_seismic_perf/budgets 对照 R0 基线比率无 >10%
   劣化（比率门，非绝对毫秒）。
5. 全量 ctest 两遍无新增失败（对照 R0 基线红清单）。
6. check_layering 三档绿；构建无新警告；git diff --check 干净。
