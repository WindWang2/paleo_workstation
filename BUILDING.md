# BUILDING

## 快速开始与 TTHW（plan §44.1）

```bash
./paleo-dev bootstrap   # preflight + 依赖 vendor（binary 路）
./paleo-dev build       # 配置 + 构建
./paleo-dev test        # QT_QPA_PLATFORM=offscreen 全套测试
```

- 引导后的可用性先由 `./paleo-dev selfcheck` 核对 provider、资源和渲染。
  全量测试范围以当前构建的 `ctest --test-dir build -N` 为准；不要把早期套件数
  或单机耗时当作当前首次构建、测试完成时间。
- 引导本身（一次性）：binary 加速档 ~10min（OSGeo4W / deb 闭包 / onnxruntime
  pin）；superbuild 首选路 ≤2h、磁盘 ≥60GB（启用步骤见
  `vendor/superbuild/README.md`，政策见下「依赖来源策略」）。
- 本机已装 QGIS 4.2.x 开发包（如 Arch `qgis 4.2.2`）时 bootstrap 短路 QGIS
  腿，只剩 onnxruntime 下载——注意这属**兜底档**，见下「依赖来源策略」。

## Data_service 子模块

`vendor/Data_service` 以 Git submodule 引入
[changyanyanchang/Data_service](https://github.com/changyanyanchang/Data_service)，
版本由主仓的 gitlink 固定。已有 checkout 或新建 worktree 后，在仓库根目录执行：

```bash
git submodule update --init --recursive -- vendor/Data_service
```

首次克隆也可使用 `git clone --recurse-submodules <主仓地址>`。
主项目通过 `cmake/extra-data-service.cmake` 嵌入其库目标，关闭独立演示、工具、
示例、测试与 SDK 安装，共用主项目已构建的 SBM 地震引擎。未初始化子模块时
configure 明确失败并提示上述命令；CI checkout 同样递归初始化子模块。

目录实体、资产、版本和关联的 SQL 读写已接入子模块；新建工程、五类井数据
导入及主要读取门面已接通版本化模型。交互编辑输出等剩余迁移仍在推进。
模型映射、兼容边界与剩余工作见
[Data_service 接入方案](docs/DATA_SERVICE_INTEGRATION.md)。子模块独立构建与
使用说明见 `vendor/Data_service/README.md`。

## 依赖来源策略（2026-10-01 起）

**原则：尽量不依赖系统库，尽量自编译 vendored。** 优先级：

### 依赖按用途分类（C++ 主线口径）

与来源路线（下）正交，按「谁在什么时候需要」分三类
（详见 `docs/CPP_PRIMARY_BOUNDARY.md`）：

| 类别 | 内容 | 缺席后果 |
|------|------|---------|
| Native Runtime | C++/Qt6/QGIS/GDAL/GEOS/ORT 二进制闭包 | 主程序无法构建/运行 |
| Development/Validation | python3（源码闸门/ctest/CI）、Ninja、ccache（可选加速）、lrelease（翻译编译）、pwsh（Windows 侧脚本自检） | python3 缺席 configure 直接 FATAL_ERROR（`paleo-test-infra.cmake` 的 `find_program(PALEO_PYTHON3)`，不静默变绿）；其余按各自既有降级行为（lrelease 缺席 WARNING、ccache 缺席直接编译、pwsh 缺席该自检不注册） |
| Optional Scripting | 用户显式启用的脚本控制台/REPL/MAMCL 所需解释器与 venv | 对应功能不可用，主程序不受影响（PATH 发现或 `PALEO_PYTHON` 显式指定） |

Native Runtime 依赖的来源路线：

1. **自编译 vendor（superbuild，首选）**——GEOS→PROJ→GDAL→QGIS 按源码
   tarball + SHA256 pin（`URL_HASH`）构建进 `vendor/superbuild/prefix`
   （启用步骤见其 README；周检 CI `.github/workflows/superbuild-weekly.yml`
   每周日在干净 runner 从零 configure+build 一轮，PR 触及
   `vendor/superbuild/**` 时增跑，#561 起接入——非 required check，首跑
   状态以 Actions 页为准）。动机：系统包状态不受本仓
   控制——发行版升级/卸载即破坏构建与运行（2026-10-01 本机实证：Arch
   系统 qgis/cmake 被卸载后，二进制缺 `libqgis_core.so.4.2.2` 无法启动，
   构建工具链同步失踪）；自编译 prefix 把版本、ABI、裁剪面钉进仓库，
   任何机器可复现。
2. **钉哈希的二进制 vendor 闭包（加速档）**——`vendor/prefix`（deb 闭包）
   / OSGeo4W（Windows）。仍是仓控 prefix 而非系统库；TTHW 快，作为 CI
   与新机的务实选择保留。
3. **系统包（仅兜底）**——发行版 QGIS 4.2.x 开发包只在上述两路都不可用
   时作临时兜底；CI 与发布构建禁止依赖系统包提供 QGIS/GDAL/PROJ/GEOS。

> **#76 状态（方向 71，2026-10-07）**：`.github/workflows/ci.yml` 的 Linux
> 侧（lint/linux/linux-perf）已从 qgis.org apt 系统 QGIS 迁到 deb 闭包
> vendored 路（bootstrap 拉 `vendor/prefix`，apt 只装政策例外面 Qt6/
> qtkeychain/工具链）；`tools/ci_apt_qgis.sh` 已删除。**状态如实：已迁移、
> 待 CI 实跑验证**（yaml 编写与结构核对在本机完成，GitHub Actions 语义
> 未在本方向内实跑）。`fetch-deps.sh --check-urls` 可做锁文件可达性冒烟
> （只 HEAD 探测，不下载）。
>
> **QGIS 版本口径（方向 71 统一）**：三路同一上游版本 **4.2.3**——
> superbuild 源码 pin（URL+SHA256）、deb 闭包（4.2.3+44resolute）、
> OSGeo4W 家族（4.2.x，installer 只有包名粒度，无法精确 pin——粒度边界
> 见 `vendor/manifest.json` notes；家族闸正则从 manifest 派生，单一来源）。
> 一致性由 `tools/check_qgis_versions.py` 门禁看守（ctest 项
> `qgis_versions`/`qgis_versions_selftest` + CI lint Source gates）：三处
> 口径不一致即红，防再分裂。升级 QGIS 时三处一起动，护栏会拦漏改。

例外（不 vendored，沿用系统/官方二进制）：Qt6（体积与构建时长，
superbuild 明示禁止 qt-everywhere 整块编译；走发行版或 OSGeo4W 同源）、
编译器工具链与构建依赖（flex/bison/nasm/python3）、glibc/libstdc++
（ABI floor，无法 vendored）、ONNX Runtime（官方 release SHA256 pin，
与 QGIS 路线正交）。Office 原件使用钉死提交的 **ranuts/document**
静态编辑页（OnlyOffice WASM，无文档服务器），不依赖 LibreOffice，也不把原件转成 PDF。
`vendor/fetch-ranuts-document.sh` 把构建结果装到 `vendor/ranuts-document/`（gitignored）。
该目录是 AGPL-3.0 派生作品，分发应用时必须带上对应源码，并保留编辑器里的 ONLYOFFICE 标识。
运行时先查 `PALEO_OFFICE_EDITOR`（指向含 `index.html` 的目录），再查应用同目录
`office-editor/`、`../vendor/ranuts-document/`、`../libexec/office-editor/`。
主程序只在 `127.0.0.1` 上打开这个目录和当前原件。无 WebEngine 时同一地址交给系统浏览器。

```bash
# 需要 Node 与 pnpm。产物不入库。
vendor/fetch-ranuts-document.sh
```

`vendor/fetch-calligra.sh` 仍可源码构建 Calligra 26.08.2，但 Office 预览不再调用它。
旧 `vendor/fetch-libreoffice.sh` 仅服务显式调用的历史转换 API。
Qt6 与 KDE Frameworks 6 开发依赖优先由 `PALEO_KF6_PREFIX` 指定前缀，系统开发包
为兜底；Qt/KF6 必须与主程序保持 ABI 一致。当前 Linux 构建使用 Qt 6.11.2、
KF6 6.30.0，Qt ≥6.5 / KF6 ≥6.0 是 Calligra 上游最低要求，与 Office 编辑页无关。

#### LibreOffice vendored（双平台，legacy 转换 API 专用）

`vendor/fetch-libreoffice.sh` 双平台同一布局 `vendor/libreoffice/`（resolver 从应用
目录向上探测 `program/soffice[.exe]`，三级序 env `PALEO_SOFFICE` > vendored > PATH）：

| 平台 | 官方包 | 解包 | 剪裁后体量 |
|---|---|---|---|
| Linux x64 | deb tarball（`deps.libreoffice` pin） | ar+tar 解 12 个 headless 子集 deb | ~550MB |
| Windows x64 | MSI（`deps.libreoffice_win` pin） | `msiexec /a` administrative extract（无需提权） | ~920MB |

两平台钉同一上游版本 26.2.6.3（`tools/check_lo_pin.py` ctest 门禁看守版本对齐 +
url/sha256/layout 口径）。Windows 段剪掉 dict-*/语言 res/python/help/shell 扩展
（Linux 白名单本就不取的面），并在解包后做 `icacls /setintegritylevel medium`
归一——Low 完整性标签的工作区（沙箱防护）会让 soffice 的 user-installation 引导
以 exit 77 失败（方向 94 R0 实测，详见 PR 描述）。Windows bootstrap
（`paleo-dev.ps1 bootstrap`）自动补这条腿；产物 DERIVED 版本带
`converterVersion`/`converterPlatform` 血统（跨平台转换差异可追溯）。

### glibc 三档口径（显式分层，非混乱）

| 档 | 下限 | 出处 | 语义 |
|---|---|---|---|
| binary vendoring 总地板 | 2.41 | `vendor/bootstrap.sh` preflight | 走 binary 路（deb 闭包/ORT）的宿主最低要求 |
| deb 闭包锁 | 2.43 | `vendor/bootstrap.sh`（闭包腿）/ `fetch-deps.sh` 头注 | 已提交锁是 Ubuntu 26.04（resolute）闭包，链接 GLIBC_2.43 符号；低 glibc 宿主能解包不能运行，提前拒绝 |
| ONNX Runtime abi_floor | 2.28 | `vendor/manifest.json` `abi_floor` | 官方 manylinux_2_28 构建；与 QGIS 路线正交 |
| Calligra | 跟随本机构建的 Qt/KF6 ABI | `vendor/fetch-calligra.sh` | 独立页面渲染进程，不进主程序链接面 |

更低 glibc 宿主走 superbuild（"superbuild-on-oldest-target"）。

### deb 闭包锁的信任根（PW-053）

- **下载期**：`vendor/fetch-deps.sh` 对锁里每个 .deb 校验 size + SHA256，不符即删文件并失败（fail closed）；pool 与 snapshot 回退两条路都走同一校验。
- **锁生成期**：锁由维护者主机 `apt-get --print-uris` 生成，来源可信度 = 该主机 apt 对 Ubuntu / qgis.org 仓库 `InRelease` 的 GPG 校验。脚本本身不再做签名校验，因此**只在干净、apt 源配置为官方签名仓库的主机/容器（ubuntu-26.04）上刷新锁**，并在 PR 中审阅锁 diff。
- qgis.org 第三方源没有 snapshot 回退，上游删包会导致锁腐烂；由 `deb-lock.yml` 每周探测发现。

## QGIS prefix 解析顺序（CMakeLists.txt:16 起）

主构建不写死 QGIS 位置，按序解析：

1. `QGIS_PREFIX` CMake 变量；未给则读环境 **`QGIS_PREFIX_PATH`**；
2. 命中后把它（及 `<prefix>/apps/qt6`，OSGeo4W 布局）前置进
   `CMAKE_PREFIX_PATH`，再在 `<prefix>/apps/qgis/include`、
   `<prefix>/include/qgis`、`<prefix>/usr/include/qgis` 等布局里找
   `qgsapplication.h` 与 `-lqgis_{core,gui,analysis}`；
3. 都没给 → 系统路径（`/usr/include/qgis`、`/usr/lib`）——仅兜底档。

命中的 `QGIS_PREFIX` 还会编译进二进制（`PALEO_QGIS_PREFIX_DEFAULT`）：
运行时 `QGIS_PREFIX_PATH` env 未设时，provider/srs.db 默认解析到构建所链的
prefix（而非硬编码 `/usr`），vendored 库自身带 `$ORIGIN` RUNPATH——裸跑
`build/paleo` 也是全 vendor 栈。ctest 沙箱同样注入 `QGIS_PREFIX_PATH`。

vendor 路径对照（按策略优先级）：

| 来源 | QGIS_PREFIX_PATH | 由谁准备 |
|---|---|---|
| superbuild 自编译（首选） | `<repo>/vendor/superbuild/prefix` | `vendor/superbuild/`（见其 README） |
| qgis.org deb 闭包（加速档） | `<repo>/vendor/prefix/usr` | `./vendor/fetch-deps.sh`（lock 锁 SHA256） |
| OSGeo4W（Windows CI） | bootstrap 注入（`apps/qgis` 布局） | `./paleo-dev.ps1 bootstrap` |
| 发行版系统包（仅兜底） | 不需要（系统路径即可） | 发行版包管理器 |

## 独立 worktree 开发

从明确的基线建分支；`master` 与 `origin/master` 不一致时先确认要用哪一个。
主 checkout 的未提交改动不会复制到 worktree。通常使用 `codex/<主题>`，
任务指定的 `refactor/`、`goal/` 等分支名优先。每个 worktree 使用自己的 `build/`。

gitignored 依赖不会随 worktree 出现。本机共享已准备的依赖时，以主仓绝对路径
建立 symlink（以下主仓路径按实际位置替换；不要在共享 prefix 中清理或重建）：

```bash
git worktree add ../paleo_workstation-refactor -b refactor/dedup-docs-tests master
cd ../paleo_workstation-refactor
ln -s /home/kevin/projects/paleo_workstation/vendor/superbuild/prefix vendor/superbuild/prefix
ln -s /home/kevin/projects/paleo_workstation/vendor/onnxruntime vendor/onnxruntime
if [ -d /home/kevin/projects/paleo_workstation/vendor/prefix ]; then
  ln -s /home/kevin/projects/paleo_workstation/vendor/prefix vendor/prefix
fi
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DQGIS_PREFIX=/home/kevin/projects/paleo_workstation/vendor/superbuild/prefix
cmake --build build -j8
ctest --test-dir build -j8 --output-on-failure
```

显式 `QGIS_PREFIX`（或 env `QGIS_PREFIX_PATH`）指向不存在的路径时，configure
直接 FATAL_ERROR 并给出修复入口（早失败替代旧的静默回落系统栈——旧行为会
链接系统库的同时把运行时 prefix 编译成悬空路径，见 TODOS.md P2 收口记录）；
未显式给 prefix 时 configure 会用 STATUS 行明示命中的 QGIS 栈路线。护栏
`ctest -R first_configure`（UNIX）用 stub prefix 覆盖依赖存在/缺失/幂等复用
三情形。QScintilla 缺头 warning 在不包含 QGIS 代码编辑器头的 Linux 构建中
允许降级；Windows 使用已有 `CMAKE_PREFIX_PATH` 前缀或显式
`-DQSCINTILLA_INCLUDE_DIR=<安装目录>/include` 提供头，详见任务框架历史账本。

## Windows 本机开发（localdeps 统一链，方向 72）

Windows 构建会从固定的 `vendor/Data_service/vendor/freexl` 源码编译兼容版
`paleo_freexl.dll`，放在应用与测试程序同目录，保留 BIFF 内嵌图表等格式修复，避免
加载 PATH 上未经修补的版本，也不替换 GDAL 自身的 FreeXL。构建按依赖闭包选择
`iconv-2.dll` 或 `iconv.dll` / `charset.dll`；安装树携带
FreeXL 的来源、许可和补丁说明。显式 `PALEO_FREEXL_LIBRARY` 仍可用于诊断。

`paleo-dev.ps1 build` 会用实际 MSVC + Ninja 的小型头文件夹具验证依赖前缀，
修正本地化 `/showIncludes` 与控制台代码页造成的错误匹配。仅设置 `VSLANG`
不足以解决未安装英文编译器语言包的机器。首次修复会清理生成目标后重建，
避免新旧类布局混用；以后通过前缀记录保持正常增量构建，不反复清库。
构建日志不应整屏泄漏 `/showIncludes` 行；可用 `ninja -C build -t deps`
核对头文件确已登记。手工运行 CMake/Ninja 的本地化环境同样需要验证这项。
若手工增量构建已在错误前缀下产出对象，仅修正 `rules.ninja` 不能补回丢失的
依赖记录；即使前缀 stamp 没变化，也须清理后重编，再核对实际对象的 `#deps` 非零。
`paleo-dev.ps1` 保留 UTF-8 BOM，以便 Windows PowerShell 5.1 正确解析中文字符串。

**背景（历史注记，勿再踩）**：本机曾以 `C:/deps/Qt/6.8.0/msvc2022_64` 编译、
而 QGIS 前缀与依赖闭包按 Qt 6.11 构建——两版 Qt 同进程混载，全量回归长期
185 红/293（含 5 个伞式测试 0xc0000139 ENTRYPOINT_NOT_FOUND），只能靠
「改前/改后红集合 diff」承载验收。「PATH 前置 `paleo-qgis-deps/Library/bin`
让进程能启动」的旧技巧只解决启动、不解决混链，已废弃。

**统一口径（2026-10-07 起）**：编译与运行同链 Qt 6.11.2，三方来源如下——

| 角色 | 位置 | 内容 |
|------|------|------|
| Qt 全家 + GDAL/GEOS/sqlite/keychain 闭包 | `~/paleo-qgis-deps`（conda 环境，qt6-main 6.11.2） | 头 + cmake 配置 + 运行 DLL（`Library/bin`，PATH 前置） |
| QGIS 4.2.0 前缀 | `~/paleo-qgis-prefix` | qgis_core/gui/analysis，按 Qt 6.11.2 构建 |
| qtpdf 覆盖层 | `C:/deps/Qt/6.11.2/msvc2022_64` | 官方 Qt 6.11.2 qtpdf 扩展（conda 闭包不含 Qt6Pdf；Qt6Config 只在自身前缀内找组件，故走 `QT_ADDITIONAL_PACKAGES_PREFIX_PATH` 补位） |

日常入口 `./paleo-dev.ps1`（build/test/selfcheck 自动进 localdeps 路线；
`vendor/osgeo4w` 存在时 vendored 路线优先，CI 口径不变）。等价手工接线：

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo `
  "-DCMAKE_PREFIX_PATH=$HOME/paleo-qgis-deps/Library" `
  "-DQT_ADDITIONAL_PACKAGES_PREFIX_PATH=C:/deps/Qt/6.11.2/msvc2022_64" `
  "-DQGIS_PREFIX=$HOME/paleo-qgis-prefix" `
  "-DQSCINTILLA_PREFIX=$HOME/paleo-qgis-deps/Library/include/qt6"
# 构建/测试时 PATH 前置（qt6 工具 lrelease/moc 也依赖它解析 DLL）：
#   $HOME/paleo-qgis-deps;$HOME/paleo-qgis-deps/Library/bin;$HOME/paleo-qgis-prefix/bin;C:/deps/Qt/6.11.2/msvc2022_64/bin
# 并设 GDAL_DATA=$HOME/paleo-qgis-deps/Library/share/gdal、
# PROJ_LIB=$HOME/paleo-qgis-deps/Library/share/proj（./paleo-dev.ps1 已内置）。
# 测试还需（ps1 test 已内置）：TEMP/TMP 与 SEISMIC_INDEX_CACHE_DIR 指到
# build/paleo-tmp 内子目录（沙箱监狱对策，见下）。PALEO_PYTHON=
# $HOME/paleo-qgis-deps/python.exe（conda 布局 python 在根不在 Library/bin）
# 由 ps1 localdeps 设置（方向 81 起；此前仅终跑脚本有），并经
# -DPALEO_TEST_PYTHON 进 cache、由 ctest 注入每个测试——手工 configure 时
# 同样加 "-DPALEO_TEST_PYTHON=$HOME/paleo-qgis-deps/python.exe"。
```

要点：

- cmake ≥3.28（工程要求；VS 自带 3.27 不够——本机用 `C:/Qt/Tools/CMake_64`
  的 3.29）。
- conda 布局里 Qt6 工具在 `Library/lib/qt6[/bin]`，其 DLL 依赖由
  `Library/bin` 供给——configure/build/test 全程都要 PATH 前置 deps bin，
  否则 lrelease 段错误式失败、MOC 偶发崩。
- `Library/bin` 同时住着 qt5 工具（本 env 含 qt5 包）——lrelease 5.15.15
  与 6.11.2 并存，勿用 PATH 裸调 `lrelease`，CMake 钉的是 qt6 绝对路径。
- 退役项：`C:/deps/Qt/6.8.0/msvc2022_64` 与 `C:/deps/qscintilla-install`
  不再进任何 CMAKE_PREFIX_PATH/PATH（QScintilla 头随 conda Qt 走
  `Library/include/qt6/Qsci`，2.14.1 同版）。
- 根位置可用 `PALEO_LOCAL_DEPS` / `PALEO_QGIS_PREFIX` / `PALEO_QTPDF_PREFIX`
  覆盖（paleo-dev.ps1 localdeps 路线）。

qtpdf 覆盖层一次性安装（已在 `C:/deps/Qt/6.11.2/msvc2022_64` 就位的可跳过）：
官方 extensions 仓库取 6.11.2 msvc2022_64 的 qtpdf 7z（ Updates.xml 里
`extensions.qtpdf.6112.win64_msvc2022_64`，版本 `6.11.2-0-202608131017`），
按同目录 `.7z.sha1` 校验后解压覆盖到该前缀。aqtinstall≤3.3 不识别 6.10+
的新仓库布局，`-m qtpdf` 会 404，需手工取档或升级 aqt。

**防回归自检**：`./paleo-dev.ps1 checkenv`（`tools/check_qt_env.ps1`）比对
CMakeCache 的编译侧 Qt 与 DLL 搜索序解析到的运行侧 Qt：major.minor 不一致
即报警退出非零（混链特征）；patch 不一致给警告。构建机换 Qt 后先跑它。

**本机沙箱文件监狱（历代「QTemporaryDir 红脸」真身，2026-10-07 定案）**：
本机安全策略对**镜像文件位于仓库树内的进程**实施写限制——只能写树内路径；
`%TEMP%` 在树外 → `QTemporaryDir`/`QFile` 构造即「拒绝访问」（isValid()=FALSE
无 syscall，cmd.exe 拷入树内同样写不了 %TEMP%，拷出树外全绿——OS 级实证，
与 Qt/QGIS/混链无关；历代 185 红的主体即此）。junction 伪装出树无效（策略
解析真实路径）。**对策**：localdeps 路线下 `paleo-dev.ps1 test/selfcheck`
自动把 `TEMP/TMP` 重定向到 `build/paleo-tmp`（树内、浅层，远离
CMakeLists 注记的 MAX_PATH 深路径顾虑）。根治需在沙箱策略侧放行本仓库
的 `%TEMP%` 写——机器配置问题，移交用户决策。

**环境债二期（方向 81）**：方向 72 终态 16 红逐条处置，名单与判定见
`tools/env_redset.json` / `tools/check_env_redset.py`（`ctest -L envdebt`
一键重跑 16 项）：

- **srs.db**：`./paleo-dev.ps1 ensure-resources` 按 `vendor/deb-closure.lock`
  同源取 qgis-providers-common + qgis-common（_all 包）补齐
  `~/paleo-qgis-prefix/resources/**`，`srs.db = cp srs-template.db`
  （Debian postinst 同义）；localdeps 的 test/selfcheck 缺 srs.db 时自动调用。
  Ubuntu pool 失效时按同一 `vendor/deb-closure.snapshot` 回退，摘要仍取锁内值；
  `PALEO_DEB_SNAPSHOT` 覆盖口径与 `fetch-deps.sh` 一致。
  解 `data.tar.zst` 需 Python≥3.14、`zstandard` 模块或 PATH 上的 `zstd` 之一
  （conda deps 的 `Library/bin` 通常带 zstd.exe）。
- **QSettings**：测试 exe 静态初始化期把本进程 HKCU 重映射到
  `build/ctest-home/<test>/registry.hiv` 私有 hive（产品仍 NativeFormat，
  零迁移）；测试不再读写/清空开发者真实的 `HKCU\Software\paleo\paleo`。
  直跑测试 exe（不经 ctest）时 hive 落 `%TEMP%\paleo-test-registry\`；
  `PALEO_TEST_REGISTRY_HIVE=native` 可关闭。
  ctest 指定的 hive 挂载失败时在进入测试前退出；直跑两档沙箱都失败时也退出，
  避免 `.clear()` 落回真实用户配置。
- **perf**：tst_cache_las / tst_singlefactor_perf / tst_startup_trace 按名单
  豁免（只限 localdeps、只认各自预算断言文案，阈值不动）；`paleo-dev.ps1 test`
  在 ctest 失败后先过检查器，全部为豁免即放行。

本机 Oracle 双遍（完整范围，含 perf，强制串行；每遍保存自己的首轮证据）：

```powershell
$savedArgs = $env:PALEO_CTEST_ARGS
$evidence = Join-Path $PWD ("build/envdebt-evidence/" + (Get-Date -Format 'yyyyMMdd-HHmmss'))
try {
  $env:PALEO_CTEST_ARGS = '-j 1'
  ./paleo-dev.ps1 checkenv
  foreach ($i in 1..2) {
    ./paleo-dev.ps1 test
    $run = Join-Path $evidence "run$i/Testing"
    New-Item -ItemType Directory -Force $run | Out-Null
    Copy-Item build/Testing/ctest-junit.xml $run
    Copy-Item build/Testing/qtest-first-run $run -Recurse
  }
  & $env:PALEO_PYTHON tools/check_env_redset.py --build "$evidence/run2" `
    --compare-build "$evidence/run1" --route localdeps
  if ($LASTEXITCODE -ne 0) { throw '环境债双遍结果未满足 Oracle' }
} finally { $env:PALEO_CTEST_ARGS = $savedArgs }
```

检查器要求双遍测试名集合相同、16 项齐全、13 个已修项连续通过（至少 10 的
收敛门），其余只允许三项指定预算豁免。平台与实际全量范围须在 ledger 中
记录；Linux 报告和 CI 的 `-LE perf` 子集不能替代 Windows localdeps 验收。

## 平台 × 版本矩阵

| 平台 | 状态 | 依赖来源 |
|------|------|----------|
| Linux x86_64：deb 闭包需 glibc≥2.43（Ubuntu 26.04 / 同代 Arch）；Debian 13（glibc 2.41）不能运行该闭包，须走 superbuild | CI 配置为 Ubuntu 26.04；实际通过情况查看对应提交的 CI 记录 | superbuild 自编译 prefix（首选）→ deb 闭包 `vendor/prefix/usr`（加速档，CI 现行）；发行版 QGIS 4.2.x 仅兜底 |
| Windows x86_64 | CI 配置为 windows-2025 四分片，每片串行；本机支持 localdeps 同链开发 | CI：OSGeo4W 包闭包由 `vendor/manifest.json` 钉版；本机：`~/paleo-qgis-deps` + `~/paleo-qgis-prefix` + 同版 qtpdf 覆盖层（见上节） |
| 更低 glibc 的 Linux x86_64 宿主 | 在目标最低系统上源码构建后验证，不可直接使用较新 glibc 的二进制闭包 | superbuild-on-oldest-target（ExternalProject） |

## 依赖（vendor manifest pin）

QGIS 4.2.x（三路口径统一上游 4.2.3，`tools/check_qgis_versions.py` 门禁看守）· Qt ≥6.6 · GDAL · PROJ · GEOS · QCA-qt6 · QtKeychain-qt6 · libspatialindex · exiv2 · libzip · OpenSSL · sqlite3/spatialite · **ONNX Runtime 1.30.0**（官方 release，sha256 `a5ed5a3c…3b3fd`，manylinux_2_28）。Ubuntu 26.04 的 deb 完整闭包和 SHA-256 在 `vendor/deb-closure.lock`（库解包在 `vendor/prefix/usr/lib/<multiarch>`，`./paleo-dev` 已加入 `LD_LIBRARY_PATH`）；Ubuntu pool 会删除被安全更新取代的旧版本，因此 `fetch-deps.sh` 在 pool 404 时回退到 `https://snapshot.ubuntu.com/ubuntu/<ts>/`，时间戳记录在 `vendor/deb-closure.snapshot`（`--update-lock` 会刷新；可用 `PALEO_DEB_SNAPSHOT` 覆盖），内容仍由锁内 SHA-256 校验；OSGeo4W 安装器摘要在 `vendor/manifest.json`。

## 测试布线约定（devex）

- **注册**：`add_paleo_test(name [LIBS ...])`（CMakeLists.txt）。单参 = 伞式
  旧式契约，回退链 `paleo_core`（cmake/extra-*.cmake 并行方向零改动可用）；
  `LIBS` 给该测试 include 面的最小闭包——touch 单模块不再牵连全部测试
  relink（对比数据 docs/progress/devex.md）。首模块决定 ctest `LABELS`
  （`ctest -L io` / `-LE core` 分步筛选）。
- **QGIS prefix**：测试 main() 统一读环境 `QGIS_PREFIX_PATH`（缺省 `/usr`）；
  `./paleo-dev test` 在 deb 闭包路（`vendor/prefix` 存在）自动注入 prefix 与
  `LD_LIBRARY_PATH`。
- **并行安全**：每个 ctest 项自动获得独立 `XDG_CONFIG_HOME`/`XDG_DATA_HOME`/
  `HOME` 沙箱（`build/ctest-home/<test>/`），QSettings 不再互踩真实用户配置
  ——Linux 可用 `ctest -j8`；构建和测试并行度一律不超过 8，不用 `$(nproc)`。
  **已知边界：Windows NativeFormat 走注册表
  不受 env 控制**，Windows 侧保持串行 ctest。
- **层护栏**：`ctest -R layering`（提示）/`layering_strict`（防回升闸门：
  baseline 非空或可收缩即红）/`layering_selftest`（扫描器自检）/
  `layering_symbol_audit`（零基线：非视图层 ui 类构造即红）/
  `layering_transitive`（传递债四指标对 `tools/transitive-baseline.txt`
  只缩不涨；heavy_* 恒可闸，func/ui 需 QGIS 头目录——CI lint 全闸）。
- **测试车道与快速道（#503 反馈周期）**：LABELS 体系 = 模块名（`io`/`qgis`/
  `ui`/`workflow`…，首模块决定，伞式回退 `core`）+ 语义标签
  `perf`（墙钟预算，`-LE perf` 排除、`-L perf` 串行单独跑）/
  `golden`（渲染像素比对）/`envdebt`（`ctest -L envdebt` 一键重跑环境债
  清单）/`heavy`（全栈/独占类功能巨测：tst_panels、tst_edittools、
  tst_import、tst_mappingworkbench、tst_correlation_full 等）。
  本地快速道：`ctest -LE 'heavy|perf' --output-on-failure`（巨测与预算类
  之外的全量，分钟级）；再叠 `PALEO_TEST_FAST=1` 可让个别巨测内部减样
  （tst_import 并发导入 12→4、tst_mappingworkbench 合并轮数 20→5——
  默认语义不变，CI 不设此变量）。快速道不能替代约定的完整验证范围。
  Windows CI 已按测试序号分片，各片内保持串行；本机不要按旧耗时估算当前全量。

## 加速与实验档（devex）

| 旋钮 | 说明 |
|------|------|
| ccache/sccache | PATH 上有即自动挂 launcher（`-DCMAKE_CXX_COMPILER_LAUNCHER=` 显式指定优先；`PALEO_NO_CCACHE=1` 关）。CI 缓存 `~/.cache/ccache` |
| `PALEO_ENABLE_ASAN=ON` | ASAN 实验档；测试注入 `ASAN_OPTIONS=detect_leaks=0`（QGIS/Qt 设计内"泄漏"面，suppressions 见 `tools/lsan-suppressions.txt`） |
| `PALEO_ENABLE_UBSAN=ON` | UBSAN 巡检档（可恢复，打印栈） |
| `PALEO_UNITY_BUILD=ON` | 实验 unity build：逐产品 target、vendor 三家（sbm/segyio/saribbon）显式排除；有跨 TU 静态符号合批风险，仅供本地加速实验 |
| `PALEO_ENABLE_LTO=ON` | 链接期优化档（`-flto=8 -ffat-lto-objects`，默认 OFF；配置期探测工具链支持）。2026-10-01 交错三轮实测：LAS 冷解析 -40.7%、SEG-Y 索引命中 -45.2%、catalog 灌库/查询 -18~25%，无一致回归；代价=全量链接 ~2.3×。证据与 -O3 否决记录见 `docs/perf/BUILD_OPT.md` |
| clang-tidy 门禁 | `python3 tools/check_tidy.py`——只扫相对 merge-base 改动的 src/ TU；配置 `tools/.clang-tidy`；CI 钉 `clang-tidy-20` |

PCH 评估结论（T6，定性）：模块静态库已把 Qt/QGIS 头的重压摊到 10 个
产品 target，重头是 ui/workflow 两个。PCH 预热
（`target_precompile_headers` 塞 `<QtGui>`/`qgsapplication.h`）的理论收益
集中在这两个 target 的全量编译；但代价是任何 PCH 头变更强制全 target
重编（正好打在本仓最重的 ui 上），与 AUTOMOC/unity 组合易碎，且会改变
编译命令形状让 ccache 全量 miss 一次。当前全量构建在 16 核已是分钟级、
增量由 ccache 兜底——**不落地 PCH**，ccache 是本仓收益/风险比更高的路；
编译时间成为痛点时再按当时数据重评。分析全文见 docs/progress/devex.md。

## 新模块脚手架（devex）

```bash
scripts/new_module.sh <name> <层> [target-lib]   # 生成层标记模板+测试骨架，
                                                 # 同步 tools/layering_vocab.json
```

词表外置后，新顶层模块不登记 `tools/layering_vocab.json` 会被
layer-marker 全量判违规（by design，防漏网）；脚本同时打印
`cmake/extra-<wave>.cmake` 接线片段。

## 常见失败表

| 现象 | 原因 | 修复 |
|------|------|------|
| `QDomDocument: No such file` | 缺 Qt6Xml 等模块 dev 包 | 装 qt6 全组 dev（见 CMakeLists find_package 列表） |
| `defaulted operator==` 报错 | C++ 标准 <20 | CMAKE_CXX_STANDARD=20（已钉） |
| `providerList() empty` | prefix 未指向 vendor/qgis 资源根 | selfcheck 检查 setPrefixPath + QGIS_PREFIX 环境 |
| srs.db missing | vendor prefix 缺 share/qgis/resources | 重跑 `paleo-dev bootstrap` |
| `GDALPolygonize` 无 geotransform 警告 | 测试栅格无仿射 | GDAL 3.11+ 需 SRC_METHOD=NO_GEOTRANSFORM（已知，警告非阻塞） |

## 升级流程

Debian 闭包升级时，在目标发行版且已配置 QGIS 官方 apt 源的干净环境运行 `./vendor/fetch-deps.sh --update-lock --print-only`，审查并提交新的 `vendor/deb-closure.lock`。正常 bootstrap 只读取锁文件，并仅解包锁内经过大小和 SHA-256 校验的文件。superbuild 升级则同步改 `vendor/superbuild/CMakeLists.txt` 的 `QGIS_URL` 与 `URL_HASH`（补丁文件名随版本重命名，需对目标版本 tarball 重验证 dry-run 干净），`vendor/cache/tarballs/` 缓存与哈希一并更新。**三路版本口径（superbuild 精确 / deb 闭包上游 / manifest `qgis_family` 家族）必须一起动**——`tools/check_qgis_versions.py`（ctest `qgis_versions` + CI lint）不一致即红。ONNX Runtime 升级则更新 `vendor/manifest.json` 的版本和摘要。Windows bootstrap 校验安装器 SHA-256，并核对 manifest 的 `qgis_family`、`packages` 与 `closure`。直接包与依赖闭包版本由 `tools/pin_osgeo4w.py` 刷新后审查提交；升级时复核 CI。

## 性能度量与 CI 产物

Phase 0 和早期增量构建的绝对耗时保留在 `docs/perf/` 与交付账本中，不能代表当前规模。
当前增量构建使用 `tools/measure_incremental.sh`，记录实际重编 TU、构建步骤及比率：

```bash
tools/measure_incremental.sh build src/app/appcontext.cpp build/incremental-baseline.jsonl
```

CI Linux 在 Selfcheck 后执行该测量，并上传 `incremental-baseline-linux` 产物。
它是非阻断的趋势观察项；比较时保留提交、工具链和构建配置，绝对时长不作跨机判定。

### CI Linux 的调试信息体积

Linux 必过车道使用 RelWithDebInfo 的 `-O2 -g1 -DNDEBUG`：保留符号和源码行位置，
减少全量测试可执行文件的 DWARF 磁盘占用，防止链接末期耗尽 runner 磁盘。
只在该 CI job 预置 CMake 缓存，随后仍走 `paleo-dev build` 并运行全部目标及测试；
本机默认 RelWithDebInfo 和 sanitizer 车道不受影响。
