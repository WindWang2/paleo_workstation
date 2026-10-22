# 便携包坐标数据库绑定与中文安装路径修复（2026-10-22）

## 背景

便携包复制到新电脑（双击「启动paleo.bat」）后新建工程失败，无法识别
EPSG:32649。开发机上不复现，掩盖了两个问题。

## 根因

1. **外来 GIS 环境变量覆盖包内数据**：启动器只设旧变量 `PROJ_LIB`；PROJ ≥ 9.1
   优先读 `PROJ_DATA`。目标机装过其他 GIS 软件时，残留的 `PROJ_DATA` 指向包外，
   包内 `proj.db` 被跳过，EPSG:32649 解析失败。
2. **中文安装路径写坏环境变量**：`qputenv(key, value.toUtf8())` 在 Windows 上经
   CRT 按 ACP（中文系统 GBK）转换——UTF-8 字节被按 GBK 解码成乱码（实测
   `地质软件` → `鍦拌川杞欢`）。消费方分属三个互不兼容的编码域（CRT getenv 按
   ACP、PROJ 按字节当 UTF-8、Qt 按 local8Bit 读回），非 ASCII 路径无论给哪种
   字节序都会被另外的域写坏；实测 PROJ 经环境变量收到中文路径即报
   `Cannot find proj.db`，QGIS 用乱码前缀初始化时直接崩溃。8.3 短路径在中文
   系统上也可能保留 CJK 字符（实测 `地质软~1`），不是可靠出路。

## 修复

- 新增 `src/app/portableenvironment.{h,cpp}`（组装根）：
  - exe 同目录存在 `plugins/platforms/qwindows.dll` 即判定便携布局，强制把
    `QGIS_PREFIX_PATH / PROJ_DATA / PROJ_LIB / GDAL_DATA / QT_PLUGIN_PATH /
    QML2_IMPORT_PATH / QTWEBENGINEPROCESS_PATH` 绑定到包内，压过任何外来
    GIS 变量；`share/proj/proj.db`、`qgis/resources/srs.db` 缺件时报告。
  - 非 ASCII 安装根改用**纯 ASCII 锚点**：优先 8.3 短路径（仅当结果纯
    ASCII），否则在 `C:\ProgramData\PaleoPortable\<sha1-16>` 建目录联接
    （mklink /J，无需管理员；两个路径实参必须反斜杠，正斜杠会被 cmd 当
    开关）。联接按 canonicalPath 复用，目标漂移时重建。
  - GDAL 侧继续 `OSRSetPROJSearchPaths` + `CPLSetConfigOption(GDAL_DATA)`
    以 UTF-8 API 直设，不依赖环境变量编码。
- `paleo.exe --check-runtime [--report <path>]`：初始化 QGIS/GDAL 后实测
  EPSG:32649/4326 解析与 UTM49N→WGS84 正算（QGIS 与 GDAL 各一组），输出
  JSON 报告。诊断子进程跳过 QgsApplication 退出清理——退出码是启动器自检
  闸，退出阶段偶发崩溃会把已通过的检查误报成失败。
- `tools/package-portable.ps1` 入库：输入检查加 proj.db/srs.db；输出目录已
  存在即报错（不再静默删旧包）；robocopy 失败按退出码置为致命；启动器补
    `PROJ_DATA`、启动前跑坐标自检（失败弹报告路径并退出）；打包完成后用包内
  exe 实测坐标链路。启动器内容保持纯 ASCII——cmd 在 chcp 切码后会错位重读
  批处理文件，中文内容会把解析打碎；correlation 目录改为运行时枚举
  `correlation\*`，不再把中文目录名烧进 bat。
- 连井图 Python 子进程补 `PYTHONUTF8=1`：中文 Windows 的 Python 缺省 GBK，
  读 UTF-8 井数据/JSON 直接 UnicodeDecodeError。

## 验证

- 单测 `tst_portableenvironment` 6 项：开发布局不动、外来 GIS 变量被覆盖
  （含中文安装根）、锚点绑定、缺件报告。
- 完整便携包（-SkipPython -SkipCorrelation）实测矩阵，退出码全 0、六项检查
  全绿：ASCII 路径；中文+空格路径（`新电脑 模拟\Paleo便携测试`，经
  ProgramData 锚点）；中文路径 + 外来 hostile 环境（`PROJ_DATA=C:\old-gis\proj`
  等五项，被锚点值覆盖）；ASCII 启动器 bat 在 cmd 双击语境下自检通过；
  同包二次启动复用锚点联接。
- 打包脚本端到端跑通（含第 10 步包内自检）；qgisruntime 5 项、projectsetup
  17 项通过；stratigraphicweb 13 项通过（`embeddedBlobDownloadSmoke` 在本
  会话环境为既有失败，还原改动复跑确认与本修复无关）。
- strict 分层、Qgs audit/spread、symbol、规模、界面不变量、i18n、tokens、
  docs drift 闸门通过，基线无增长。

## 已知边界

- `C:\ProgramData` 不可写或非 ASCII（非常规配置）且 8.3 短路径不可用时，
  退回长路径并在报告中说明（anchorNote）；此时中文路径的便携包仍不可用，
  自检会给出定位信息而不是静默崩溃。
- 用户现有 `paleo-portable` 安装需用新脚本重打包替换。
