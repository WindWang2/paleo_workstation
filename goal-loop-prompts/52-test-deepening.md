# Goal-Loop 方向 52：测试深化——零测试头清零 + dataChanged 增量通道

## 背景（实测事实，勿再勘察；2026-10-05 master `adf2be7` 口径）

- **零测试头**（AUDIT TEST-06 基线 228 头 vs 126 套件交叉，33 个
  零测试头待复核）：最高风险 `src/metadata/atomicfile.h`——**原子
  写原语零测试**（工程文件损坏防线，一旦有 bug 是数据丢失级）；
  其余高席位 `src/ai/remotepredictionservice.h`（方向 51 改造面，
  协调注记：若 51 在飞则该头测试归 51，本方向跳过；51 号已声明
  承接该头测试套件）、
  `src/workflow/registration.h`、`src/catalog/catalogindex.h`。
- **TEST-01/02/06（待证）**：测试稳定性/隔离性问题——R0 复核
  定性（同方向 48 口径）。
- **dataChanged 全仓 0 处**：编辑型模型（属性表、分层编辑器、
  资产表）无行级增量更新通道；唯一 QAbstractTableModel 走
  fetchMore 分批（`src/ui/pages/dataopsviews.h:57` FlatAssetModel
  定义 kBatchSize=256，`:69` 首屏取一批）——读侧分页设计好，**写侧增量是空白**：
  编辑提交后全模型 reset（或无刷新路径）。
- 测试基建现状：286 个 tst_*.cpp；add_paleo_test 双契约（单参
  伞式/LIBS 最小链接）；沙箱 XDG 隔离；Windows ctest 串行。

本方向双线：**A 线**补高危零测试头；**B 线**给编辑型模型立
dataChanged 增量通道（首个消费者：属性表面板 + 资产表）。

## 环境接线（Windows 本机实测口径，源 goal/sf-kriging 账本 R0）

```powershell
cd C:\Users\wangj.KEVIN\projects\paleo_workstation
git fetch origin
git worktree add .worktrees\test-deepening -b goal/test-deepening-20261006 origin/master
cd .worktrees\test-deepening
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo `
  -DQGIS_PREFIX=C:/Users/wangj.KEVIN/paleo-qgis-prefix `
  -DCMAKE_PREFIX_PATH="C:/deps/Qt/6.8.0/msvc2022_64;C:/Users/wangj.KEVIN/paleo-qgis-deps/Library;C:/deps/qscintilla-install" `
  -DQSCINTILLA_PREFIX=C:/deps/qscintilla-install
cmake --build build -j8
```

坑位同方向 48（沙箱 QTemporaryDir 拒绝面——atomicfile 测试
恰恰重依赖临时目录，R0 基线对照先行，环境红记档不硬扛）。

## 预算（开发量）

- **Agent tokens**：上限 3 亿，预计 0.9–1.3 亿（测试编写为主 +
  B 线模型改造）。
- **执行花费**：12–16 轮构建+测试，预计墙钟 2 个工作日会话。

## 目标形态（建议按序）

1. **R0 复核**：33 零测试头清单重验（哪些已有测试、哪些真零）；
   TEST-01/02/06 定性；定本方向 A 线目标清单（预计 ≥15 个
   高中危头）。
2. **A 线·atomicfile 优先**：原子写原语全语义测试——正常写/
   中途崩溃模拟（kill 点注入或错误注入）/目标已存在/磁盘满
   模拟（配额难模拟则错误注入接口）/权限拒绝/换行符与二进制
   round-trip；损坏防线断言（.running 脏标记先例：crashreport）。
3. **A 线·其余头**：registration/catalogindex 等按风险序逐个补
   （每头 ≥5 用例，覆盖正常+边界+失败三态）；跳过项记协调
   注记（remotepredictionservice 若归 49）。
4. **B 线·增量通道**：dataopsviews FlatAssetModel + 属性表面板
   模型增 `updateRows`/`updateCells` 增量接口——dataChanged 按
   范围发（先例语义：QAbstractItemModel 契约）；调用点（编辑
   提交/批量改型）改走增量；reset 只留给全量换数据源场景。
5. **B 线·断言**：增量更新后视图状态保留（滚动位置/选中集）
   ——QSignalSpy 断言 dataChanged 范围正确、无多余 reset 发生
  （modelReset spy 计数零）。
6. **门禁**：TEST-01/02/06 若定性为真实问题，修复并加防回归
   用例；已修则记档。

## 通用纪律（方向内全程有效）

- **分层**：测试文件无层标记要求（tests/ 目录）；B 线模型改造
  在 ui 层内；`check_layering.py --strict` 绿。
- **测试质量红线**：禁恒真断言（TEST-04 教训——新用例必须能被
   mutation 打红，A 线每套至少一处示范）；断言写不变量不写
  实现细节。
- **资源**：`-j8`；ctest 串行；新测试遵守沙箱纪律（独立 XDG）。
- **无人值守**：零测试头优先级排序自行定案记 ledger。
- **ledger**：`.goal-loop-ledger-test-deepening.md`。
- **多轮 review（硬要求）**：每批 → 新测试全绿 → diff 自审
  （断言真实性/覆盖三态/增量语义正确/无测试物泄漏进生产头/
  i18n 五维）→ 修复 → 再 review，至少两轮零 High/Medium；
  Low 记 PR body。
- **提交**：原子提交，中文 conventional 前缀。

## Oracle 验收（逐条需验证证据）

1. R0 清单：33 头复核终态（真零/已有/协调移交）逐条记档；
   A 线目标 ≥15 头全部有测试套件（ctest -N 清单证据）。
2. atomicfile：崩溃/错误注入路径的防数据损坏断言全绿——
   「写失败后原文件完整」核心不变量有直接用例。
3. 增量通道：编辑提交后 QSignalSpy 捕获 dataChanged 且
   modelReset 计数为零；滚动/选中状态保留断言通过。
4. mutation 示范：A 线每套至少一处「故意打坏被断言抓住」
   的记录（方法+输出）入 ledger。
5. TEST-01/02/06：定性终态+（若真实）修复证据入 AUDIT_ISSUES.md。
6. 全量 ctest 两遍无新增失败（对照 R0 基线红清单）；新测试
   稳定性（连跑三遍零 flake）。
