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
- 新构建使用仓库控制的 QGIS 前缀。自编译路线先按 `vendor/superbuild/README.md`
  准备并激活前缀，再配置主项目。

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

## 依赖来源策略

**包括 QGIS 在内，项目使用的第三方库及其传递依赖均由仓库管理。**
这涵盖头文件、开发库、共享库、插件及运行所需资源。仓库固定来源、版本和完整性信息；
大型归档和构建产物可由引导脚本生成到 gitignored 的项目依赖前缀，不要求提交进 Git。
编译、链接、测试和发布运行使用同一套兼容依赖，不从宿主系统目录补缺。

### 依赖按用途分类（C++ 主线口径）

与来源路线（下）正交，按「谁在什么时候需要」分三类
（详见 `docs/CPP_PRIMARY_BOUNDARY.md`）：

| 类别 | 内容 | 缺席后果 |
|------|------|---------|
| Native Runtime | C++/Qt6/QGIS/GDAL/GEOS/ORT 二进制闭包 | 主程序无法构建/运行 |
| Development/Validation | 编译器、SDK、CMake、Ninja、宿主 Python（源码门禁）、ccache、lrelease、pwsh | 宿主工具可以独立供应；随 Qt 等第三方库提供的工具须与项目所用版本一致 |
| Optional Scripting | 用户显式启用的脚本控制台/REPL/MAMCL 所需解释器与 venv | 对应功能不可用，主程序不受影响（PATH 发现或 `PALEO_PYTHON` 显式指定） |

Native Runtime 依赖的来源路线：

1. **自编译 vendor（superbuild，首选）**——Debian 13 归档锁提供 Qt、插件和配套
   开发库，zlib → Zstd → SQLite → GEOS → PROJ → GDAL → QGIS 从校验后的源码
   构建进 `vendor/superbuild/prefix`。启用、离线重放及锁刷新步骤见
   [源码依赖闭包](vendor/superbuild/README.md)。
2. **锁定的二进制 vendor 闭包（加速档）**——无法合理自编译时，可从上游发行包、
   deb 包或 OSGeo4W 等渠道取包，按明确版本和完整性信息校验后放入项目控制的
   前缀。发行包是供货来源，不安装成系统依赖；不能混用宿主机的同名库。

宿主边界限于操作系统自身的基础 ABI（例如 Linux 动态加载器和 glibc、
Windows 系统 API）及用于构建和校验的工具。工具链的 ABI 兼容要求应明确；
不能用它作为从系统安装或加载 Qt、QGIS、GDAL 等项目库的理由。

**迁移现状**：Linux 二进制路线从同一份 Ubuntu 26.04 归档锁
供应 Qt6、QtKeychain、QGIS、GDAL、PROJ、GEOS 及配套工具、资源和插件。
Windows 二进制路线使用逐归档校验的 OSGeo4W 离线锁。依赖变更以本地构建、
测试和源码门禁验收。
改动依赖供应或发现路径时，应验证干净机器在没有系统项目库的情况下能构建和测试，
并检查链接、运行时库及插件解析均来自受控前缀或发布目录。缺失依赖时应报出
所需前缀和补齐方法。
>
> **QGIS 版本口径（方向 71 统一）**：三路同一上游版本 **4.2.3**——
> superbuild 源码 pin（URL+SHA256）、deb 闭包（4.2.3+44resolute）、
> OSGeo4W 离线归档锁（实际包修订见 `vendor/osgeo4w-offline.lock.json`）。
> 一致性由 `tools/check_qgis_versions.py` 门禁看守（ctest 项
> `qgis_versions`/`qgis_versions_selftest` + CI lint Source gates）：三处
> 口径不一致即红，防再分裂。升级 QGIS 时三处一起动，护栏会拦漏改。

