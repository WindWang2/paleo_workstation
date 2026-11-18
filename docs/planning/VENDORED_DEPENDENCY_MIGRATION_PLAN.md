# 项目依赖迁移实施计划

基线：master `9b56050f`；#1039 的 CI 修复与 #1040 的仓库政策是实施前置。
计划阶段只盘点、定顺序和验收，不改变运行时或工具入口。Agent Shell 继续后置。

## 目标与边界

QGIS、Qt 在内的项目第三方库及其传递依赖统一由仓库管理。优先钉版本和哈希自编译；
若上游构建代价过高，使用经过完整性校验的二进制闭包。下载缓存和依赖产物可放在
gitignored 前缀；提交锁、来源、构建参数和适用的许可证材料。按平台和 ABI 分开锁定；
同一平台用匹配的一套头文件、工具、链接库、运行库、插件和资源。
供应端可以使用发行版包，但使用端从项目目录解包，不把项目库安装到系统目录。

宿主提供编译器、SDK、构建及校验工具和不可搬迁的 OS 接口。Linux 只允许经实际验证、
逐名记录的基础 ABI（如动态加载器、glibc）；Windows 只允许平台提供的 API set 和
系统 DLL。C++/编译器运行库先检查能否随包供应，确需依赖目标 ABI 的才逐项列明，
不按 `/usr/lib`、`System32` 或文件名通配整体豁免。X11/XCB、GL/EGL 用户态库、
OpenSSL、ICU 等先归入项目闭包；显示服务和 GPU 驱动是宿主接口，另列出可移植边界。
主程序、Data_service SDK、测试和发行包都遵循同一规则。现有受控 Python、Pi、Office
产物保持原锁；宿主 Python 用于构建和源码门禁，不能作为交付的 Agent 运行时。

## 现状盘点

区分「已观测的系统依赖」「代码仍允许回退」「尚未通过运行时审计」。静态引用不等同于
每种构建实际从系统加载；V0 会给每条路线建立可复核报告。

**本机 Linux 抽样**：用此前通过回归的 Arch/WSL `tst_aiassistdock`，在该构建
使用的 Qt WebEngine/PDF、GIS 和 ORT 前缀下执行 `ldd`，仍解析到宿主
`/usr/lib` 的 Qt Core/Gui/Widgets/Network/Sql 等、GEOS、PROJ、SQLite、
zlib、zstd、QCA、ICU 和 GL/XCB，并带入 GLib、NSS/NSPR、图像和多媒体库。
这是本机混合路线的实测样本，不能外推为 Ubuntu CI 或 Windows 的加载清单。

