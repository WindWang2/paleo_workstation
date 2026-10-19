# 审查覆盖清单（full-audit-20261020）

来源：5 个只读 subagent 报告（/tmp/paleo-audit-r4/findings-agent-{A..E}.md，全文入库前的过程产物）+ 主 Agent 复核。

## 覆盖统计

| 域 | 审查范围 | 逐行深审 | 接口级/抽查 | 未核查（如实声明） |
|---|---|---|---|---|
| A 数据/Catalog/IO/Metadata | src/{domain,catalog,io,metadata} | catalog 全 11 TU（store×4+datacatalog×6+index）、io 六大文件（ziparchive/sfpkg×2/dlis/lis/segy 边界+扫描链）、metadata 六头+mapversionstore/metastore/atomicfile、§4.2 模式全扫 | domain 值类型、io 中小文件（lasparser/outsourceworkbook/constraintstore/wellcompositexml 等主体）、paleoprojectfile/cachecore/streaming | domain 主体 ~30 文件、io 其余中大文件（上两轮已深扫大部分） |
| B 算法/Services | src/{algorithms,services} | **singlefactor 全 17 文件**（上轮移交缺口补齐）、faultsurface 条带剖分主体、dsp/fft、seismicattr、geostat 三核、velocitymodel、sattrio/crossplot×2/storagegovernance/paleomapfiles、paleotaskservice/jobrunner/seismictaskservice 全链 | batchjobqueue 契约面、imagelod | 无（本域上轮移交面全部补齐） |
| C Workflow/AI/联动 | src/{workflow,ai,linkage} | **#579 新链路全量**（paleomaplibrary/paleomapimport/assetops/paleomapfiles/paleomapsource/paleomapfacies/datalist 族 6 TU）、X-01 复核链、workflow 主干 13 文件 | ai 域（onnx/remotepredictrouter/wellfaciesservice 重点段）、linkage 全域 | workflow 长尾（batchjobqueue 之外的低频编排器） |
| D QGIS/UI | src/{qgis,ui} | **上轮移交缺口补齐**：correlation 全族 8 文件、seismic3d 全族 12 文件（viewpanel 1828 行全读）、layout 全族 9 文件、wellsection 全族 2 大文件、#579 datalist 新操作行、mappingreferencepanel、constraintpage 换勾链 | maptools 生命周期抽查、DESIGN token/错误三级/HighDPI/双主题合规抽查 | ui/pages 其余 20+ 页（上轮已接口级）、qgis 封装其余（上轮已深扫） |
| E 构建/测试/架构 | tests/、tools/、cmake/、CMakeLists.txt、.github/、src/{app,selfcheck} | ci_triage_reds.py 全文、ci.yml triage/lint 段、CMakeLists 1709 行全读、BUILDING.md 全读、tst_paleomaplibrary 600 行逐行、known-reds 全文、闸门脚本 6 个 | tests/ 注册面对账（400 cpp 全量脚本对账）、tools 检查器可绕过性 | resources/ 内容级（许可证/注册面已核） |

## 与上轮移交缺口的对账

- 上轮「D 域 correlation/seismic3d/layout 大面（仅接口级）」→ 本轮**全量逐行**（D-05..D-09 即出自该面）。
- 上轮「B 域 singlefactor 12 文件、faultsurface 深网格段」→ 本轮**全量逐行**（B 域数值面正面确认为主，B-10/B-11 新登记）。
- 上轮登记未修 9 项（B-03/B-04/B-05/A-04/D-01残留/D-02/E-02/E-05/E-07）→ **全部复核收口**：
  A-04→A-13 证实；B-03→A-14/B-03 证实+机制修正；B-05→A-15/B-05 升级 VERIFIED；
  B-04→证实（爆炸半径为零）；D-01 残留→D-13/C-13 证实+最小方案（含上轮两次回退根因）；
  D-02→D-12 证实+方案；E-02→E-10 升级（三新盲区）；E-05→E-12 收口；E-07→持平 RECORD-ONLY。
- 上轮 X-01（#568 活跃面排除）→ #568 已合并，重新复核：copyDataset 孤儿已修（C-14 契约残留）、
  surveyArea /1e6 仍在（C-11）、worker 内 QgsLayer 仍在（C-12，可辩护降级）。

## 新代码审查（#579，816f9b40..BASE）

14 文件 +413/-28 全量逐行（C/D/E 三域交叉）：
- workflow 侧五重发布守卫齐全，唯一缺口 C-10（文件重导入无回收防线 + declareMaps 不过滤回收清单）。
- UI 侧使能条件/状态同步符合 DESIGN 2026-10-19 决策；D-11（§35 reason tooltip）。
- 测试侧 +169 行为真实行为测试（E 域 verdict：零 mock、零永真）。

## 排除面执行

4 个开放 issue（#556/#558/#559/#561）与 PR #580 的 51 文件/16 已修项：5 份报告均按 01-conflict-matrix.md 执行，未重报；同文件新问题照常登记（如 #580 触碰的 datalist_paleomaps.cpp 的 C-10 关联面在 paleomaplibrary.cpp 主导）。