以下是旧构建链的供应记录，不能作为新建系统依赖的依据：
Qt6（体积与构建时长，
superbuild 明示禁止 qt-everywhere 整块编译；走发行版或 OSGeo4W 同源）、
编译器工具链与构建依赖（flex/bison/nasm/python3）、glibc（宿主 ABI，构建设最低版本）、ONNX Runtime（微软 NuGet 原生库，校验归档大小和 SHA-256，
与 QGIS 路线正交）。Office 原件使用钉死提交的 **ranuts/document**
静态编辑页（OnlyOffice WASM，无文档服务器），也不把原件转成 PDF。
`vendor/fetch-ranuts-document.sh` 把构建结果装到 `vendor/ranuts-document/`（gitignored）。
该目录是 AGPL-3.0 派生作品，分发应用时必须带上对应源码，并保留编辑器里的 ONLYOFFICE 标识。
运行时先查 `PALEO_OFFICE_EDITOR`（指向含 `index.html` 的目录），再查应用同目录
`office-editor/`、`../vendor/ranuts-document/`、`../libexec/office-editor/`。
主程序只在 `127.0.0.1` 上打开这个目录和当前原件。无 WebEngine 时同一地址交给系统浏览器。

```bash
# 需要 Node 与 pnpm。产物不入库。
vendor/fetch-ranuts-document.sh
```

完整的 `paleo-dev bootstrap` / `paleo-dev.ps1 bootstrap` 会准备这个组件。
已有组件时，可用 `PALEO_OFFICE_EDITOR` 环境变量或 CMake 参数
`-DPALEO_OFFICE_EDITOR=<目录>` 指向其所在位置。构建树会接入
`office-editor/`，安装树把组件放到 `libexec/office-editor/`。
`cmake --install` 与 Windows 两套便携打包工具在缺少编辑器入口或 JS/WASM
运行文件时会失败。仅取依赖的 `bootstrap fetch-only` 可供 CI 夹具测试使用。

`vendor/fetch-calligra.sh` 仍可源码构建 Calligra 26.08.2，但 Office 预览不再调用它。
其消费的 `cmake/CalligraRenderer.cmake` 由该脚本注入 Calligra 构建、本树零引用属有意且无 CI 车道——改动须手跑该脚本验证（见 `cmake/README.md`，#874）。
通过 `PALEO_KF6_PREFIX` 为这个独立工具指定受控 Qt6 和 KF6 开发前缀。
Qt/KF6 必须与主程序保持 ABI 一致。
当前 Linux 构建使用 Qt 6.11.2、
KF6 6.30.0，Qt ≥6.5 / KF6 ≥6.0 是 Calligra 上游最低要求，与 Office 编辑页无关。

### glibc 三档口径（显式分层，非混乱）

| 档 | 下限 | 出处 | 语义 |
|---|---|---|---|
| binary vendoring 总地板 | 2.41 | `vendor/bootstrap.sh` preflight | 走 binary 路（deb 闭包/ORT）的宿主最低要求 |
| deb 闭包锁 | 2.43 | `vendor/bootstrap.sh`（闭包腿）/ `fetch-deps.sh` 头注 | 已提交锁是 Ubuntu 26.04（resolute）闭包，链接 GLIBC_2.43 符号；低 glibc 宿主能解包不能运行，提前拒绝 |
| ONNX Runtime abi_floor | 2.28 | `vendor/manifest.json` `abi_floor` | 官方 manylinux_2_28 构建；与 QGIS 路线正交 |
| Calligra | 跟随本机构建的 Qt/KF6 ABI | `vendor/fetch-calligra.sh` | 独立页面渲染进程，不进主程序链接面 |

Debian 13（glibc 2.41）可走源码路线。低于 2.41 的宿主需要独立验证的底座锁。

### deb 闭包锁的信任根（PW-053）

- **下载期**：`vendor/fetch-deps.sh` 对锁里每个 .deb 校验 size + SHA256，不符即删文件并失败（fail closed）；pool 与 snapshot 回退两条路都走同一校验。
- **锁生成期**：锁由维护者主机 `apt-get --print-uris` 生成，来源可信度 = 该主机 apt 对 Ubuntu / qgis.org 仓库 `InRelease` 的 GPG 校验。脚本本身不再做签名校验，因此**只在干净、apt 源配置为官方签名仓库的主机/容器（ubuntu-26.04）上刷新锁**，并在 PR 中审阅锁 diff。
- qgis.org 第三方源没有 snapshot 回退，上游删包会导致锁腐烂；由 `deb-lock.yml` 每周探测发现。

