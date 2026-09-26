# 目标平台矩阵（Phase 0 §39/E3 钉定）

状态：钉定 + 当前实测行。最低集 = **Linux x86_64 + Windows x86_64**（plan
§39 spike 验收判定原文）；macOS / 非 x86_64 不在矩阵内（内部工作站产品，
无此需求——加行须先过平台基线评审，见文末「加行流程」）。

| # | OS × 架构 | Qt 来源 | QGIS 来源 | 状态 | 验证手段 |
|---|---|---|---|---|---|
| 1 | Arch Linux（rolling，glibc 2.44）× x86_64 | 发行版 `qt6-base 6.11.2` | 发行版 `qgis 4.2.2-1`（`/usr/include/qgis` + `-lqgis_core/_gui/_analysis`） | ✅ **本机实测**（开发主力机） | `ninja -C build` + `QT_QPA_PLATFORM=offscreen ctest`（全套，含 `tst_smoke_realdata` 真数据档） |
| 2 | Ubuntu 26.04（CI runner）× x86_64 | `qt6-base-dev` 等 dev 组 | qgis.org deb 源（`resolute` 套件）`libqgis-dev` + `paleo-dev bootstrap` | ✅ **CI 实测**（`.github/workflows/ci.yml` linux job） | 同上 + `paleo-dev selfcheck`（prefix/providers/srs.db/渲染断言） |
| 3 | Windows Server（CI `windows-latest`）× x86_64 | OSGeo4W `qt6-devel`（与 QGIS 同源） | OSGeo4W `qgis` + `qgis-devel` 4.2.x（`vendor/manifest.json` pin 安装器 SHA256） | ✅ **CI 实测**（ci.yml windows job；本机无 Windows） | `paleo-dev.ps1 bootstrap/build/test`（MSVC `/MD`，offscreen） |
| 4 | Debian 13 / 其他 glibc ≥ 2.41 宿主 × x86_64 | 发行版 qt6 dev | qgis.org deb 闭包解包进 `vendor/prefix/usr`（`vendor/deb-closure.lock` 锁 SHA256） | ⏳ 路线就绪未逐发行版实测（行 2 已覆盖同族） | `./vendor/fetch-deps.sh` + `QGIS_PREFIX_PATH=vendor/prefix/usr` |
| 5 | glibc < 2.41 老宿主 × x86_64 | 发行版 | ExternalProject superbuild（源码自建） | 🧱 **骨架**（`vendor/superbuild/`，未启用） | 启用条件见 `vendor/superbuild/README.md` 回退条款 |
| 6 | macOS / arm64 | — | — | ❌ 不在矩阵 | — |

## 版本约束（行 1–4 共同）

- **QGIS 4.2.x**（headers C++20：defaulted `operator==`）；ABI：Linux 系统
  包随发行版；Windows 钉 MSVC v14x + `/MD`（`vendor/manifest.json`）。
- **Qt ≥ 6.6**（QGIS 4.x 要求；实测 6.11.2）。WebEngineWidgets 可选
  （`CMakeLists.txt:27` QUIET 探测，缺失时 WebViewPanel 降级外链浏览器）。
- **GDAL/PROJ/GEOS**：随 QGIS 来源（行 1 实测 GDAL 3.13.3 / PROJ 9.8.1 /
  GEOS 3.15.0；行 2/3 由 apt/OSGeo4W 闭包决定）。
- **ONNX Runtime 1.30.0**：与平台矩阵正交（GitHub release pin，manylinux_2_28
  glibc floor 2.28）；未 vendor 时构建降级——`ai/` 服务与 `tst_onnx*` 排除。

## 实测基线（写死在 CI 与本机的两档）

| 档 | 命令 | 当前结果 |
|---|---|---|
| 本机（行 1） | `ninja -C build` → `QT_QPA_PLATFORM=offscreen ctest --output-on-failure` | 62/62（wave4 起；`PALEO_REAL_PROJECT_AREA` 指向真工区时含真数据 smoke） |
| CI（行 2/3） | `./paleo-dev build && ./paleo-dev test`（+ selfcheck） | 两 leg 全绿为合并门 |

## 加行流程（新平台/新架构）

1. 先答两个问题：QGIS 4.2 在该平台有没有**二进制**来源（发行版包 /
   OSGeo4W 等价物）？没有 → 走 `vendor/superbuild` 回退（行 5 同款决策记录）。
2. 在 `PLATFORM_MATRIX.md` 加行（OS × 架构 × Qt/QGIS 来源），状态必须如实
   标「未实测」直到有 CI leg 或本机证据。
3. CI leg 复用 `paleo-dev` / `paleo-dev.ps1` 入口（plan §44.1：人工步骤 = 1）；
   Windows shell 差异已由 pwsh 脚本吸收，勿在 workflow 里写平台裸命令。
