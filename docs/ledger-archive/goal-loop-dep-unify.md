# Goal-Loop Ledger — 方向 71：依赖口径统一（dep-unify）

- 分支：`goal/dep-unify-20261007`，基线 `origin/master` = `2ab3310`
  （方向书行号口径为 `f130fb2`；基线更新后相关文件行号有漂移，
  事实逐一重新核对仍成立——见下。worktree：`.worktrees/dep-unify`）。
- 执行机：Windows（Git Bash + MSVC 2022 + CMake 3.29.3/Ninja）。
  Linux 侧操作（deb 闭包重锁、CI 实跑）不可本机执行——本方向选定
  方案后 **无需** 闭包重锁；CI yaml 编写后不等待实跑（用户门禁语义）。

## R0 决策：统一到 4.2.3（方案 A：superbuild 升级），弃 B（deb 闭包降级）

三处口径现状（基线 `2ab3310` 实测）：

| 路线 | 文件 | 口径 |
|---|---|---|
| superbuild（源码 vendored） | `vendor/superbuild/CMakeLists.txt:49` URL + `:125` URL_HASH | 4.2.2 精确（SHA256 `45c3a85b…becf0`），补丁名锁 4.2.2 |
| deb 闭包（Linux CI/加速档） | `vendor/deb-closure.lock`（qgis.org/debian 源） | 4.2.3+44resolute 精确（逐包 size+SHA256） |
| OSGeo4W（Windows） | `vendor/manifest.json` `qgis_family` | "4.2.x" 浮动（家族粒度） |

判据对比：

| 判据 | A：superbuild → 4.2.3 | B：deb 闭包 → 4.2.2 |
|---|---|---|
| 补丁在目标版本适用性 | **已实证干净**：4.2.3 tarball 抽取 4 个触及文件，`patch -p1 --dry-run` exit 0 无 fuzz（LF 索引版补丁）；实弹应用后 `QGIS_PALEO_LABELS_WITH_LAYER` 宏落位 | 4.2.2 原生匹配（无需动补丁） |
| 闭包重锁成本 | **零**（deb 闭包保持 4.2.3 不动，CI 锁/缓存键不失效） | 需 Linux 干净环境跑 `--update-lock`（本机不可执行）；且 qgis.org 池只保留现役版本、第三方源无 snapshot 回退（`fetch-deps.sh` 明示）——4.2.2 旧 deb 大概率已下架，**重锁可能根本不可行** |
| LTR/支持窗口 | 4.2 系最新补丁版，窗口最长；系列内补丁版互换为 drop-in | 回退丢 4.2.3 修复，窗口缩短 |
| URL/SHA 可获得性 | **已实证**：`qgis-4.2.3.tar.bz2` 下载成功，211,045,676 字节，SHA256 `b044889d895c07d9355512cba76973822830153f6f15feb0d5e00077d3d76e59` | 同上——不可行风险 |
| 本机可完成度 | 全部动作（URL/SHA 回填、补丁重命名、dry-run 验证）Windows 可完成 | 核心动作（重锁）需 Linux，本方向内无法闭环 |

**弃案 B 理由**：qgis.org apt 池不保留被取代版本且无快照服务，
4.2.2 deb 闭包重锁存在不可行风险；即使可行也需本机不具备的 Linux
干净环境，且方向为降级（丢修复）。A 的唯一风险点（补丁适用性）
已用上游 tarball 实证排除。

**OSGeo4W 侧**：installer（setup.exe `-P` 包名清单）只支持包名粒度，
不支持逐包精确版本 pin——粒度边界如实记录（manifest 注释 +
`paleo-dev.ps1` 家族闸）。收紧动作为：把 `paleo-dev.ps1` 里硬编码的
`4\.2\.` 家族正则改为消费 `manifest.json` 的 `qgis_family` 单一来源，
闸门语义 = 拒绝家族不符（原来如此，去掉双份口径）。

## 执行记录

### 环境边界与本机接线的例外处理（如实）

- 首轮基线构建链接失败（非基线红）：worktree 无 gitignored
  `vendor/onnxruntime`（AGENTS/BUILDING 已知边界），master `2ab3310`
  在 **无 ORT 配置**下 `paleo_ai` 为空壳而 `aiassistworkflow.cpp`
  无条件引用 `runTileInference`（`src/ai/tileinference.cpp` 仅在
  ORT 分支编入）→ LNK2019。处置：按 `vendor/manifest.json`
  `onnxruntime_win` 钉位（sha256 `c6ba983b…a949`、82645522 字节
  校验通过）下载解包进 worktree `vendor/onnxruntime`，重配重建。
  这是环境接线不是 src 改动；该链接断点对「裸 worktree + 无 ORT」
  可复现，值得单独 issue 跟踪（本方向不修 src/）。