## 受控依赖前缀

配置读取 `QGIS_PREFIX`。未指定时，读取 `QGIS_PREFIX_PATH`，再选择仓库中已准备的前缀。
`PALEO_DEPENDENCY_PREFIX` 若显式设置，须解析到同一前缀。CMake 核对供货锁戳，
并验证 Qt 配置、工具链提供的 Qt 工具、QGIS/GDAL/GEOS 库与头、QScintilla、zlib 和 Zstd 的位置。

| 路线 | CMake `QGIS_PREFIX` | 准备入口 |
|---|---|---|
| Linux 源码路线 | `<repo>/vendor/superbuild/prefix` | [源码依赖闭包](vendor/superbuild/README.md) |
| Linux 二进制加速档 | `<repo>/vendor/prefix/usr` | `./paleo-dev bootstrap fetch-only` |
| Windows | `<repo>/vendor/osgeo4w` | `./paleo-dev.ps1 bootstrap fetch-only` |

开发构建将实际 GIS 资源前缀写入 `PALEO_QGIS_PREFIX_DEFAULT`，测试继承同一资源目录。
`paleo-dev` 和 `paleo-dev.ps1` 接线 Qt、GDAL 和 PROJ 运行路径。
发布包通过搬移后的相对路径运行，见「便携发行树」。

## 独立 worktree 开发

从明确的基线建分支；`master` 与 `origin/master` 不一致时先确认要用哪一个。
主 checkout 的未提交改动不会复制到 worktree。通常使用 `codex/<主题>`，
任务指定的 `refactor/`、`goal/` 等分支名优先。每个 worktree 使用自己的 `build/`。

gitignored 依赖不会随 worktree 出现。本机共享已准备的依赖时，以主仓绝对路径
建立 symlink（以下主仓路径按实际位置替换；不要在共享 prefix 中清理或重建）。
`vendor/ranuts-document`（Office 编辑组件，AGPL-3.0）也在此列——缺它则
应用内 Office 预览报「组件未安装」（configure 会警告；运行时见
`<build>/office-editor` 接线）：