| 范围 | 现状与依据 | 处理批次 |
|---|---|---|
| Linux 主 CI：Qt 和 QtKeychain | `.github/workflows/ci.yml` 的 lint、linux、linux-perf、linux-asan 调用 apt 装 Qt Base、5Compat、Svg、SerialPort、Positioning、Tools、WebEngine、PDF 和 QtKeychain。deb 锁已有大部分 Qt、QScintilla、QPA 和 QSQLITE 归档；未找到 `qtkeychain-qt6-dev`。第一个任务是查明配置和加载到底取了哪一份，而非重复供应。 | V1 |
| QGIS、GDAL、PROJ、GEOS 与资源 | Linux CI 已下载归档到 `vendor/prefix/usr`；Windows 走 OSGeo4W 私有前缀。 `vendor/bootstrap.sh` 仍会因系统 QGIS 跳过准备，`cmake/paleo-dependencies.cmake` 有系统搜索路径，显式前缀主要检查 QGIS 头；`src/qgis/qgisruntime.h` 还有 `/usr` 运行兜底。GDAL 驱动与数据、PROJ 数据库、QGIS providers 和 `srs.db` 要随路线审计。 | V1、V2、V5 |
| Windows OSGeo4W、Qt 配套库 | `vendor/manifest.json` 校验安装器摘要、包版本和传递包集合；安装仍依赖上游实时索引，缓存未命中会重解和下载。#1039 的一次运行有三个分片进入构建，另一个安装器重试后仍失败。具体安装故障不能从现有日志判定。 本机 `localdeps` 组合 conda、QGIS、独立 QtPdf 目录，仍缺从空机器可复建的统一锁。 | V2 |
| 压缩、数据库、加密及插件 | `cmake/extra-sf-kriging.cmake`、`extra-io-perf.cmake`、`paleo-dependencies.cmake` 发现 zlib/zstd；Data_service 另查 Qt/ZLIB/zstd，QGIS 头透出 SQLite，Qt 还加载 SQL/TLS、QCA/Keychain、图像及平台插件。锁内已有部分归档，需验证命中路径与 ABI。 | V1、V2、V3 |
| QGIS superbuild | `vendor/superbuild/CMakeLists.txt` 已钉 GEOS → PROJ → GDAL → QGIS 源码，Qt 来自外部默认发现，PROJ 没有完整受控前缀，GDAL 和 QGIS 的配置仅靠优先搜索前缀。周检通过 apt 装 SQLite、TIFF、curl/OpenSSL、Expat、libzip、Protobuf、nlohmann-json、zlib/zstd、Exiv2、GeographicLib、SpatiaLite、Draco、GSL、libtasn1、libpq、x11/xkb/GL 及 Qt 配套；这些是候选依赖，按最终链接与功能取舍确认。 | V4 |
| 原生性能与 SDK 独立构建 | `.github/workflows/native-perf.yml` 装系统 Qt、zlib、zstd；`tools/native-perf/CMakeLists.txt` 复用生产源码但有独立发现流程。Data_service 独立构建也有自己的依赖发现。Windows 修补的 FreeXL 是仓库来源，还要核对 iconv/charset 及安装布局。 | V3 |
| 发布链 / 已受控组件 | `tools/package_portable.py` 和 `tools/package-portable.ps1` 已收集 Windows DLL 并检查部分运行时补件；还需证明 DLL 来源和 Linux 安装树闭包。ORT 是单独锁定供货（#1039 切换同版本 NuGet），但 CMake `PATHS` 仍允许默认搜索。Agent Python、Pi、Office 保留现有供货和自检。独立 Calligra 开发脚本的 Qt/KF6 发现单列，不把它算进主程序发布闭包。 | V0、V3、V5 |

## 批次与依赖

推荐顺序：V0 → V2 → V1 → V3 → V4 → V5 → V6。V1 和 V2 可在 V0 后分别开发，
先处理已暴露冷启动不稳定的 Windows V2。V3 依赖两端可复用前缀；V4 不必等
V3 开始取证，但切换验证复用 V3 的依赖目标。每批独立 PR，合入后再以最新 master
重基下一批；如涉及 Data_service 独立入口，先送 SDK PR，再由主仓更新 gitlink。

