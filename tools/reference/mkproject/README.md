# mkproject 夹具工厂（方向 78）

`paleo_mkproject` 的清单外置参数化与微型合成数据集：让「无头建工程」离开
鄂尔多斯竞赛数据也能跑，成为可复用的**测试夹具工厂**——30+ 导入相关测试
从此可以获得「真跑生产导入路径」（io 解析 → catalog 登记 → 工程文件
round-trip 全链）的覆盖，这是 `PerfFixtures` synthetic 夹具盖不到的面。

## manifest 模式

```
paleo_mkproject --manifest <manifest.json> [--out 目录] [--name 名字]
```

- `imports[].path` 相对 **manifest 所在目录**解析（绝对路径直用）；
  `--data` 不要求（给了也只作竞赛模式入口，与 manifest 无关）。
- `--out` 缺省 = manifest 所在目录（就地工程语义，与竞赛模式对称）；
  夹具消费时总是显式给临时目录。
- manifest 描述：`schema`（=1）、`georeference`（可选，**形状与
  project.paleo 的 georeference 节完全一致**——同一个
  `paleoGeoreferenceFromJson` 解析器）、`imports`（有序导入清单：
  `type`=forceType / `convert`=工作簿先转规范井文本 / `linkExternal`）、
  `expect`（第 5 步重开自检期望：wells / entitiesByType / unresolvedLinks /
  georeferencedWells / seismicGeometry——只断言出现的键）。
- 井口先行契约由 `imports` 排序承载（`well_head` 排最前）。

Schema 全文：[manifest.schema.json](manifest.schema.json)。

**向后兼容红线**：无 `--manifest` 时竞赛路径行为零变化——分支在 `--data`
检查之前早退，竞赛代码体零改动（唯一共享面 = 井名泄露兜底块的 verbatim
提炼）。行为证据 = 竞赛形状树对拍（见下）。

## mini 数据集（`mini/`）

`mini/`（~150 KiB，全链 ctest 秒级）是 manifest 模式的参考数据集：

| 面 | 内容 |
|----|------|
| 井 | 3 口（A1/A2/A3），井位 SpreadsheetML 工作簿（`convert=well_head` 走竞赛同款转换） |
| 分层 | 双源：位置约定 `tops.dat` + 头驱动列序 `tops_header.dat`（列序故意打乱） |
| 测井 | LAS×4（A3 双份——多文件井面），400 采样 × DEPT/GR/DT/RHOB/NPHI |
| 地震 | 微型 SEG-Y 4 IL × 5 XL × 64 采样（`tools/make_segy_fixture.py` 生成——道头约定单一真源） |
| 井附件 | 岩心 JPG×3（深度锚在文件名）、薄片 JPG×1、岩屑 CSV×3（目录名带角色关键词） |
| 参考 | 工区说明 docx + 外委工作簿 xml（`linkExternal` 外链口径） |
| 配准 | 恒等相似变换 + A3 纬度微扰 3.0m（残差断言有物理意义） |

重生成（确定性，逐字节可复现）：

```
python tools/reference/mkproject/mini/generate.py
```

生成器 stdlib-only；SEG-Y 段 subprocess 复用 `tools/make_segy_fixture.py`。

## 夹具消费（测试侧）

```
tests/fixtures/mkprojectfixture.{h,cpp}   # QProcess 驱动 + catalog 摘要 helper
tests/tst_mkprojectfixture.cpp            # 四断言面消费测试
```

- `MkProjectFixture::buildMiniProject(outDir)`：QProcess 跑
  `paleo_mkproject --manifest mini/manifest.json --out <tmpdir>`，环境全继承
  （ctest 的 XDG 沙箱/QGIS_PREFIX_PATH 与 paleo-dev 的树内 TEMP/TMP 同监
  子进程——沙箱监狱口径，方向 72）。
- `MkProjectFixture::openCatalogSummary(projectDir)`：`DataCatalog::open`
  直读 catalog.json 给摘要（井数/实体类型表/角色计数/未决链接/逐井角色）。
- CMake：`MKPROJECT_BIN=$<TARGET_FILE:paleo_mkproject>` +
  `MKPROJECT_MINI_DIR` 编译定义注入；`RUN_SERIAL`（QProcess 子进程资源面，
  对齐 tst_perfbudget 先例）；LABEL `services`（core 段，不进 perf）。

## 竞赛形状树对拍（向后兼容证据）

```
python tools/reference/mkproject/make_compshape_tree.py --out <throwaway-dir>
# R0 二进制（f31ee461 原版 mkproject.cpp 编出）vs 改造后二进制同树各跑：
paleo_mkproject --data <throwaway-dir> --out <out1>   # > r0.log
paleo_mkproject --data <throwaway-dir> --out <out2>   # > new.log
# 掩码归一后逐字节比对（掩掉输出目录名自引用与 Qt 线程地址噪声）：
sed -e 's/out1/OUTDIR/g; s/out2/OUTDIR/g' -e 's/0x[0-9a-f]*/0xADDR/g' …
cmp <r0.norm> <new.norm>   # 一致 + 退出码一致 = 零差异
```

`make_compshape_tree.py` 按第 9 届竞赛相对路径一比一生成最小合成树
（20 井 xlsx/分层 per-井 sheet/21 LAS/tops dat/SEG-Y/岩心薄片粒度岩屑/
docx/外委 xml），xlsx 走 sharedStrings（`readWorkbook` OOXML 面只认这条）。
树是 throwaway（不进仓），生成器进仓保证可复现。三用例口径（全链树 /
mini 目录作 --data 的缺件路径 / 无参数用法）与实测证据见方向 78 ledger。
