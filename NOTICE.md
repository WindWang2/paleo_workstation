# NOTICE — paleo_workstation 第三方组件归属清单

本文件登记 `paleo_workstation` 仓库内随源码一起流转的第三方材料及其许可状态，
是 `docs/PALEO_QGIS_PLAN.md`（QGIS 封装）与各 `vendor/*/README.md`（溯源/补丁）
之外**唯一**的许可归属汇总面。#738 立项时仓库根与 `vendor/mamcl` 均无任何许可文本，
本文件补齐「归属与状态」这一半。

## paleo_workstation 自身

**本仓库自身的 LICENSE 尚未定稿**（截至 2026-10-28，仓库内无任何许可声明，
见 issue #738）。在所有者明确定稿前，请按「保留所有权利」对待：不要复制、
不要再分发、不要在外部产品中引用。定稿后在此节写明许可名称与年份，并补根
`LICENSE` 全文。

## 源码内 vendored 组件

| 组件 | 位置 | 上游 | 许可 | 依据 |
|---|---|---|---|---|
| SARibbon v2.9.5 | `vendor/saribbon/` | https://github.com/czyt1988/SARibbon | MIT | `vendor/saribbon/README.md`「许可」行 + 同目录 `LICENSE` |
| segyio（本地补丁版） | `vendor/segyio/` | https://github.com/equinor/segyio | LGPL | `vendor/segyio/License.md`（LGPL 全文） |
| glm（header-only） | `vendor/glm/` | https://github.com/g-truc/glm | MIT | `vendor/glm/copying.txt` |
| Seismic-Body-Management 引擎 | `vendor/sbm/` | https://github.com/changyanyanchang/Seismic-Body-Management（pinned `aae56c77…`） | **上游未发布顶层许可**——内部资产，勿外流 | `vendor/sbm/README.md`「License」节 |
| MAMCL 地震多属性智能分析 | `vendor/mamcl/` | 第三方交付的打包程序（`MAMCL_software_W04_v0.2.1_20260924.zip`） | **包内无任何许可文本**——许可状态待定 | `vendor/mamcl/NOTICE.md` |
| QGIS 属性表封装 | `vendor/qgis_attribute_table/` | QGIS 上游 | GPL-2.0-or-later | `vendor/qgis_attribute_table/README.md` + `COPYING` |
| 离线底图瓦片（MBTiles） | `vendor/basemap/` | Esri 公开瓦片服务抓取 |  Esri 服务条款（非入库产物，`.gitignore` 忽略） | `vendor/basemap/README.md` |
| Ranuts 文档编辑器 | `vendor/ranuts-document/` | 上游静态站点产物 | 许可状态待定 | `vendor/ranuts-document/README.md` |

## 构建/运行期外部依赖（不入库，按锁文件取用）

| 依赖 | 取用处 | 许可 |
|---|---|---|
| QGIS / GDAL / PROJ / GEOS（deb 闭包） | `vendor/deb-closure.lock`、`vendor/fetch-deps.sh` | GPL-2.0+ / LGPL / MIT / GPL-3.0（各随包） |
| QGIS 栈（OSGeo4W） | `vendor/manifest.json` `deps.osgeo4w`、`vendor/fetch-deps.sh` | 同上 |
| ONNX Runtime | `vendor/onnxruntime/`（不入库归档） | MIT |
| LibreOffice（legacy 转换兜底） | `vendor/fetch-libreoffice.sh` | MPL-2.0 / LGPL-3.0 |
| Qt6（系统/发行版包，政策例外） | CI apt / 本机 deps | LGPL-3.0 / GPL-2.0+ / 商业 |
| QtKeychain（QGIS 既有依赖） | `cmake/extra-well-facies.cmake` | LGPL / BSD |

外部依赖的版本钉子在 `vendor/manifest.json`、`vendor/deb-closure.lock` 与
`vendor/osgeo4w` 安装树；一致性由 `tools/check_qgis_versions.py`、
`tools/check_lo_pin.py`、`tools/check_mamcl_lock.py` 逐轮看守。

## 维护纪律

- 新增 `vendor/<name>` 时**必须**在本表加一行（上游地址 + 许可 + 依据文件路径）。
- 「许可状态待定」条目不得静默外流；定稿或拿到许可文本后同步更新对应
  `vendor/<name>/README.md`（或新增 `NOTICE.md`）并删掉本行的待定标注。
- 上游无许可文本的组件（sbm / mamcl）在 README 里写清理由，不要凭空补一份
  看起来像许可的文件。