| 批次 | 范围和交付 | 依赖 | 验收标准 |
|---|---|---|---|
| V0 证据与边界 | 为 Linux/Windows 的主构建、SDK、native-perf、superbuild 和发行树输出依赖来源清单。复用 CMake cache/imported target、编译 include、link map、PE/ELF 静态闭包、进程实际加载模块及插件日志。按 `项目前缀 / 工具链 / 宿主基础 ABI / 未解析或系统泄漏` 分类；先收集，不放宽红项。 | #1039、#1040 合并 | 从干净构建目录生成可比较的报告，记录提交、锁摘要、OS/编译器、真实路径、库版本、调用者及 stage。用受控样例验证误装同名库和缺失传递库能被识别。 |
| V2 Windows 固定供货 | 固定 OSGeo4W 索引和所有选中归档的 URL、大小、SHA-256，按锁生成本地仓库，以离线方式执行经过验证的安装脚本或解包接线；保持现有 provider、Qt 和 ORT 版本。缓存键按锁与平台，不以缓存作为信任根；四分片共享同一份锁定义。将有意支持的 `localdeps` 转成可校验的锁定前缀。 | V0 | 空缓存从可信来源准备一次后，断公网重放得同一包清单；缺、坏档和版本漂移早失败。在本地 Windows 或干净 VM 复跑四分片覆盖的测试范围，验证 selfcheck、QPA/QSQLITE/Office、QGIS provider 与 CRS；不依赖系统 Qt/QGIS。 |
| V1 Linux 同源 Qt/GIS | 复用 deb 锁已有 Qt/GIS 归档，补 QtKeychain 开发件等缺项；锁定 CMake 配置、moc/lrelease、WebEngineProcess、QPA/QSQLITE 等，给构建入口显式受控前缀。清理工作流里安装应用库的步骤；关闭 bootstrap 检测系统 QGIS 而跳过供货的路径。 | V0 | 本地干净 Linux 环境不预装 apt 应用库，冷缓存完成原有编译、回归、性能及 sanitizer 步骤；检查 Qt 全套头、工具和运行插件以及 QGIS/GDAL/PROJ/GEOS 来自受控前缀。模拟缺 Qt 模块、缺 provider、损坏归档均早失败。 |
| V3 公共小库、SDK 和 native-perf | 收敛 zlib、zstd、SQLite、FreeXL/iconv 及实际用到的加密/数据配套。复用同一前缀入口并固定 imported targets，给 SDK 独立构建和 native-perf 提供无需 QGIS 安装的最小闭包。ORT 按锁启用或明确关闭，移除系统隐式发现。 | V1、V2；如需 SDK PR，先合 SDK 再提交主仓 gitlink | 两端最小配置、SDK 独立测试及 native-perf 数据/地统分支通过；ORT 开/关、压缩缓存、SQL 和 FreeXL 夹具通过。路径和符号来源可追溯；替换 PATH/系统同名库不得改变选中版本。 |
| V4 源码路线闭包 | 按底座库 → Qt/配套 → GEOS/PROJ/GDAL → QGIS 顺序扩充 superbuild。小库优先源码，Qt WebEngine 等重件先用匹配的已锁二进制；用明确配置禁用未使用的可选依赖，仅当功能回归验证通过。保留现有 QGIS 补丁和 ABI 口径；Windows 以经过验证的二进制闭包为先，源码支持另按实测扩展。 | V0、V1、V3 | 在本地最低目标 ABI 的干净 Linux 环境完成源码冷构建；各子工程 CMake cache 和运行闭包无未批准系统库。比较 QGIS 版本、补丁、provider/驱动、坐标变换和主项目功能；记录构建时间、峰值磁盘及产物大小。 |
| V5 安装与发布树 | 修正 Linux RUNPATH 与资源、插件布局，收敛 Windows 两套打包入口的依赖来源规则。检查主程序、子进程、Qt/QGIS/GDAL 动态插件、数据库、证书与 Office 静态资源；生成来源、许可证、哈希清单。 | V1、V2、V3、V4 | 从安装树而非源码运行。移到不同绝对路径，用无开发前缀、纯净环境、断网 VM 运行 selfcheck 和代表性 GUI/数据/Agent 任务；无项目 DLL/so 越界、无缺插件和资源。破坏一项库或资源时校验明确失败。 |
| V6 强制与收口 | 删除默认系统搜索、运行回退和旧兜底文案，统一审计到 CI；将供货锁刷新、缓存失效、离线恢复、取证写入构建文档。Calligra 只在其保留范围内独立验收；用户启用的外部解释器不写入主程序依赖。 | V5 | 无系统项目库的构建/安装/运行端到端通过；从磁盘放入同名系统依赖的反向用例仍拒绝。受控前缀、构建树和发行树可定位；零未说明系统项目库、零依赖基线放宽。 |

每批按相同口径验收：先编译（并行不超过 8，Windows 测试串行），跑相关回归及
受影响平台的完整非性能套件，涉及包供应时补冷缓存和离线验证。提 PR 前
`git fetch`、`git rebase origin/master` 后重跑 Source gates，包含带真实 QGIS
头路径的 transitive 门禁；PR 附实际编译、测试、锁差异和来源审计证据。性能与 ASan
单列本地结果，不能用缓存命中或存在性检查代替实际验证。按 `AGENTS.md` 以本地
结果作为验收依据；不查询或等待托管 CI。没有改变供应链的其他功能（包括 Agent
Shell）不纳入本计划。
