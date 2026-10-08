# Goal-Loop 方向 78：mkproject 夹具工厂化——manifest 参数化 + 测试消费闭环

## 背景（实测事实，勿再勘察；行号为 2026-10-08 master `e3c8d31d` 口径）

`paleo_mkproject`（#261 交付）是无头建工程生成器：
`src/app/mkproject.cpp`（619 行，组装根）——QgisProjectService
createProject 双件 → 清单导入（well_head、well_stratification×2、
well_log/LAS×21、seismic/SEG-Y 外链冻结几何、image_reference
岩心+薄片、cuttings、reference 文档、outsource_workbook）→
地理配准七井最小二乘 → 重开自校验。**但它现在不是测试夹具**：

- **清单硬编码鄂尔多斯竞赛数据目录**（mkproject.cpp:371-377
  rel 路径），无参数化数据源——离开这套数据无法运行。
- **tests/ 全目录 `mkproject|paleo_mkproject` 零命中**（唯一
  引用是 CMakeLists.txt:325 的 add_executable 定义）；现行测试夹具走
  `PerfFixtures::makeSyntheticCatalogDir`（tst_assetpaging.cpp:181
  等 6 处）——**synthetic 夹具绕过真实生产导入路径**（io/
  QGIS 面）。
- 能造不了的实体：断层面/相栅格/版本链衍生资产等编图产物
  不在生成清单。

价值：manifest 参数化 + 微型合成数据集后，30+ 导入相关测试
可获得「真跑生产导入路径」的覆盖（io 解析→catalog 登记→
工程文件 round-trip 全链），这是 synthetic 夹具盖不到的面。

## 环境接线（Windows 本机实测口径；方向 72 后统一链）

```powershell
cd C:\Users\wangj.KEVIN\projects\paleo_workstation
git fetch origin
git worktree add .worktrees\mkproject-fixtures -b goal/mkproject-fixtures-20261009 origin/master
cd .worktrees\mkproject-fixtures
./paleo-dev.ps1 build
./paleo-dev.ps1 test
```

等价手工接线见 BUILDING.md:140-152。

## 目标形态（建议按序）

1. **manifest 参数化**：清单外置——`tools/reference/mkproject/
   manifest.schema.json`（实体类型/数量/字段/数据文件相对
   路径的声明式描述）+ mkproject 读 manifest 替代硬编码清单
  （向后兼容：无 manifest 参数时保留现有竞赛数据行为）。
2. **微型合成数据集**：`tools/reference/mkproject/mini/`——
   2-3 口井 × 每 LAS 数百采样 × 微型 SEG-Y（几条 IL）×
   几张 1KB 级 JPG × 微型岩屑 CSV，脚本生成（进仓可复现）
   或直接仓库化小文件；体量以「全链 ctest 秒级」为准。
3. **夹具工厂**：`tests/fixtures/mkprojectfixture.{h,cpp}`——
   QProcess 驱动 `paleo_mkproject --manifest mini.json --out
   <tmpdir>`（沙箱监狱口径下 QTemporaryDir 可用——方向 72
   后已修）+ 就绪等待 + catalog 摘要断言 helper。
4. **首轮消费测试**：选 3 个高价值面接夹具——导入全链
  （各实体类型经真实 dataimportservice 路径落 catalog）；
   工程重开 round-trip（qgz 双件 + catalog 摘要一致）；地理
   配准（七井最小二乘残差断言）。不要求全量替换 synthetic。
5. **CI 面**：新测试进 core 段（不进 perf）；Windows 串行
   兼容（QProcess 子进程在沙箱监狱内的行为——R0 验证，
   若监狱策略挡子进程则按方向 72 的 SEISMIC_INDEX_CACHE_DIR
   先例树内化）。
6. **文档**：tools/reference/mkproject/README（manifest 格式 +
   mini 数据集 + 消费方式）；devex.md 测试面一节更新。

## 通用纪律（方向内全程有效）

- **分层**：mkproject 改造留 `src/app`（组装根，by design 可
  include 一切）；夹具在 tests/；`check_layering.py --strict` 绿。
- **行为红线**：mkproject 无 manifest 时的现有行为零变化
  （对拍——竞赛数据路径输出一致）；新增面只加不改。
- **资源**：`./paleo-dev.ps1` 系；ctest 串行；mini 数据集
  秒级约束。
- **无人值守**：manifest schema 设计自行定案记 ledger。
- **ledger**：`.goal-loop-ledger-mkproject-fixtures.md`。
- **多轮 review（硬要求）**：每批 → 测试全绿 → diff 自审
  （分层/向后兼容/夹具真实性/沙箱兼容/文档 五维）→ 修复 →
  再 review，至少两轮零 High/Medium；Low 记 PR body。
- **提交**：原子提交，中文 conventional 前缀。

## Oracle 验收（逐条需验证证据）

1. manifest 化：mini manifest 驱动 mkproject 全链跑通（产出
   工程文件 + catalog 摘要）；无 manifest 时旧路径行为对拍
   零差异。
2. 夹具工厂：QProcess 驱动在沙箱监狱下可复用（两个连续
   实例测试；TEMP/TMP 树内化生效）。
3. 消费测试：3 个新测试全绿——导入面逐实体类型断言 catalog
   登记（实体数/角色/版本数）；round-trip 摘要一致；配准
   残差 ≤ 阈值（断言口径记 ledger）。
4. 覆盖增量：新测试触达的 io 解析路径清单（哪些解析器从
   synthetic-only 变为生产路径覆盖——文件:行号对照）。
5. 体量：mini 数据集 ≤2MB；夹具测试墙钟秒级（比率口径）。
6. 全量 ctest 对照 R0 红集合 diff 为空；layering 三档绿。
