# Goal-Loop 方向 71：依赖口径统一——QGIS 三路版本分裂收敛 + #76 CI 政策迁移

## 背景（实测事实，勿再勘察；行号为 2026-10-06 master `f130fb2` 口径）

1. **QGIS 小版本三口径分裂**：
   - superbuild（源码 vendored）：`vendor/superbuild/CMakeLists.txt:49`
     钉 **4.2.2**（URL+SHA256）+ 补丁
     `patches/qgis-4.2.2-labels-with-layer.patch`（补丁名锁
     4.2.2）；
   - deb 闭包（Linux CI 实际链路）：`vendor/deb-closure.lock`
     钉 **4.2.3+44resolute**（逐包 SHA256）；
   - OSGeo4W（Windows）：`vendor/manifest.json`
     `qgis_family: "4.2.x"`（浮动口径）。
   同一产品三条路两个精确小版本 + 一个浮动口径；4.2.2 补丁
   在 4.2.3 上是否干净适用无验证证据。
2. **#76（BUILDING.md:38-41 自认）**：政策「CI 与发布构建
   禁止依赖系统包提供 QGIS/GDAL/PROJ/GEOS」，但 ci.yml
   linux job `:86-87` 仍 `tools/ci_apt_qgis.sh ...`（qgis.org
   apt 系统 QGIS 4.2 dev 包，#138 钉版本）——CI 实际处于
   第 3 档与自定政策矛盾。
3. **onnxruntime 无过期证据**（1.30.0 为 1.30 线最新——勿动）。
4. glibc floor 三档分层显式管理（bootstrap.sh:20-22 二进制
   2.41 / deb 闭包 2.43 / manifest abi_floor 2.28）——非混乱，
   仅需文档对齐口径。

**决策空间（R0 定案，两选一）**：A. superbuild 升 4.2.3
（补丁重验证/rebase）统一到 deb 闭包口径；B. deb 闭包降
4.2.2（重锁闭包）统一到 superbuild 口径。判据：补丁在目标
版本的应用干净度 + 闭包重锁成本 + LTR 支持窗口。OSGeo4W
侧收紧为与所选版本一致的精确口径（manifest qgis_family
语义核对——installer 的包族粒度若不支持精确 pin 则如实
记录粒度边界）。

## 环境接线（Windows 本机实测口径）

```powershell
cd C:\Users\wangj.KEVIN\projects\paleo_workstation
git fetch origin
git worktree add .worktrees\dep-unify -b goal/dep-unify-20261007 origin/master
cd .worktrees\dep-unify
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo `
  -DQGIS_PREFIX=C:/Users/wangj.KEVIN/paleo-qgis-prefix `
  -DCMAKE_PREFIX_PATH="C:/deps/Qt/6.8.0/msvc2022_64;C:/Users/wangj.KEVIN/paleo-qgis-deps/Library;C:/deps/qscintilla-install" `
  -DQSCINTILLA_PREFIX=C:/deps/qscintilla-install
cmake --build build -j8
```

坑位同方向 58。**注意**：本机是 Windows——deb 闭包与
superbuild Linux 侧的重锁/重验证需要 Linux 环境；本方向在
本机做的是：锁文件/清单/文档的统一编辑 + 决策记录 +
OSGeo4W 侧（Windows 可实测）的对齐验证 + CI yaml 迁移编写；
Linux 侧重锁按 `vendor/fetch-deps.sh --update-lock --print-only`
的产出路径走（脚本明示需在目标发行版干净环境跑——若本机
不可行，产出变更清单与操作手册，**如实记录未执行**，由
CI/有 Linux 环境的会话执行——这不算失败收尾，是环境边界
的诚实呈现）。

## 目标形态（建议按序）

1. **R0 决策**：两案对比（补丁适用性/闭包成本/LTR 窗口）
   → 定案记 ledger（含弃案理由）。
2. **版本统一落地**：按所选方案改 superbuild 或 deb 闭包
  （URL/SHA256/补丁 rebase 或闭包重锁）；锁文件格式与
   既有校验机制一致（大小+SHA256 验证，fetch-deps.sh 口径）。
3. **manifest 收紧**：OSGeo4W qgis_family 口径核对——支持
   精确 pin 则收紧；不支持则记录粒度边界 + bootstrap 拒绝
   主版本不符的既有闸对齐。
4. **#76 迁移编写**：ci.yml linux job 从 apt 系统 QGIS 切
   vendored 路线——按 BUILDING.md 优先级（superbuild 首选/
   deb 闭包加速档）；缓存策略（superbuild prefix 或 deb
   闭包解包产物 artifact 缓存）；**CI 不等跑**（用户门禁
   语义——yaml 写好 + 本地可验证的语法/结构核对）。
5. **文档对齐**：BUILDING.md 依赖来源策略段更新（#76 状态、
   版本决策、glibc 三档口径表）；vendor/superbuild/README
   与补丁名同步。
6. **护栏**：加一致性检查（tools/ 脚本或 fetch-deps 校验
   扩展：三处 QGIS 版本口径提取比对，不一致即红——防再
   分裂）。

## 通用纪律（方向内全程有效）

- **分层**：不触 src/ 代码（纯 vendor/CI/文档面）；若护栏
   脚本需要，tools/ 按既有脚本风格。
- **诚实面**：Linux 侧未执行的操作如实记录（操作手册 +
   待执行清单），不冒记已验证；版本决策给判据不拍脑袋。
- **资源**：`-j8`（本方向构建面小——锁文件编辑为主）。
- **无人值守**：版本决策自行定案记 ledger（判据齐全）。
- **vendor 纪律**：改 vendor/sbm 需登记 PATCHES.md（本方向
  预计不触 sbm）；superbuild 补丁变更在 PATCHES/README 记录。
- **ledger**：`.goal-loop-ledger-dep-unify.md`。
- **多轮 review（硬要求）**：每批 → 可本机验证项全绿 →
  diff 自审（锁文件格式正确性/决策判据完整性/CI yaml 结构/
  文档同步/护栏有效 五维）→ 修复 → 再 review，至少两轮零
  High/Medium；Low 记 PR body。
- **提交**：原子提交，中文 conventional 前缀。

## Oracle 验收（逐条需验证证据）

1. 决策记录：两案对比 + 判据 + 弃案理由入 ledger（可复核）。
2. 版本统一：三处 QGIS 版本口径一致（护栏脚本输出零差异
   证据）；所选路线的锁文件/补丁变更与既有校验机制兼容
  （fetch-deps.sh --check-urls 可跑则跑，不可跑则结构核对
   记录）。
3. manifest：qgis_family 口径收紧或粒度边界记录（二选一
   有证据）。
4. #76：ci.yml linux job 走 vendored 路线（yaml 结构与既有
   job 对齐的核对记录）；BUILDING.md #76 段更新（状态如实：
   「已迁移待 CI 验证」）。
5. 护栏：一致性脚本对故意制造的三处版本分歧能打红
  （mutation 验证）。
6. 本机可验证项全绿：构建照常（依赖未变路径）；全量 ctest
   对照 R0 红集合 diff 为空。