```bash
git worktree add ../paleo_workstation-refactor -b refactor/dedup-docs-tests master
cd ../paleo_workstation-refactor
ln -s /home/kevin/projects/paleo_workstation/vendor/superbuild/prefix vendor/superbuild/prefix
ln -s /home/kevin/projects/paleo_workstation/vendor/onnxruntime vendor/onnxruntime
if [ -d /home/kevin/projects/paleo_workstation/vendor/ranuts-document ]; then
  ln -s /home/kevin/projects/paleo_workstation/vendor/ranuts-document vendor/ranuts-document
fi
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
`ctest -R first_configure`（UNIX）用当前受控前缀验证初次配置，也覆盖
悬空路径、缺供货收据和幂等重试。QScintilla 缺头 warning 在不包含 QGIS 代码编辑器头的 Linux 构建中
允许降级；Windows 使用已有 `CMAKE_PREFIX_PATH` 前缀或显式
`-DQSCINTILLA_INCLUDE_DIR=<安装目录>/include` 提供头，详见任务框架历史账本。

## Linux 同源 Qt 和 GIS 闭包

在 glibc 2.43 或更高的 x86_64 Linux 上，`./paleo-dev bootstrap fetch-only` 按
`vendor/deb-closure.lock` 校验并解包到 `vendor/prefix`。联网准备一次后，保留
`vendor/cache/debs` 和 `vendor/cache/onnxruntime`，可在干净工作树离线重放。
全量 CTest 使用带 `venv` 和 `ensurepip` 的宿主 Python（Ubuntu 的 `python3-venv` 构建测试工具包）。

```bash
PALEO_VENDOR_OFFLINE=1 ./paleo-dev bootstrap fetch-only
./paleo-dev configure -DCMAKE_BUILD_TYPE=Release
./paleo-dev build
./paleo-dev test -j 4 -LE perf
./paleo-dev selfcheck
```

`paleo-dev` 为 Qt 工具、平台和 SQLite 插件、字体配置、WebEngine 子进程及资源、QGIS
provider、GRASS、GDAL 和 PROJ 设定同一前缀。QGIS 桌面插件存放在
`lib/qgis/desktop-plugins`，避免嵌入式 provider 扫描初始化桌面程序。
CMake 会检查锁戳、关键资源与实际 imported
target 的路径。configure 缓存指向其他前缀时，另用新构建目录。可用
`BUILD_DIR=build-asan` 配置和运行独立的 sanitizer 构建。

## Windows OSGeo4W 离线供货

在可联网的机器上运行 `./paleo-dev.ps1 bootstrap fetch-only`，准备 Windows x64 缓存并得到一份已校验的安装树。复制 `vendor/cache/osgeo4w-offline` 和 `vendor/cache/onnxruntime` 到离线工作树。构建还需要按独立 worktree 的约定提供已经准备好的 Office 编辑组件 `vendor/ranuts-document`。使用 PowerShell 重放：

```powershell
$env:PALEO_VENDOR_OFFLINE = '1'
./paleo-dev.ps1 bootstrap fetch-only
./paleo-dev.ps1 build
./paleo-dev.ps1 selfcheck
$env:PALEO_CTEST_ARGS = '-j1 -LE perf --no-tests=error'
./paleo-dev.ps1 test
```

`vendor/osgeo4w-offline.lock.json` 固定安装器、离线索引及每个归档的字节数和 SHA-256。bootstrap 先校验镜像，再以本地安装模式运行安装器，最后逐包校对 `installed.db`（包含 OSGeo4W 的打包修订号）。缺包或坏包会指出文件，原样保留以供诊断。用同一份锁从可信缓存补齐它，再重新运行。已存在的旧安装树或锁变更后的安装树应移到其他目录，由 bootstrap 在目标路径重装。

有意升级时，准备 OSGeo4W 官方 `setup.ini` 中当前范围选中的归档与安装器。先用 `python -B tools/pin_osgeo4w.py --setup-ini <完整索引>` 更新 manifest，再用 `python -B tools/osgeo4w_offline.py generate --cache <镜像根> --installer <安装器> --index <新索引> --lock <新锁>` 生成新文件。镜像根下应有 `x86_64/setup.ini` 及 `x86_64/release`；生成命令写入新文件，避免覆盖当前锁。审查锁差异及上游来源、复跑本地离线验收后替换仓库中的索引与锁。保存按锁验证的缓存，供滚动上游下架历史包后重放。

## 独立 SDK 与原生性能测试

原生前缀复用工作站的归档锁，包含 Qt、压缩、SQLite 和 FreeXL 所需的文件。
Linux 通过包名清单从完整 deb 锁取子集，Windows 从同一 OSGeo4W 离线索引
计算 Qt 软件包闭包。Windows 的 Zstd 开发库由固定源码构建，Qt 所用的
OSGeo4W Zstd DLL 原样保留。两个入口都支持 `PALEO_VENDOR_OFFLINE=1`。

Linux 从工作站仓库根执行以下命令。

```bash
bash vendor/fetch-deps.sh --native
source vendor/activate-deb-native.sh
cmake -S tools/native-perf -B build-native-perf -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DPALEO_DEPENDENCY_PREFIX="$PALEO_NATIVE_PREFIX" \
  '-DPALEO_NATIVE_PERF_TESTS=geostat;welllogset'
