# Paleo Workbench

古地理/地质解释与编图工作台。Qt6 Widgets 桌面应用，vendored QGIS 4.2.x LTS 提供 GIS 底座（渲染/图层/CRS/编辑/符号化/布局），地质业务（井-震-平面联动、约束建模、AI 预测、验证、发布）由本项目实现。**C++ only，无 Python/PyQGIS。**

## Quickstart

```bash
git clone https://github.com/WindWang2/paleo_workstation.git && cd paleo_workstation
./paleo-dev bootstrap    # preflight -> vendor deps -> selfcheck（尾跑，输出 map.png）
```

成功标志：`SELFCHECK OK` + `vendor/logs/map.png` 是一张真的地图。

**宿主要求**：Linux x86_64、glibc ≥ 2.41 可用发行版 QGIS 4.2.x 包或 superbuild；**vendored deb 闭包需 glibc ≥ 2.43**（Ubuntu 26.04 级，Debian 13 不满足；Arch 天然满足）、≥15GB 磁盘；Windows 走 CI（windows-latest + MSVC v143）。

## 开发入口 `./paleo-dev`

| verb | 作用 |
|------|------|
| `bootstrap` | preflight + vendor + 自动尾跑 selfcheck |
| `build` | ninja 增量构建（RelWithDebInfo） |
| `test` | `QT_QPA_PLATFORM=offscreen` ctest |
| `selfcheck` | provider/srs.db/渲染 自检 → map.png |
| `clean-vendor [dep]` | 清 vendor 构建物（留下载包） |

构建/依赖/升级细节见 [BUILDING.md](BUILDING.md)；架构与评审决策见 [docs/PALEO_QGIS_PLAN.md](docs/PALEO_QGIS_PLAN.md)。

构建与测试并行度上限为 8：`cmake --build build -j8`、
`ctest --test-dir build -j8 --output-on-failure`（Windows ctest 串行）。
独立 worktree 的 vendor symlink 与 `QGIS_PREFIX` 接线见
[BUILDING.md 的 worktree 说明](BUILDING.md#独立-worktree-开发)。

> **勘误（2026-10-07，方向 72）**：八、九批任务书（方向 48-72）「环境接线」
> 段所载 Windows 命令（`CMAKE_PREFIX_PATH` 首位 `C:/deps/Qt/6.8.0/...` +
> 运行期 PATH 前置 deps bin）是 Qt 6.8 编 / 6.11 载的混链口径，已废弃；
> 统一链（Qt 6.11.2 编译+运行）以
> [BUILDING.md「Windows 本机开发」](BUILDING.md#windows-本机开发localdeps-统一链方向-72)
> 与 `./paleo-dev.ps1` 为准。原文不改，以此行为准。
