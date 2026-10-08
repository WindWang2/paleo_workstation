# 方向 75 ledger：CI asan 断链修复 + 运行侧验证

- 分支：`goal/ci-asan-fix-20261009`（worktree `.worktrees/ci-asan-fix`）
- 基线：origin/master `f31ee461`（任务书口径 `e3c8d31d`，fetch 时 master
  已前进两个提交：#296 LibreOffice vendored / #297 issue 集中批；两提交
  均未动 ci.yml 的 asan job，断链仍在，任务前提成立）
- 范围：`.github/workflows/ci.yml`、`tools/`、文档；零 src/ 改动。

## R0 运行侧核对（2026-10-08，gh api 实测，最近 10-13 次 runs）

### 五 job 结局表（最近 10 个已完成/进行中 runs，2026-10-07 15:54 起）

| run | 时间(UTC) | 事件/分支 | lint | linux | linux-perf | linux-asan | windows |
|---|---|---|---|---|---|---|---|
| 37725191312 | 10-08 03:57 | PR codex/well-vector-attributes | 进行中 | 进行中 | 进行中 | **红** | **红** |
| 37720953318 | 10-08 03:04 | PR docs/goal-loop-prompts-batch10 | 绿 | 红 | 红 | **红** | 红 |
| 37720706843 | 10-08 03:01 | push master f31ee461 | 绿 | 红 | 红 | **红** | 红 |
| 37720680924 | 10-08 03:00 | PR goal/issue-batch | 绿 | 红 | 红 | **红** | 红 |
| 37720237028 | 10-08 02:55 | push master 6f1f6a26 | 绿 | 红 | 红 | **红** | 进行中 |
| 37719207995 | 10-08 02:42 | PR（被新 push 取消） | 取消 | 取消 | 取消 | **红** | 红 |
| 37718290346 | 10-08 02:31 | PR codex/vendor-libreoffice | 绿 | 红 | 红 | **红** | 红 |
| 37712605073 | 10-08 01:22 | push master e3c8d31d | 绿 | 红 | 红 | **红** | 红 |
| 37710168348 | 10-08 00:54 | push master 74396674 | 绿 | 红 | 红 | **红** | 红 |
| 37666459256 | 10-07 18:23 | PR goal/singlefactor | 绿 | 取消 | 取消 | **红** | 红 |

统计（10 runs）：linux-asan 10/10 红（100% 死在安装步）；linux 7 红 2 取消
2 进行中；linux-perf 同 linux；windows 9 红 1 进行中；lint 6 绿 3 取消
1 进行中。

### 故障归因（job 步骤日志实测）

1. **linux-asan**：全部死在 `Install QGIS 4.2 development packages` 步——
   `run: tools/ci_apt_qgis.sh ...`，该脚本方向 71（dc478176）已删。
   被 job 级 `continue-on-error: true` 吞掉，观察档自方向 70 合入起
   **从未跑进编译步**。← 本方向主修对象。
2. **linux / linux-perf**：自 `27ac69d8`（方向 71 合入，10-07 13:15，
   最后绿 run 37626880143=`61c8f287`）起持续红在 `Vendor dependencies`
   （bootstrap 尾部全量构建链接期）：vendored `libarmadillo.so.15` /
   `libarpack.so.2` 缺 LAPACK/BLAS 符号（cgeqrf_/dtrmm_/sgemv_...），
   `libpulse.so.0` 缺自身 pa_* 符号（libpulsecommon 未进闭包）。
   → **deb 闭包缺传递依赖（libblas/liblapack/libpulsecommon 族）**，
   vendor/deb-closure 域问题，超出本方向范围，记档移交。
3. **windows**：近期（37712605073 起）红在 `Vendor dependencies
   (OSGeo4W)`——OSGeo4W 上游已发新版，manifest 钉版过期（arrow-cpp
   25.0.0→25.0.1、base、brotli、curl、expat、gdal 等全家族漂移），
   bootstrap 钉版闸正确拦截；更早 runs 红在 Test 步（另一独立遗留面）。
   → vendor/manifest 钉版刷新域问题，同样移交。

**结论**：asan 断链修复后，运行侧期望是「过安装步、进入 bootstrap/
编译步」；linux 三件套能否全绿取决于上述共享 bootstrap 故障（不在本
方向修复）。如实记录，不静默。

## 决策记录（无人值守自决）

- D1 asan job 修法：逐行对拍 linux 主 job（checkout 钉 SHA → Qt6 发行版
  包 apt → ccache 缓存（保留 asan 专用 key，restore 带 os-ccache- 兜底）
  → vendor 缓存补齐 debs/lo 路径与 debclosure key → `./paleo-dev
  bootstrap`）。原死脚本步整步删除。
- D2 Configure 步补 `QGIS_PREFIX_PATH=vendor/prefix/usr` env：asan job
  裸跑 cmake 不过 paleo-dev 顶部的自动注入（linux 主 job 用
  `./paleo-dev build` 自带注入；linux-perf 的 Configure 同样显式给）。
- D3 Test 步从裸 `ctest -j1` 改 `./paleo-dev test -j 1
  --output-on-failure`：vendored 路测试期需要 deb 库路径/QGIS_PREFIX_PATH/
  GDAL_DRIVER_PATH 注入（paleo-dev test 承担，linux 主 job 同源）；
  `ASAN_OPTIONS=detect_leaks=0` 语义保留（env 透传）。