cmake --build build-native-perf --parallel 8
QT_QPA_PLATFORM=offscreen ctest --test-dir build-native-perf -j1 --output-on-failure
```

Windows 从 MSVC x64 开发终端准备并激活这一前缀。

```powershell
./paleo-dev.ps1 bootstrap-native
$prefix = (Resolve-Path vendor/osgeo4w-native).Path
$env:PATH = "$prefix/bin;$prefix/apps/qt6/bin;$env:PATH"
$env:QT_PLUGIN_PATH = "$prefix/apps/qt6/plugins"
$env:QT_QPA_PLATFORM = "offscreen"
cmake -S tools/native-perf -B build-native-perf -G Ninja "-DPALEO_DEPENDENCY_PREFIX=$prefix" -DCMAKE_BUILD_TYPE=Release '-DPALEO_NATIVE_PERF_TESTS=geostat;welllogset'
cmake --build build-native-perf --parallel 8
ctest --test-dir build-native-perf -j1 --output-on-failure
```

独立 SDK 使用相同的环境及 `-DPALEO_DEPENDENCY_PREFIX`，CMake 源码目录
选择 `vendor/Data_service` 或 SDK 自己的 checkout。

工作站的 `-DPALEO_ONNXRUNTIME=ON` 要求仓库内的固定版本运行时准备完整，
`OFF` 构建不含本地推理的配置。默认 `AUTO` 使用 `vendor/onnxruntime`
中已准备的版本，目录缺席时关闭本地推理。

## Windows 本机开发

```powershell
./paleo-dev.ps1 bootstrap fetch-only
$env:PALEO_CMAKE_BUILD_TYPE = 'Release'
./paleo-dev.ps1 build
./paleo-dev.ps1 test
./paleo-dev.ps1 selfcheck
./paleo-dev.ps1 checkenv
```

本机和发行包共用 OSGeo4W 供货锁。独立 worktree 可共享经过校验的前缀和下载缓存。
`PALEO_PYTHON` 可指向用于本地测试的宿主解释器。测试串行执行，`PALEO_CTEST_ARGS`
可限定本地回归范围。`diagdeps <测试目标>` 输出 MSVC 导入表和实际搜索路径。
`ensure-resources` 核对 GIS 数据库，缺件时按已锁版本补齐。

## 平台 × 版本矩阵

| 平台 | 状态 | 依赖来源 |
|------|------|----------|
| Linux x86_64：deb 闭包需 glibc≥2.43（Ubuntu 26.04 / 同代 Arch）；Debian 13（glibc 2.41）不能运行该闭包，须走 superbuild | 使用 Ubuntu 26.04 做本地构建、回归及运行时依赖审计 | superbuild 自编译 prefix（首选）→ deb 闭包 `vendor/prefix/usr`（加速档，CI 现行） |
| Windows x86_64 | 本地 MSVC Release，串行 CTest | OSGeo4W 离线归档锁及源码 OpenCL、Zstd 补件 |
| Debian 13 x86_64，glibc 2.41 | 在目标 ABI 上从锁定底座执行源码构建与回归 | Debian 13 Qt 底座和源码 GIS 闭包 |

## 依赖（vendor manifest pin）

QGIS 4.2.x（三路口径统一上游 4.2.3，`tools/check_qgis_versions.py` 门禁看守）· Qt ≥6.6 · GDAL · PROJ · GEOS · QCA-qt6 · QtKeychain-qt6 · libspatialindex · exiv2 · libzip · OpenSSL · sqlite3/spatialite · **ONNX Runtime 1.30.0**（官方 release，sha256 `a5ed5a3c…3b3fd`，manylinux_2_28）。Ubuntu 26.04 的 deb 完整闭包和 SHA-256 在 `vendor/deb-closure.lock`（库解包在 `vendor/prefix/usr/lib/<multiarch>`，`./paleo-dev` 已加入 `LD_LIBRARY_PATH`）；Ubuntu pool 会删除被安全更新取代的旧版本，因此 `fetch-deps.sh` 在 pool 404 时回退到 `https://snapshot.ubuntu.com/ubuntu/<ts>/`，时间戳记录在 `vendor/deb-closure.snapshot`（`--update-lock` 会刷新；可用 `PALEO_DEB_SNAPSHOT` 覆盖），内容仍由锁内 SHA-256 校验；OSGeo4W 安装器摘要在 `vendor/manifest.json`。

## 便携发行树

Windows 使用 MSVC Release 构建，然后从同一 OSGeo4W 供货前缀组装。QGIS 的
OpenCL 加载器由 bootstrap 从锁定的 Khronos 源码构建。PowerShell 打包入口调用
同一个 Python 打包器，运行时可直接打开包内的 `paleo.exe`。

```powershell
./tools/package-portable.ps1 -OutDir ./build/paleo-portable
python tools/package_portable.py --verify-only ./build/paleo-portable
```

Linux 打包器先执行 CMake 安装，再收集来自构建前缀的 ELF 闭包、Qt/GIS 插件和
数据、证书、字体及 Office 组件。需要宿主 `patchelf`、`dpkg-deb` 和 Python 3.11+。
首次缓存证书归档后，可用 `PALEO_VENDOR_OFFLINE=1` 复跑。