- 因此基线 ctest 在「补齐 ORT + 含本方向新增的两个护栏测试项」
  的配置上取得（新增项只增不改，红集合比较口径 = 既有测试项）。

### 1. 版本统一落地（方案 A）

- `vendor/superbuild/CMakeLists.txt`：`QGIS_URL` →
  `qgis-4.2.3.tar.bz2`，`URL_HASH` → 实测 SHA256
  `b044889d895c07d9355512cba76973822830153f6f15feb0d5e00077d3d76e59`
  （下载实测：211,045,676 字节）；注释同步（含 Arch 同版说明改为
  deb 闭包同版）。
- 补丁 `git mv` 重命名 `qgis-4.2.2-…` → `qgis-4.2.3-…`，内容零
  改动。**适用性证据**：对 4.2.3 官方 tarball 抽取 4 个触及文件，
  LF（索引版）补丁 `patch -p1 --dry-run` exit 0 无 fuzz；实弹应用
  exit 0，`QGIS_PALEO_LABELS_WITH_LAYER` 宏落位、
  `layerLabelingEngine` 接线 5 处。
- 裁剪面断言在 4.2.3 重新实证：tarball 顶层 CMakeLists
  `WITH_MESH` 零命中；`WITH_DESKTOP`/`WITH_GUI`/`WITH_ANALYSIS`/
  `WITH_3D` 为 cache 变量存在——原裁剪决定全部继续成立。
- **未执行（环境边界，如实）**：Linux 侧 superbuild 实跑重建
  （需要 Linux + ≥2h/60GB）；`vendor/cache/tarballs/` 缓存 tarball
  更新（gitignored，随下次 superbuild 实跑落地）。下次任何 Linux
  superbuild 会按新 URL/SHA 下载并校验。

### 2. manifest 收紧（OSGeo4W）

- `vendor/manifest.json` notes 记录粒度边界：installer 只吃包名
  清单（`-P`），无逐包版本 pin 能力 → `qgis_family` 保持家族粒度
  （4.2.x）。
- 收紧动作为去双份口径：`paleo-dev.ps1` 家族闸正则原为硬编码
  `4\.2\.`（与 manifest `qgis_family` 重复维护），现改为从
  `qgis_family` 派生（`^(\d+\.\d+)(\.x)?$` 解析 + Escape），
  manifest 成为单一来源。pwsh AST 解析通过；功能验证：4.2.x/4.2
  对 4.2.3 安装记录命中、垃圾值 throw、5.0.0 安装被 4.2.x 闸拒。

### 3. #76 CI 迁移（yaml 编写，不等跑）

- `.github/workflows/ci.yml`：lint/linux/linux-perf 三 job 的
  `tools/ci_apt_qgis.sh` 全部移除；`tools/ci_apt_qgis.sh` 删除。
  apt 面收窄为政策例外（Qt6 dev 组、qtkeychain、ccache、
  ninja-build/pkg-config/g++）；QGIS/GDAL/PROJ/GEOS 走
  `./paleo-dev bootstrap` 的 deb 闭包路（`vendor/prefix/usr`）。
- 缓存策略：`vendor/cache/debs` 进 actions/cache（键 =
  `hashFiles(deb-closure.lock, deb-packages.txt)`，锁变即重下）；
  解包产物不缓存（dpkg-deb -x 是本地 CPU 面，省缓存配额）。
  linux/linux-perf 的 vendor 缓存键并入 debclosure 段；ort 缓存
  路径不变。lint job 同样加 deb 闭包缓存（compile_commands 需要
  QGIS 头）。
- env 链路核对：lint/perf 的直连 Configure 步骤显式注入
  `QGIS_PREFIX_PATH`；linux job 的 build/test/selfcheck 经
  `paleo-dev`（顶层导出块）自动注入 prefix + multiarch
  LD_LIBRARY_PATH——与既有 deb 闭包本机用法同一链路。
- yaml 结构核对：PyYAML 解析通过，四 job 步骤清单逐一过目；
  Source gates 增挂 `check_qgis_versions.py`（+selftest）。
- **已知首跑风险（如实，待 CI 实跑验证）**：runner 根盘余量需
  ≥15GB（bootstrap 门禁）；GH ubuntu-26.04 镜像 cmake≥3.28/ninja
  假设在（旧 job 同样直用 cmake/ninja 未显式装，风险低）。