- D4 增量基线步放 linux 主 job（非 asan：门禁 job runs 最密、趋势面最
  全），step 级 `continue-on-error: true`——绝对时长只记录不设门是既
  有纪律，preflight no-op 校验失败（CONFIGURE_DEPENDS 重扫抖动）不许
  拖红合并门禁。jsonl 入独立 artifact `incremental-baseline-linux`。
- D5 护栏 `tools/check_ci_scripts.py`：静态解析 `.github/workflows/*.{yml,
  yaml}` 的 `run:` 块（单行 + `|`/`>` 块标量，注释行不扫——ci.yml 内
  「原 tools/ci_apt_qgis.sh 已删」历史注释不得误伤），提取 `tools/`
  引用做存在性检查（Test-Path 语义，不查执行位——Windows checkout
  丢 x bit）；`--selftest` 合成树双向验证；挂 lint job Source gates。
- D6 零 src/ 改动保持；发现的三处越界问题（deb 闭包缺库 / OSGeo4W 钉版
  过期 / windows Test 步遗留红）只记档（本 ledger + devex.md）不修。

## 轮次

| 轮 | 改动 | 验证 | 结果 | 下一步 |
|---|---|---|---|---|
| 0 | 运行侧核对（gh api 10-13 runs 逐 job 拆结局+失败步日志） | 见上表与归因 | 通过（数据齐） | ci.yml asan 修复 |
| 1 | ci.yml asan job 重写（对拍 linux 主 job）+ Test 步走 paleo-dev test + Source gates 挂 check_ci_scripts.py + linux job 加 ET14 基线步 + 文档 | 护栏 selftest 8/8、本仓 9 引用全存在、mutation 打红/还原复绿、rg ci_apt_qgis 零命中、YAML 可解析、五维 diff 自审 | 通过 | 原子提交 |
| 2 | 五原子提交（25918973 asan / e14beca6 护栏 / c1e7efb9 增量 / 673849f5 文档 / 68e83ada 补钉 lint cache SHA）+ push + PR #300 | 关键检查二遍复验无漂移；分步重放与最终版 diff 为空；ci.yml 零 unpinned action | 通过 | PR 首跑取证 |
| 3 | （等待侧）PR #300 run 37727797358：linux-asan 安装步 **completed/success**（原致命步），已进入 Vendor dependencies（bootstrap 全量构建）；windows 挂在 OSGeo4W 钉版闸（master 既有移交项） | gh api 步骤级状态 | 通过（asan 断链修复实证） | 补 bootstrap 结局观察 |
| 4 | 终轮观察（~50 min）：lint **completed/success**（同 run 实跑新护栏 check_ci_scripts.py 于 Source gates）；linux-asan / linux / linux-perf 仍在 Vendor dependencies（ASAN -j2 无缓存全量构建慢，超出不等跑纪律的等待预算）；主 checkout 复核无本地 incremental-baseline.jsonl | gh api + 本地 ls | 通过（护栏 CI 侧实证；bootstrap 结局按纪律不等待，闭包修复后自然揭晓） | Oracle 对账收口 |

## Oracle 对账（验收逐条）

| # | 验收项 | 证据 | 判定 |
|---|---|---|---|
| 1 | ci.yml 全文零 ci_apt_qgis + 依赖步对拍 | rg 于 .github/ 零命中（两遍）；对拍记录 D1-D3（含 Configure 补 QGIS_PREFIX_PATH、Test 走 paleo-dev test 的偏差及理由） | ✅ |
| 2 | asan 修后跑进编译步 + 五 job 结局表 | run 37727797358 步骤级：安装步 success、Vendor dependencies in_progress（bootstrap=fetch+全量构建+selfcheck 即编译步）；结局表见 R0 节；windows 现状如实（OSGeo4W 钉版闸红） | ✅（全绿被共享故障阻塞，见 #3） |
| 3 | ASAN/UBSAN 全量对照 80/83 基线 | **阻塞**：deb 闭包缺 LAPACK/BLAS/pulsecommon，bootstrap 链接期红（与 linux 主 job 同一处，vendor 域移交）。devex.md 已记「闭包修复后首跑补对照」 | ⛔ 移交（非本方向可解） |
| 4 | 护栏进 Source gates + mutation 打红 | ci.yml lint job 两行接线；selftest 8/8（两遍）；mutation（deb-lock.yml 幽灵脚本）exit 1 → 还原 exit 0；**CI 侧 lint job 同 run 绿** | ✅ |
| 5 | 增量基线首测 jsonl + BUILDING.md 落点 | 步骤+artifact（incremental-baseline-linux，retention 90d）已接线；BUILDING.md 补 CI 落点段；jsonl 首产物被 #3 同一故障阻塞（linux job 未走到该步），主 checkout 亦无本地 jsonl（复核） | ⚠️ 接线完成，产物待闭包修复 |
| 6 | 全量 ctest 对照 R0（若跑 sanity 构建） | 本方向零 src/ 改动、零 C++ 构建（纯 yml/tools/文档），按任务措辞不适用 | ➖ N/A |