```bash
python3 tools/package_runtime_linux.py --build-dir build --out build/paleo-portable
build/paleo-portable/paleo-portable selfcheck
python3 tools/package_runtime_linux.py --verify-only build/paleo-portable
```

两端包内的 `RUNTIME_SOURCES.json` 记录文件来源、大小和 SHA-256，
`share/paleo/supply` 和 `share/paleo/licenses` 保留供货锁及许可。
搬移到新路径后用 `--verify-only` 复验完整性、GIS 数据库和坐标变换。
受控 Python 和 Pi 使用各自的可选部署入口。

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

模型快照使用随构建生成的 `paleo_data_snapshot_worker`（Windows 带 `.exe`），
只依赖当前部署的 Qt Core/Sql；应用安装及两套 Windows 便携打包脚本均包含它。
嵌入 Data_service 时即使关闭 `PDS_BUILD_TOOLS` 也须构建此运行组件。组件位于
主程序同目录，或其 `../libexec/`，不从系统 PATH 查找可执行文件；缺失时快照明确失败。
父进程通过取消或超时终止私有快照任务，成功后才发布目标文件。Office 编辑组件仍须
按上文单独接线。组件部署、取消、WAL 快照和旧库不变的回归见 `tst_dataservice_snapshot`。

| 现象 | 原因 | 修复 |
|------|------|------|
| `QDomDocument: No such file` | 项目 Qt 模块或前缀接线缺失 | 用锁重放项目依赖后重新配置 |
| `defaulted operator==` 报错 | C++ 标准 <20 | CMAKE_CXX_STANDARD=20（已钉） |
| `providerList() empty` | prefix 未指向 vendor/qgis 资源根 | selfcheck 检查 setPrefixPath + QGIS_PREFIX 环境 |
| srs.db missing | vendor prefix 缺 share/qgis/resources | 重跑 `paleo-dev bootstrap` |
| `GDALPolygonize` 无 geotransform 警告 | 测试栅格无仿射 | GDAL 3.11+ 需 SRC_METHOD=NO_GEOTRANSFORM（已知，警告非阻塞） |

## 升级流程

Debian 闭包升级时，在目标发行版且已配置 QGIS 官方 apt 源的干净环境运行 `./vendor/fetch-deps.sh --update-lock --print-only`，审查并提交新的 `vendor/deb-closure.lock`。正常 bootstrap 只读取锁文件，并仅解包锁内经过大小和 SHA-256 校验的文件。superbuild 升级则同步改 `vendor/superbuild/CMakeLists.txt` 的 `QGIS_URL` 与 `URL_HASH`（补丁文件名随版本重命名，需对目标版本 tarball 重验证 dry-run 干净），`vendor/cache/tarballs/` 缓存与哈希一并更新。**三路版本口径（superbuild 精确 / deb 闭包上游 / manifest `qgis_family` 家族）必须一起动**——`tools/check_qgis_versions.py`（ctest `qgis_versions` + CI lint）不一致即红。ONNX Runtime 升级则更新 `vendor/manifest.json` 的版本和摘要。Windows 同时刷新 `vendor/manifest.json`、离线索引与归档锁，步骤见「Windows OSGeo4W 离线供货」。

升级后在新的依赖目录准备前缀，保留旧目录供尚未切换的构建使用。归档缓存可共享。
原位解包前，准备脚本会逐个核对锁定大小和 SHA-256；缺失或损坏的文件需按当前锁
补齐。用 `PALEO_VENDOR_OFFLINE=1` 验证缓存重放，重新配置主项目并运行本地
编译、测试、源码门禁和发行树自检。取证时记录提交、锁摘要、宿主 ABI，以及
构建树和搬移后发行树的依赖来源。

## 性能度量与 CI 产物

Phase 0 和早期增量构建的绝对耗时保留在 `docs/perf/` 与交付账本中，不能代表当前规模。
当前增量构建使用 `tools/measure_incremental.sh`，记录实际重编 TU、构建步骤及比率：

