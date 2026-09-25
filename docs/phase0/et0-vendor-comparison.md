# ET0 — Vendor 策略对比：binary vendoring vs source superbuild（Phase 0 spike-0）

日期：2026-09-25。输入：§39 spike-0（DX 评审 D7）。目标平台：Linux x86_64 + Windows x86_64；Qt6 Widgets；无 Python。
TTHW 预算：1 命令 + ≤2h 无人值守（§44.1）。

## 版本事实

- QGIS 4.2.0 发布于 2026-07-03（首个 Qt6-only 系列），当前 4.2.2（2026-08-28），为首个 4.x LTR。
- QGIS 4.x 要求 **Qt ≥ 6.6**。qgis.org 仅为 **Debian trixie/sid、Ubuntu plucky(25.04)/questing(25.10)/resolute(26.04)** 构建 4.x 包（bookworm/jammy/noble 标注 LTR-only——发行版 Qt6 太旧）。

## 二进制可用性（按 4.2.x 精确 pin）

| 组件 | Linux | Windows | pin 方式 |
|------|-------|---------|----------|
| QGIS 4.2.x + dev 头 | qgis.org/ubuntu + qgis.org/debian：`libqgis-dev` + `libqgis-core4.x/gui4.x/analysis4.x`（版本化 .deb，pool 保留旧版）*实现前需验证：`apt-get download libqgis-dev=4.2.*`* | OSGeo4W：`qgis-4.2.2-1` + **`qgis-devel-4.2.0-1`**（headers + import libs，已确认存在） | 版本化 URL + SHA256/SHA512 |
| Qt6 | 发行版 qt6-base-dev（questing 6.9.2 / trixie 6.8.2）或 aqt 官方二进制 | OSGeo4W qt6-devel 6.11.1（推荐与 vendor 同源）或 aqt `win64_msvc2022_64` | 同上 |
| GDAL/PROJ/GEOS/其余 | deb 闭包 | OSGeo4W dep 闭包 | 同上 |
| ONNX Runtime | GitHub 官方 release `onnxruntime-linux-x64`（manylinux_2_28，glibc≥2.28） | `onnxruntime-win-x64`（MSVC） | 自建 SHA256（官方无 sidecar） |
| 运行时资源（srs.db/proj.db/GDAL_DATA/SVG/Qt 插件） | `/usr/lib/qgis`、`/usr/share/qgis` + distro data | `apps/qgis/{resources,svg,plugins}`、`apps/Qt6/plugins` | 随包自带 |

## ABI / 工具链配对

- **Windows**：OSGeo4W 用 VS2022 cl.exe v143 `/MD` —— 应用必须 MSVC v14x + /MD，MinGW 不可链接。Qt 必须 `win64_msvc2022_64`。
- **Linux**：vendored deb 前缀要求宿主 glibc/libstdc++ ≥ 构建发行版（trixie = 2.41/GCC14）。打包不能修复「比宿主新的 glibc」。
- **Qt minor 锁**：Qt 大版本内向前兼容——编译期 Qt ≤ vendor 运行期 Qt。最干净做法：直接消费 vendor 同源的 Qt（OSGeo4W qt6-devel / distro qt6-base-dev），不要引入第三方 Qt。

## 合规

- Qt LGPL：动态链接官方二进制 = 标准合规路径（notice + source offer + 可重链）。
- **QGIS 本身 GPL-2.0+**：无论哪条 vendor 路线，链接 QGIS 的组合作品即为 GPL——本项目已确认遵循 GPL（台账 D3），非阻塞项。

## 对比

| 维度 | Source superbuild | Binary vendoring |
|------|-------------------|------------------|
| pin 精度 | tarball SHA256 | 版本化 URL + SHA256（OSGeo4W 有日期快照） |
| 冷 TTHW | ~1.5–3h+（8 核无 ccache 大概率超 2h） | ~5–15min（fetch+extract） |
| CI 复杂度 | 多小时 job + 缓存管理 + 构建依赖漂移 | 下载脚本 + artifact cache |
| 增量开发 | 依赖编一次后免费 | 依赖永不重编 |
| patch 升级 | bump tag → 重编 ~1h | bump pin+hash → ~分钟 |
| 磁盘 | ~30–60GB | ~5–10GB |
| 主要失败模式 | 缺构建依赖、configure 漂移 | 仓库布局变化、dep 闭包漂移、glibc floor |
| 裁剪 | 全控（WITH_3D/PDAL off） | 拿全量 runtime payload |

## 结论与阻塞点

**Windows 无阻塞**（`qgis-devel-4.2.x` 已确认）。**Linux 唯一强阻塞 = 发行版/glibc floor**：qgis.org 的 4.x 包只在 trixie/plucky/questing/resolute 上跑；若产品必须支持 Ubuntu 24.04 及更老宿主，二进制路不可行 → 回到「最老目标平台上做 superbuild」。

**推荐路线（待用户批准）**：binary vendoring（混合源）——Windows 用 OSGeo4W `qgis`+`qgis-devel`+dep 闭包（URL/SHA512 pin，日期快照保证不可变）；Linux 用 qgis.org deb 闭包（`libqgis-dev`+依赖，发行版钉 resolute/trixie）解包进 vendor prefix；ONNX Runtime 用官方 release。回退条件：`apt-get download libqgis-dev=4.2.*` 验证失败，或必须支持 Ubuntu 26.04/Debian 13 之前的宿主。

**待验证项**（实现时）：(a) `libqgis-dev_4.2.x` 在 qgis.org pool 的直接清单；(b) `qgis-devel-4.2.x` 的 qgis_analysis 头文件完整性；(c) OSGeo4W Qt 6.11.1 与 aqt 6.11.x 的 patch skew——最安全是直接用 OSGeo4W qt6-devel；(d) ORT 无官方校验和，vendor 时自建 SHA256。
