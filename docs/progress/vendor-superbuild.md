# vendor superbuild 启用 — 自编译首选路线交付底账

> 2026-10-01 交付。抓手：用户政策指令「尽量不依赖系统库，尽量自编译 vendored」
> （政策全文 BUILDING.md「依赖来源策略」，AGENTS.md 已加指引行）。
> TODOS P2「superbuild 回填 pin 并启用」随本底账关闭。

## 交付一览

| 项 | 交付 | 验证 |
|---|---|---|
| S1 | 四个底座钉位回填并实测 SHA256：GEOS 3.15.0 / PROJ 9.8.1 / GDAL 3.13.3 / QGIS 4.2.2（tarball 存 `vendor/cache/tarballs/`，gitignored） | 哈希实测、压缩完整性校验、版本断言（顶层目录名） |
| S2 | 修静默 bug：CMake 4.x 下骨架 URL 判空守卫 `if(NOT "${var}")` 恒真——填了 URL 也永不启用；改 `STREQUAL ""` | 空/非空两态回归 |
| S3 | 裁剪开关对齐 QGIS 4.2.2 tarball 实证：`WITH_APP` 无此 option → 映射真名 `WITH_DESKTOP`；删不存在的 `WITH_MESH`（mesh 已并入 core）；`WITH_DESKTOP=OFF` 连带裁掉 Qml/Quick/QuickControls2 依赖面 | configure 后 QGIS desktop app 目标不生成 |
| S4 | spatialindex 门禁绕过：系统 libspatialindex 2.1 触发 QGIS `CMakeLists.txt:455` FATAL（要求 <2.1）→ `WITH_INTERNAL_SPATIALINDEX=ON` 走 tarball 内置 2.0.0，与 Arch 官方 qgis 包同策 | QGIS configure 打印 `Using internal spatialindex` |
| S5 | 宿主预检全量复核（Arch PKGBUILD makedepends 反推：nasm/opencl-clhpp/libspatialindex/fcgi、nlohmann-json 装齐；QGIS 全 FATAL/VERSION 门禁逐项核对——Qt6Sql 私有头实测在位） | configure exit 0 无 REQUIRED 缺失 |
| S6 | superbuild 首轮构建：geos→proj→gdal ~7min；qgis 门禁修复后 ~48min；产物 `vendor/superbuild/prefix`（434MB：qgis 4.2.2 三库 + proj.db/srs.db/GDAL 数据） | `[8/8] Completed 'qgis'`；QGIS/GEOS/PROJ/GDAL 配置全部解析到 prefix 而非系统 |
| S7 | 主构建切换验收：`QGIS_PREFIX_PATH=vendor/superbuild/prefix` 新 build 树 789 targets 全绿（~11min）；**rpath 已烙，运行零环境变量**（ldd 解析到 prefix）；ctest 129/129（37s）；selfcheck 8/8（357ms） | CMakeCache `QGIS_CORE_LIB` 指向 prefix |
| S8 | 消费侧接线：`paleo-dev` 顶部统一注入 prefix（superbuild 首选 > deb 闭包 > 系统兜底，显式 `QGIS_PREFIX_PATH` 优先），build/test/selfcheck 全 verb 受用；`clean-vendor` 全覆盖 superbuild prefix | 注入逻辑回归（显式值不被覆盖） |

## 实测值

- superbuild 首次全量 ≈ 56min（含一次门禁失败重试；构建树 `vendor/build` 3.5GB，续跑保持——**删 prefix 需重跑**）
- 主构建切 prefix 全量重编 789 targets ≈ 11min（ccache 兜底增量）
- 全套 ctest 129/129，37s；`paleo_selfcheck` 8/8 PASS，357ms
- prefix 434MB + tarball 缓存 224MB（`vendor/cache/`，已 gitignore）

## 遗留

- CI 侧 superbuild 产物缓存策略未建（当前 CI 走 deb 闭包加速档，渐进切换，
  属 CI 改动另开任务）
- Windows superbuild 未评估（当前 OSGeo4W 加速档）
