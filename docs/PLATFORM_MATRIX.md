# 平台与依赖来源

2026-10-27 按仓库配置整理。支持目标是 Linux x86_64 与 Windows x86_64；
macOS / arm64 尚不在支持矩阵内。这里描述构建路线，实际验证结果以对应提交的 CI
或带日期、工具链信息的本机报告为准。

| 平台 / 路线 | 依赖来源 | 当前配置 |
|---|---|---|
| Linux x86_64，源码构建 | `vendor/superbuild/prefix` | 首选路线。在目标最低系统构建并验证依赖和应用 |
| Linux x86_64，二进制加速 | `vendor/deb-closure.lock` 与 `vendor/prefix/usr` | Ubuntu 26.04 CI 使用此路线；当前闭包要求 glibc ≥ 2.43 |
| Linux x86_64，系统包兜底 | 同一发行版的 QGIS/Qt 开发包 | 仅开发兜底；不能代替 CI/发布的 vendored 路线 |
| Windows x86_64，CI | OSGeo4W 钉版包闭包 | `windows-2025`、MSVC `/MD`；四个测试分片，每片内串行 |
| Windows x86_64，本机 localdeps | 本机 QGIS 前缀与同链 Qt/GDAL/PROJ，按需补 qtpdf | `paleo-dev.ps1` 配置和检查；当前本机路线为 Qt 6.11.2 |
| macOS / arm64 | 未建立路线 | 未支持 |

Debian 13 的 glibc 2.41 不能加载当前 Ubuntu 闭包；需在目标系统使用源码构建路线。
具体接线、资源目录、Windows 私有注册表沙箱和环境检查见 [BUILDING.md](../BUILDING.md)。

## 版本与能力

- QGIS 家族和 OSGeo4W 直接包/依赖闭包版本读取 [vendor/manifest.json](../vendor/manifest.json)。
  Linux 闭包读取 `vendor/deb-closure.lock`，源码构建版本读取 `vendor/superbuild/CMakeLists.txt`。
  三路一致性由 `tools/check_qgis_versions.py` 校验，不在本文另存一套版本清单。
- 主程序采用 C++20。Qt/GDAL/PROJ/GEOS 运行库须与所选 QGIS 构建链匹配，
  Windows 更换前缀后先运行 `./paleo-dev.ps1 checkenv`。
- WebEngineWidgets 与 ONNX Runtime 为可选依赖；缺失时按构建配置降级，不能宣称对应功能已验证。
- “标注随图层 z 序”依赖源码构建中的 QGIS 补丁；deb/OSGeo4W 二进制路线使用原生标注行为。
  补丁清单以 [superbuild 说明](../vendor/superbuild/README.md) 为准。

## 验证与新增平台

CI runner、测试标签与分片设置以 [.github/workflows/ci.yml](../.github/workflows/ci.yml) 为准。
本机使用 `paleo-dev` / `paleo-dev.ps1` 的 build、test、selfcheck 入口，构建并行度不超过 8，
Windows 本机测试保持串行。旧报告中的“本机全绿”不能代表当前提交。

新增平台时先明确 QGIS/Qt 来源、ABI、最低运行系统及资源安装方式，再补 CI 或本机验证证据。
取得证据前标记“未验证”，不要复用其他平台的通过结论。
