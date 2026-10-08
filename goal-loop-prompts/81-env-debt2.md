# Goal-Loop 方向 81：环境债二期——16 红清单逐条清零 + QSettings 沙箱决策

## 背景（实测事实，勿再勘察；行号为 2026-10-08 master `e3c8d31d` 口径）

方向 72（#272）把本机全量回归从 63%（185 红/293）修到
**96%（345 绿/361）**，账本「终态 16 红逐条归类」表（
`.goal-loop-ledger-win-qt-unify.md` R9/R11 段）如实移交了
剩余面——本方向把这 16 红清到理论下限：

1. **QGIS prefix 资源缺口**（srs.db 全盘无）：tst_runtime、
   boot*——方向 65 账本 `:121-126` 同报（paleo-qgis-prefix/
   resources 缺失）；方向 71 账本 `:219-221` 的「212 项本机
   环境红排查建议单独立项」与此同根。修法候选：bootstrap
   时按 deb 闭包/superbuild 同源补 srs.db + coordinate
   reference 资源到 prefix/resources（方向 72 已有 qtpdf
   手工取档先例）。
2. **Windows pid 存活探测疑真缺陷**：stale 锁不可恢复——
   tst_metastore、boot*（双因）。这是疑似真 bug 非 env：
   `src/metadata/` 的 pid 探测逻辑（进程死后锁不可恢复）
   需真修 + 测试钉死。
3. **QSettings 注册表 + UI 行为断言**（Windows NativeFormat
   已知串行面）：tst_ui、tst_panels、tst_uxtheme、
   tst_dockmanager、tst_wellsection_ui、tst_stratigraphicweb、
   tst_wellfaciesconfig——**决策项**：qt.conf 强制 IniFormat
   vs NativeFormat→IniFormat 跨方向切换 vs 保持串行豁免
   （devex 递延 3）。若转 IniFormat：QSettings 面全局换档 +
   迁移读旧注册表口径 + 测试沙箱化（env 隔离恢复——原
   add_paleo_test 的 XDG 沙箱设计即为此）。
4. **机器性能预算红**：tst_cache_las、tst_singlefactor_perf、
   tst_startup_trace——非缺陷；按 perfbudget_relax.h 先例
   评估机器档降级口径或保持「本机豁免清单」（清单化 +
   自动检测理由）。
5. **QProcess×conda python 启动未遂**（方向 68 新增）：
   tst_pythonrepl、tst_scriptrunner——`PALEO_PYTHON=
   $HOME/paleo-qgis-deps/python.exe`（conda 布局 python 在
   根不在 Library/bin）在测试进程里是否传递到位；修好即绿
  （方向 68 功能是好的，测试环境没喂对）。
6. **ctest 沙箱夹具分叉**（直跑绿）：tst_aiassist_perf——
   ctest 与直跑的行为分叉（方向 72 账本 Low ③ 未深挖）；
   定案：ctest env 补齐 or 测试改为 env 自洽。

## 环境接线（Windows 本机实测口径；方向 72 后统一链）

```powershell
cd C:\Users\wangj.KEVIN\projects\paleo_workstation
git fetch origin
git worktree add .worktrees\env-debt2 -b goal/env-debt2-20261009 origin/master
cd .worktrees\env-debt2
./paleo-dev.ps1 checkenv   # 链一致性自检（方向 72 交付）
./paleo-dev.ps1 test
```

等价手工接线见 BUILDING.md:140-152（含 TEMP/TMP/SEISMIC_INDEX_
CACHE_DIR 沙箱监狱、PALEO_PYTHON conda 根布局）。

## 目标形态（建议按序）

1. **R0 十六红复跑**：终版 env 下全量 ctest 重跑取当前红
   集合（可能较账本 16 有漂移——十方向合并后）；逐条归到
   上述六类。
2. **srs.db 资源闭包**：修 paleo-dev bootstrap（或独立
   ensure-resources 步）把 QGIS 资源文件补进 prefix——与
   方向 71 的 deb 闭包源对齐（同源不手抄）；tst_runtime/boot*
   转绿。
3. **pid 探测真修**：stale 锁恢复语义（进程不存在→锁可
   回收）；测试钉死（持有者死亡→新进程接管）；MetaStore
   双因中的 env 部分一并处理。
4. **QSettings 决策与落地**：按判据（测试沙箱化收益 vs
   用户注册表数据迁移风险）三选一；若转 IniFormat——
   迁移读旧 + 沙箱恢复 + 相关 7 测试转绿；若保持——豁免
   清单化 + 理由文档化。
5. **python 启动修复**：PALEO_PYTHON 在 ctest env 的传递
  （add_paleo_test 沙箱 env 或测试自寻）；tst_pythonrepl/
   tst_scriptrunner 转绿。
6. **性能预算与分叉**：三 perf 红按 relax 机制评估；tst_
   aiassist_perf 分叉定案（env 自洽优先）。
7. **防回归**：红集合清单化（tools/ 或 ctest 标签）；每类
   修复配 mutation 验证。

## 通用纪律（方向内全程有效）

- **边界**：环境修复优先；暴露的真 bug（pid 探测）属显式
  授权修复面；其余代码问题记档移交。
- **诚实面**：QSettings 决策的弃案理由记 ledger；perf 红
  不硬修（比率门语义）。
- **资源**：`./paleo-dev.ps1` 系；ctest 串行；全量 ctest
  多轮（两遍确认）。
- **无人值守**：QSettings 三选一自行定案记 ledger（判据
  齐全）。
- **ledger**：`.goal-loop-ledger-env-debt2.md`。
- **多轮 review（硬要求）**：每批 → 相关测试转绿 → diff
  自审（env 正确性/真修质量/决策判据/防回归/文档 五维）→
  修复 → 再 review，至少两轮零 High/Medium；Low 记 PR body。
- **提交**：原子提交，中文 conventional 前缀。

## Oracle 验收（逐条需验证证据）

1. 红集合收敛：全量 ctest 两遍——16 红中**至少 10 项转绿**
  （srs.db 族 + pid + python 族 + aiassist_perf 预期全绿）；
   余项逐条有终态（豁免清单/决策记录）。
2. srs.db：prefix/resources 齐备（文件清单）；来源与方向 71
   闭包同源（非手抄）；boot*/tst_runtime 绿。
3. pid 真修：stale 锁恢复测试（持有者死亡→接管）绿；
   mutation（改回旧逻辑）打红。
4. QSettings：决策记录（判据 + 弃案理由）；落地后相关 7
   测试绿或豁免清单化。
5. python：PALEO_PYTHON 在 ctest 下可达（测试断言）；两个
   python 测试绿。
6. 防回归：红集合清单化脚本/标签存在；新增突变能打红。
7. 全量 ctest 两遍稳定（无新 flake 引入）；layering 三档绿。