```bash
tools/measure_incremental.sh build src/app/appcontext.cpp build/incremental-baseline.jsonl
```

CI Linux 在 Selfcheck 后执行该测量，并上传 `incremental-baseline-linux` 产物。
它是非阻断的趋势观察项；比较时保留提交、工具链和构建配置，绝对时长不作跨机判定。

### CI 调试信息体积

Windows 必过车道通过 `PALEO_CMAKE_BUILD_TYPE=Release` 构建全部目标，
省去全量测试链接的 PDB 和增量链接文件。各分片仍执行原有的串行测试范围，
本机 `paleo-dev.ps1 build` 默认仍使用 RelWithDebInfo。

Linux 必过车道使用 RelWithDebInfo 的 `-O2 -g1 -DNDEBUG`：保留符号和源码行位置，
减少全量测试可执行文件的 DWARF 磁盘占用，防止链接末期耗尽 runner 磁盘。
只在该 CI job 预置 CMake 缓存，随后仍走 `paleo-dev build` 并运行全部目标及测试；
本机默认 RelWithDebInfo 和 sanitizer 车道不受影响。
## 可选 Agent Python 环境

离线安装、宿主持久配置与可选 Pi 接线见本节。设置页通过同一服务核验、探针和导出脱敏诊断；
已安装、内容完整、已启用与当前可执行分别报告。各批范围和验收见
[实施计划](docs/planning/PYTHON_DELIVERY_AND_PI_AGENT_PLAN.md)。

主程序的默认构建、安装和启动不需要该运行时。开发和源码检查依赖的 Python
工具链仍按原有要求准备。`analysis-core` 单独锁定 CPython 3.14.10 与 NumPy 2.5.4，
只支持已列明的 Windows/Linux x86-64；不含 PyQGIS、PyQt 或运行期 pip 安装入口。
离线准备、摘要锚定、许可、系统 ABI 闭包及升级保留约束见
[供货说明](vendor/agent-python/README.md)。执行前必须由操作者准备可选环境；
Agent 可查询状态及运行已登记的可信分析，不能调用准备工具或决定解释器路径。
已准备 generation 可在供货端封装为[离线交付流](vendor/agent-python/DELIVERY.md)，客户端
使用 C++ 部署工具安装，无需系统 Python、pip 或解包程序：

```text
cmake --build <build> --target paleo_python_environment -j3
<build>/paleo_python_environment --package <offline-package> --sha256 <independent-package-SHA256> --store <absolute-dedicated-store>
```

该工具不在 Agent 目录中，也不随默认构建/安装交付；确需部署时显式构建，按
`AgentPythonDeployment` 安装组件安装其原生可执行文件并配套现有 Qt Core 闭包。
安装只更新完整 generation 的安装选择，不启用可信/生成执行；后续仍按宿主配置准入。
当前交付包仅用于已授权的内部准备与验证，外部分发入口关闭，许可状态保留原始说明。

同一原生部署工具的 `--activate` 在完整核验后原子保存运行时、模板、权限与兼容身份；
`--disable`、`--revoke` 和 `--rollback` 复用同一宿主记录与修订号 CAS。生成专用配置
不要求安装可信模板；回滚不恢复更宽的旧权限。App 从稳定 registry 解析新请求和已授权
历史操作，普通版本切换保留在途 provider，不重绑文档。命令、来源优先级、紧急撤权与
持久化失败语义见[宿主配置](vendor/agent-python/CONFIGURATION.md)。激活只验证内容，
实际可信/强隔离后端执行准入仍需独立探针。