### 4. 文档对齐

- `BUILDING.md`：#76 段改为「已迁移待 CI 验证」+ 版本口径统一
  说明；新增 glibc 三档口径表（2.41/2.43/2.28）；平台矩阵行、
  升级流程（superbuild 升级程序 + 三路一起动）、依赖清单段同步。
- `vendor/superbuild/README.md`：状态行（已钉位 4.2.3）、钉位
  清单、裁剪决定表（4.2.3 重新实证注记）、依赖清单、补丁段
  （重命名 + dry-run/实弹应用证据履历）。
- `docs/PLATFORM_MATRIX.md`：行 2（CI 路线迁 deb 闭包，状态
  ⏳ 待复验）、行 4（CI 已迁此路）、补丁文件名引用。

### 5. 护栏

- 新增 `tools/check_qgis_versions.py`（风格对齐
  check_layering.py：未知参数 exit 2、selftest 单独档）：抽取
  superbuild 精确版本+补丁名版本+URL_HASH、deb 闭包
  qgis-providers 上游版本、manifest 家族，一致性断言四条。
- ctest 接线：`qgis_versions` + `qgis_versions_selftest` 两项
  （WORKING_DIRECTORY=源码根，与 layering 同组属性）；CI lint
  Source gates 同步挂载。
- 验证证据：真树三路口径输出 4.2.3/4.2.3/4.2.x 一致 exit 0；
  selftest 六案例（一致基线 + 五种分裂形态）全对；**真文件副本
  mutation 三红一绿**（deb→4.2.4 / family→4.3.x / superbuild→
  4.2.2 各自 exit 1，还原副本 exit 0）；cp1252 强制编码下不假红。

### 6. 锁可达性冒烟（Oracle #2 证据）

- `vendor/fetch-deps.sh --check-urls`（LF 规范副本上跑，863 包
  ~842 MiB）：**全部可达**，部分 Ubuntu pool 包走
  snapshot.ubuntu.com 回退（libssl-dev/libheif-dev 等被安全更新
  取代下架的——正是设计回退路径），qgis.org 池直连可达。
- 顺手修复：Windows 检出（autocrlf）的锁文件 `\r` 行尾会让
  `--print-only` 的格式断言伪红——`URIS=$(tr -d '\r' < "$LOCK")`
  一行修复（索引/CI 的 LF 语义不变）；真树 CRLF 锁 `--print-only`
  复验通过（863 条目格式全绿），`bash -n` 语法过。

### Review 第 1 轮（diff 五维自审）发现与处置

| 维度 | 发现 | 处置 |
|---|---|---|
| 锁文件格式 | 本方向未改锁（方案 A）；CRLF 伪红见上节 | fetch-deps.sh 健壮化 |
| 决策判据 | ledger R0 表齐全（补丁适用/闭包成本/LTR/URL 可得/本机可完成 五维+弃案理由） | ✓ |
| CI yaml | PyYAML 解析 + 折叠标量展开核对 + 步骤清单过目；残留 `ci_apt_qgis` 引用仅方向书与历史注 | ✓ |
| 文档同步 | superbuild README H1 仍「待启用」、PLATFORM_MATRIX「行 2/3 由 apt 决定」两处陈旧口径 | 已修 |
| 护栏有效 | mutation 三红一绿 + selftest 六案例 + cp1252 兜底 | ✓ |
| 代码 bug | 护栏脚本 `%3a` 撞 `%`-format（TypeError）——真树首跑即暴露 | 已修（拼接替代 % 格式化），复验绿 |

### R0 红集合（本机基线，两轮全量串行 ctest）

- 环境：Windows 本机（MSVC 2022 / RelWithDebInfo / paleo-qgis-prefix
  链接），`QT_QPA_PLATFORM=offscreen`、串行（Windows QSettings 注册表
  语义）、DLL PATH = deps/Library→qgis-prefix→Qt6.8.0。
- 两轮全量（363 项，含本方向新增 2 项）：**42% passed / 212 failed，
  红集合按名 diff 为空**（`diff redset-baseline redset-run2` 零差异）
  ——即方向改动未引入任何新红/未修复任何旧红（口径：对照 diff 为空）。
