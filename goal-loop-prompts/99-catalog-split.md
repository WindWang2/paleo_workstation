# Goal-Loop 方向 99：catalog 层深化——datacatalog 1,795 + catalogstore 1,605 双巨兽解体 + 词表面收敛

## 背景（实测事实，勿再勘察；行号为 2026-10-09 master `32851a1e` 口径）

catalog 是数据层最大的两个文件，且是全部方向的共同底座：

- `src/catalog/datacatalog.cpp` **1,795 行**——目录门面
 （实体/资产/链接/版本/角色/派生链/realization 集合查询
  ——方向 47/69/77/84 反复加查询面）。
- `src/catalog/catalogstore.cpp` **1,605 行**——SQLite 存储
  （catalog-sqlite 方向 16 迁移产物 + 历次 schema 增量）。
- **敏感性**：catalog 是所有测试与功能的底座——拆分回归
  面最广（tst_catalog/tst_realizationset/tst_mkprojectfixture/
  几乎全部 workflow 测试间接依赖）。
- **词表面机会**：`datacatalog.h:59` 角色注释 → 方向 92 若
  已落 `catalogRoles()` 则对齐；realization 集合查询
  （realizationset.h）与实体查询的边界——R0 勘察。
- **拆分先例**：方向 16 的 catalog-sqlite 迁移账本 + 方向
  55/65/66 的 TU 拆分模式；catalog 测试基础最好
 （tst_catalog + fixture 面广）。

## 环境接线（Windows 本机实测口径；方向 72 后统一链）

```powershell
cd C:\Users\wangj.KEVIN\projects\paleo_workstation
git fetch origin
git worktree add .worktrees\catalog-split -b goal/catalog-split-20261010 origin/master
cd .worktrees\catalog-split
./paleo-dev.ps1 build
./paleo-dev.ps1 test
```

等价手工接线见 BUILDING.md:140-152。

## 目标形态（建议按序）

1. **R0 结构勘察**：datacatalog 按查询族盘点（实体/资产/
   链接/版本/派生/realization/属性——#299 加的
   wellattributestore 是独立文件好先例）；catalogstore 按
   SQL 域盘点（schema DDL/实体 CRUD/版本/链接/迁移）；
   切分线记 ledger 定案。
2. **公共 API 冻结**：两头的类定义公共区 diff 对拍零变更；
   消费方零改动（catalog 扇入全仓最大——rg 消费清单先存档）。
3. **datacatalog 拆分**：按查询族拆 TU
  （datacatalog_<族>.cpp：_entities/_assets/_links/_versions/
   _derivation/_realization）+ internal.h；每 TU ≤700 行；
   主文件 ≤900 行。
4. **catalogstore 拆分**：DDL/迁移与 CRUD 分离
  （catalogstore_schema.cpp / catalogstore_<域>.cpp）；
   SQLite 事务边界语义逐条保留（journal→事务原子化是
   方向 16 的核心承诺）。
5. **词表面收敛**：`catalogRoles()`（若方向 92 已落）与
   realization 集合查询对齐；`datacatalog.h` 头注释刷新
  （当前契约描述 vs 实际查询面的漂移——R0 记录）。
6. **测试口径**：tst_catalog 全绿零改动×2 遍（底座回归的
   金标准）；mkprojectfixture 双实例测试（真实 SQLite 路径）
   绿；全量 ctest 两遍。
7. **收口**：CMake 更新；include 纪律；性能门（catalog 打开
   /查询比率门——tst_coldstart 的 catalog_open 段对照）。

## 通用纪律（方向内全程有效）

- **分层**：全部留 `src/catalog/`（数据层，paleo_store 库）；
  新文件头三行 `// 层：数据`；`check_layering.py --strict` 绿。
- **行为保留红线**：SQLite schema/事务边界/查询语义逐条
  不变；「顺手改进」违规记 TODOS；schema_epoch 不动
 （零迁移）。
- **性能断言**：catalog_open/查询比率门不回退（tst_coldstart
  catalog_open 对照 R0）；禁绝对毫秒。
- **资源**：`./paleo-dev.ps1` 系；ctest 串行；全量构建后再
  ctest。
- **无人值守**：切分线自行定案记 ledger；与方向 92 的
  catalogRoles 协调（谁先合谁为准）。
- **ledger**：`.goal-loop-ledger-catalog-split.md`。
- **多轮 review（硬要求）**：每批 → 测试全绿 → diff 自审
  （API 等价/事务边界/schema 不动/TU 边界/性能门 五维）→
  修复 → 再 review，至少两轮零 High/Medium；Low 记 PR body。
- **提交**：原子提交，中文 conventional 前缀。

## Oracle 验收（逐条需验证证据）

1. 类零变更：两头文件公共区 diff 零变更；消费方零改动
  （rg 消费清单对拍——拆分前后一致）。
2. 体量：两主文件各 ≤900 行；新 TU 各 ≤700 行（wc 证据入
   ledger）。
3. 底座回归：tst_catalog 零改动通过×2 遍；mkprojectfixture
   （真实 SQLite 路径）绿；全量 ctest 两遍对照 R0 红集合
   diff 为空。
4. 性能门：catalog_open/查询比率对照 R0 无 >10% 劣化。
5. schema：schema_epoch 不动（diff 证据——零迁移）；
   事务边界语义逐条保留（journal 原子化测试零改动）。
6. layering 三档绿；构建无新警告；git diff --check 干净。
