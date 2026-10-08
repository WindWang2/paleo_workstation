# Goal-Loop 方向 75：CI asan 断链修复 + 运行侧验证——观察档真正跑起来

## 背景（实测事实，勿再勘察；行号为 2026-10-08 master `e3c8d31d` 口径）

三轮盘点发现的**唯一真实接线缺陷**（被 continue-on-error 掩盖）：

- `.github/workflows/ci.yml:258` `linux-asan` job 仍执行
  `run: tools/ci_apt_qgis.sh libproj-dev ...`——该脚本已被
  方向 71（#271，提交 739f09ec）**删除**（`Test-Path` = False；
  `ci.yml:125` linux job 注释亲口说「tools/ci_apt_qgis.sh 已删」）。
- 后果：asan job 在依赖安装步即报错，被 `continue-on-error: true`
  吞掉——**asan 观察档自合入起实际从未跑进编译步**，方向 70
  的 sanitizer CI 化名存实亡。
- 方向 71 的 linux 主 job 已示范正确路线（`ci.yml:127-146`：
  Qt6 发行版包 + `./paleo-dev bootstrap` 拉 deb 闭包、
  `QGIS_PREFIX_PATH=vendor/prefix/usr` 见 `:227-230`），
  apt 只装 Qt6 发行版包（政策例外）。
- 附带：`tools/measure_incremental.sh` 已交付但**无已提交的
  实测数据**（主 checkout 无 build/incremental-baseline.jsonl）；
  CI 五 job（lint/linux/linux-perf/linux-asan/windows）在
  GitHub Actions 上的运行侧结局未做过系统核对。

## 环境接线（Windows 本机实测口径；yml 语法本地可验）

```powershell
cd C:\Users\wangj.KEVIN\projects\paleo_workstation
git fetch origin
git worktree add .worktrees\ci-asan-fix -b goal/ci-asan-fix-20261009 origin/master
cd .worktrees\ci-asan-fix
# 本方向主要改 .github/workflows/ci.yml 与 tools/ 脚本——无 C++ 构建；
# 若需 sanity 构建：./paleo-dev.ps1 build（自动 localdeps 路线）
```

坑位（新统一口径，方向 72 后）：编译/运行同链 Qt 6.11.2——
`CMAKE_PREFIX_PATH=$HOME/paleo-qgis-deps/Library` +
`QT_ADDITIONAL_PACKAGES_PREFIX_PATH=C:/deps/Qt/6.11.2/msvc2022_64`
+ `QGIS_PREFIX=$HOME/paleo-qgis-prefix`；`./paleo-dev.ps1`
build/test/selfcheck 自动进 localdeps 路线；`checkenv` 自检可用。
**旧接线（C:/deps/Qt/6.8.0 + C:/deps/qscintilla-install）已退役，
勿使用**（BUILDING.md:125-197 统一口径表）。

## 目标形态（建议按序）

1. **asan job 修复**：`ci.yml:258` 步改为对齐 linux 主 job 的
   vendored 路（bootstrap 拉 deb 闭包，删除对已死脚本的引用）；
   依赖步骤顺序与 linux job 逐行对拍。
2. **运行侧核对**：`gh run list`（GH_TOKEN 可用）核对五 job
   最近 10 次 runs 的真实结局——逐 job 记录绿/红/取消比；
   asan job 修后确认能跑进编译步（run 列表 + job 步骤日志）。
3. **asan 首跑实测**：修后（或引用最近一次成功 run）确认
   ASAN/UBSAN 全量输出——对照 devex.md 记录的 80/83 基线
  （3 个失败应为预算断言，已被 PALEO_SANITIZER_BUILD 降档
   吸收——预期全绿或剩已知面）。
4. **增量基线首测**：在 CI linux job（或 linux-asan）里跑
   `tools/measure_incremental.sh` 一轮，产物 jsonl 入 artifact
   （趋势面；绝对时长只记录不设门——既有纪律）。
5. **护栏**：加轻量检查——yml 内引用的 tools/ 脚本必须存在
  （静态解析 yml 的 `run:` 行扫 `tools/xxx` 引用 + Test-Path；
   进 lint job Source gates 或独立脚本挂 ctest），防再断链。
6. **文档**：BUILDING.md 增量基线段补实测数据落点；devex.md
   记录 asan job 修复前后的首次真实运行。

## 通用纪律（方向内全程有效）

- **分层**：只动 `.github/workflows/ci.yml`、`tools/` 脚本、
  文档；零 src/ 改动（发现代码问题记档移交不扩大范围）。
- **诚实面**：CI 运行侧核对结果如实记录（包括 windows leg
  现状）；`continue-on-error` 的观察档语义保留（不静默转阻断）。
- **资源**：CI 不等跑（门禁语义）；本机 `./paleo-dev.ps1` 自检。
- **无人值守**：yml 修法对齐 linux job 的取舍自行定案记 ledger。
- **ledger**：`.goal-loop-ledger-ci-asan-fix.md`。
- **多轮 review（硬要求）**：每批 → 本地可验证项绿（yml 结构/
  脚本存在性/mutation）→ diff 自审（yml 正确性/护栏有效/文档
  同步/无越权/运行证据 五维）→ 修复 → 再 review，至少两轮
  零 High/Medium；Low 记 PR body。
- **提交**：原子提交，中文 conventional 前缀。

## Oracle 验收（逐条需验证证据）

1. `ci.yml` 全文零 `ci_apt_qgis` 引用（rg 证据）；asan job 的
   依赖步与 linux 主 job 结构对拍记录。
2. 运行侧：asan job 修复后首次 run 的 job 步骤日志证据（跑进
   编译步；或如实记录触发条件与等待状态）；五 job 最近 runs
   结局表入 ledger。
3. asan 全量：ASAN 零内存错误 + UBSAN 已知面清单（对照
   devex.md 基线，无新增）。
4. 护栏：yml 脚本引用存在性检查进 Source gates；mutation
  （yml 里故意引用不存在脚本）能打红后撤销。
5. 增量基线：measure_incremental 首测 jsonl 产物（artifact 或
   仓内记录）；BUILDING.md 落点更新。
6. 全量 ctest 对照 R0 红集合 diff 为空（若本机跑了 sanity 构建）。