- 红的构成（抽样归因，全部为环境/机器面，无一与本方向 diff 相关）：
  - ~168 项断言红，主导形态 `QTemporaryDir/QTemporaryFile isValid()
    == false`（tst_welllogset/tst_io_laswriter 等；已证伪 TEMP 环境变量
    假设——显式 Windows 风格 TEMP 直跑仍红；根因未深究，超出方向面
    「不触 src/」纪律，值得单独 issue 在本机复现排查）；
  - 40 项 `0xc0000139` 入口点缺失（本地 prefix 的 DLL 版本竞合，
    tst_wellsection_workflow/tst_faciessupervised 等）；
  - 3 项 `0xc0000409` fail-fast；其余个别为超时/其他。
  - `boot` 红因 `paleo-qgis-prefix` 无 `resources/srs.db`（该 prefix
    仅 bin/include/lib，非完整运行时布局）。
- 本方向新增项两轮均绿：`qgis_versions`、`qgis_versions_selftest`。

### Review 第 2 轮（全 diff 五维复审）发现与处置

| 维度 | 发现 | 处置 |
|---|---|---|
| CI yaml（High） | lint job 用 `./paleo-dev bootstrap` 会触发 bootstrap.sh 尾部的 configure+全量 build+selfcheck（超 lint 预算且语义错位） | bootstrap.sh 增 `fetch-only`（对齐 ps1 语义，paleo-dev 已透传），lint 改 `bootstrap fetch-only`；`set -e` 下 `[ ]&&` 短路假退出陷阱规避（if 形式），argparse 四态验证 |
| 护栏脚本 | `%3a` 撞 `%`-format（首轮发现已修）；cp1252 兜底 | 复验三态绿 |
| vendor 脚本 | fetch-deps CRLF 伪红（check-urls 首跑暴露） | `tr -d '\r'` 修复 + 真树 CRLF 锁复验 |
| 文档一致性 | ci_apt_qgis 引用清零（仅方向书与历史注提及）；BUILDING/README/PLATFORM_MATRIX 三处口径互检一致 | ✓ |
| 环境异常 | 提交时 .git/worktrees 短暂不可见（与并行会话的 git 操作竞态，主仓工作树同期出现他人改动的删除痕迹）；退避重试后恢复，提交内容不受影响 | 已如实记录 |

### 提交清单

1. `00d6706 build(vendor)` superbuild 4.2.3 统一 + 补丁重命名 + README
2. `1266345 test(tools)` 三路一致性护栏 + manifest 粒度边界 + ps1 家族闸
3. `dc47817 ci` Linux 迁 deb 闭包 vendored + 删 ci_apt_qgis.sh + fetch-only
4. `4cb156b docs` BUILDING/PLATFORM_MATRIX 口径对齐 + glibc 三档表
5. ledger（本文件）

## Oracle 对账（逐条）

1. **决策记录** ✓——两案判据表 + 弃案 B 理由入本 ledger（§R0）。
2. **版本统一** ✓——三处口径一致：`tools/check_qgis_versions.py`
   输出 4.2.3/4.2.3/4.2.x（家族 4.2）exit 0（真树证据）；锁文件未动
   （方案 A 无需重锁），`--check-urls` 863/863 全可达（snapshot 回退
   属设计路径）；superbuild 侧 URL/SHA/补丁三件套与既有
   URL_HASH+apply-patch 机制兼容。
3. **manifest** ✓——粒度边界记录（manifest notes + BUILDING）+ 收紧
   为单一来源（ps1 家族闸派生自 qgis_family，四态功能验证）。
4. **#76** ✓——ci.yml 三 job vendored 路（yaml 结构 PyYAML 核对 +
   env 链路逐 job 过目）；BUILDING #76 段如实写「已迁移待 CI 验证」。
5. **护栏 mutation** ✓——真文件副本三处故意分歧各 exit 1，还原
   exit 0；selftest 六案例。
6. **本机可验证项** ✓——构建照常（BUILD_EXIT=0，依赖未变路径）；
   全量 ctest 两轮红集合按名 diff 为空；新增护栏测试项两轮均绿。

## 未尽事项（环境边界，如实）

- GitHub Actions 实跑未做（用户门禁语义「CI 不等跑」）——首跑关注点：
  runner 根盘 ≥15GB（bootstrap 门禁）、ubuntu-26.04 镜像 cmake≥3.28
  假设、deb 闭包首轮下载 ~842MiB（缓存命中后免除）。
- Linux 侧 superbuild 4.2.3 实跑未做（需 Linux + ≤2h/60GB）；下次任何
  superbuild 实跑会按新 URL/SHA 下载校验（钉位语义不依赖本方向验证）。
  `vendor/cache/tarballs/` 旧 4.2.2 缓存（gitignored，各机自持）随
  下次实跑自然替换。
- 本机 212 项环境红的根因排查（QTemporaryDir/DLL 竞合/srs.db）超出
  方向面（不触 src/），建议单独方向跟进。