未创建宿主持久记录时，兼容环境变量整组回退；可信分析需设置 `PALEO_AGENT_PYTHON_ENABLE_TRUSTED=1` 和经宿主准备的
`PALEO_AGENT_PYTHON_SCRIPTS_ROOT`；脚本包见[说明](vendor/agent-python/scripts.README.md)。
运行时 ready、模板 ready 和实际执行后端 ready 分别报告。当前 Linux 执行要求钉版
辅助程序、用户 manager 与 CPU/memory/pids 委派；WSL 缺控制器时明确拒绝。
输入通过独立能力选择曲线、显式数值表或精确地震道的有界窗口；调用者不能传入路径、
SQL、运行时根或推断的类型／单位。原 REPORT、表格输入 REPORT、地震道 REPORT、
REPORT+CSV 和 REPORT+CSV+PNG 保持各自 ID 与恢复契约，旧操作不会被静默升级。
数值表最多 16 列（含轴）、8192 行，单道最多 8192 样点；REPORT ≤256 KiB，CSV／PNG
各 ≤4 MiB，RGBA8 宽高各 ≤1024，并同时遵守获准 policy 的更小上限。具体选择和协议见
[表格输入](docs/PYTHON_NUMERIC_TABLE_INPUT.md)、[地震道输入](docs/PYTHON_SEISMIC_TRACE_INPUT.md)
和[联合成果](docs/PYTHON_GENERATED_OUTPUTS.md)。PNG 是非空间报告工件，生成值与像素未由宿主独立重算。

prepare 冻结输入和声明，compute-only 只产生私有认证成果；正式成果由独立 publish 使用
原操作引用成组登记。发布、幂等重试和重启恢复沿用原 Host/operation/outputset 链路，
不重跑原程序；任一端口验收失败则整组拒绝。

在无宿主记录的兼容环境变量回退路径，生成执行另需
`PALEO_AGENT_PYTHON_ENABLE_GENERATED=1`，不因此启用 trusted。有宿主记录时按已激活的
独立权限准入，无需额外设为 `1`；兼容变量显式设为 `0` 仍作为拒绝上限。
生成路径只做运行时内容身份核验，版本与 NumPy 运算在实际平台强隔离内验证；
缺任何后端/资源前提时不开放工具，也不退回普通进程。版本/部署、零子进程磁盘写入、
Windows 线程与 CPU 限制的精确区别见[平台说明](vendor/agent-python/SANDBOX_SUPPORT.md)。
已有人工控制台/REPL/MAMCL 保持独立，Shell、GPU、安装包与网络能力仍后置。

### 可选 Pi 核心供货与编排（A1/A2）

默认 configure、应用启动和安装都不发现或运行 Node。供货端显式执行
`tools/prepare_pi_agent.py --cache <dedicated-cache> --fetch` 抓取源码锁的闭包，
再用 `--offline --cache <cache> --store <dedicated-store> --platform <platform>`
离线准备 generation；不使用系统 Node、npm 或包生命周期脚本。支持的固定发行包与
源锁见 [Pi 清单](vendor/pi/README.md)。准备结果仍为禁用状态；A1 固定握手与
A2 对话入口使用独立协议，运行时必须匹配所选入口的完整供货身份。

`PALEO_PI_INSTALL_ROOT=<prepared-generation>` 和 `PALEO_PI_INSTALL_SHA256=<host-SHA>`
仅为显式 `cmake --install <build> --component AgentPiRuntime` 提供输入；组件从默认
安装排除，安装时完整校验与暂存发布，不启动 Node、不授执行权限。无 Node 的正常构建
仍运行 `tst_piprotocol` / `tst_piruntime`。真实固定进程验收显式开启
`PALEO_TEST_PI_NATIVE` 并提供 `PALEO_TEST_PI_NATIVE_ROOT` / `PALEO_TEST_PI_NATIVE_SHA256`，
缺夹具直接失败。`tools/probe_pi_agent.py` 的独立 Node 协议证据不替代 C++ 进程验收。

应用默认使用既有 C++ 编排。可信宿主同时设置 `PALEO_AGENT_PI_ENABLE=1`、
`PALEO_AGENT_PI_ROOT=<prepared-generation>` 和
`PALEO_AGENT_PI_MANIFEST_SHA256=<independently-approved-manifest-SHA>` 后，
聊天才选择可选 Pi 入口。安装组件的变量不授予此权限；项目与模型参数也不能选择运行时。
显式选择的 Pi 不可用时返回失败，不切换引擎重放该轮。模型设置、密钥与网络请求仍由
C++ `LlmClient` 持有，工具通过共享 Runner/Host 准入；Node 是可信编排组件，不是隔离沙箱。
协议、预算、取消及跨引擎重试语义见 [A2 对话契约](vendor/pi/CONVERSATION.md)。

