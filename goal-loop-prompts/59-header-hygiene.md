# Goal-Loop 方向 59：头文件卫生——重头出闸收口（previewdoc 扇出 + QWidget 前置声明）

## 背景（实测事实，勿再勘察；行号为 2026-10-06 master `f130fb2` 口径）

方向 49（arch-closure）账本 `:128-136` 记档的两条头文件卫生债，
均无方向覆盖：

1. **previewdoc 重头扇出**：`src/services/previewdoc.h:15-19` 把
   `../domain/sectiontrace.h`、`../domain/wellrecords.h`、
   `../domain/wellcompositemodel.h`、`../io/lasdoc.h`、
   `../io/lasparser.h` 五个重头直接带进公共头——`src/ui` 有
   **26 个 TU（另 4 个中间头）** include previewdoc.h，全部
   被迫重编 io/domain 契约头（变更一次 lasparser.h，26 个
   UI TU 重编）。
   `lasparser.h` 更进一步被 `dlisparser.h:8`/`lisparser.h:8`/
   `welllogread.h:7` 传递扩散（「井曲线文档共用契约」注释自证）。
2. **qgisprocessingservice.h 的 QWidget**：`src/qgis/
   qgisprocessingservice.h:8` 直接 `#include <QWidget>`——
   前向声明即可（成员只持指针/返回值时）；这使 **11 个 workflow
   TU**（constraintfactorjobs 拆分六 TU + compositionworkflow/
   constraintworkflow/mappingworkbench/predictionworkflow/
   workflows）因直接 include 此头消费 QtWidgets（方向 49
   时点为 6 处，方向 57 拆分后增殖到 11 处）——消掉后黄牌
   清零。

配套：方向 49 已给 `check_layering.py` 加了 `--transitive`
opt-in 档（BFS 解析 include 图）——本方向修完后用它出证。

## 环境接线（Windows 本机实测口径）

```powershell
cd C:\Users\wangj.KEVIN\projects\paleo_workstation
git fetch origin
git worktree add .worktrees\header-hygiene -b goal/header-hygiene-20261007 origin/master
cd .worktrees\header-hygiene
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo `
  -DQGIS_PREFIX=C:/Users/wangj.KEVIN/paleo-qgis-prefix `
  -DCMAKE_PREFIX_PATH="C:/deps/Qt/6.8.0/msvc2022_64;C:/Users/wangj.KEVIN/paleo-qgis-deps/Library;C:/deps/qscintilla-install" `
  -DQSCINTILLA_PREFIX=C:/deps/qscintilla-install
cmake --build build -j8
```

坑位同方向 58（PATH 顺序/基线红对照）。

## 目标形态（建议按序）

1. **R0 扇出测量**：`python tools/check_layering.py --transitive`
   现状输出记档（哪些 ui TU 经哪些头吃到 io/domain 重头）；
   touch lasparser.h 后 ninja 重编 TU 计数记为基线。
3. **previewdoc 瘦身**：出参类型改前向声明 + PIMPL 或出参拆
   细头（`lasdoc.h` 本就是纯类型门面——契约类型若已在 lasdoc，
   lasparser include 可降级为仅实现侧）；26 个直接消费 TU 编
   译面验证。
4. **井曲线契约头收口**：dlisparser/lisparser/welllogread 对
   lasparser.h 的 include 需求逐个核实——只需 LasHeaderInfo/
   LasCurve 契约的话改 include lasdoc.h（纯门面）；lasparser.h
   自身只留解析入口。**另验证 qgisprocessingservice.h 的
   QWidget 消除后，11 个 workflow TU 的传递黄牌清零**
   （`--transitive` 实测对拍）。
5. **护栏**：check_layering.py `--transitive` 档升级——「公共头
   出闸重头」检测（io/algorithms 头出现在非实现 TU 的传递闭包
   即黄牌），selftest 夹具补正反例。
6. **编译面证据**：touch lasparser.h 后重编 TU 计数对比基线
  （预期从 ~30+ 大幅下降）；全量构建时间对比记档（非门，仅记录）。

## 通用纪律（方向内全程有效）

- **分层**：只动头文件与 include 面，不改任何行为；`check_layering.py
  --strict` 绿（含新护栏档）。
- **行为红线**：零行为变更——纯编译面手术；任何「顺手改」违规。
- **资源**：`-j8`；ctest 串行。
- **无人值守**：拆头方案自行定案记 ledger。
- **性能断言**：编译面用 TU 计数对比（禁绝对墙钟做门）；全量
  构建时长仅记录不设门。
- **ledger**：`.goal-loop-ledger-header-hygiene.md`。
- **多轮 review（硬要求）**：每批 → 测试全绿 → diff 自审
  （零行为变更/include 正确性/护栏有效性/自测夹具/无死代码
  五维）→ 修复 → 再 review，至少两轮零 High/Medium；Low 记
  PR body。
- **提交**：原子提交，中文 conventional 前缀。

## Oracle 验收（逐条需验证证据）

1. previewdoc.h 的 io/domain 重 include 清零（头文件 rg 证据：
   只剩 Qt 标准头 + 前向声明）；26 个直接消费 TU 编译通过。
2. 井曲线三头（dlis/lis/welllogread）不再传递带 lasparser.h
  （rg 证据）。
3. qgisprocessingservice.h 无 `#include <QWidget>`（rg）；
   `--transitive` 黄牌较 R0 基线清零或逐条列因。
4. 编译面：touch lasparser.h 重编 TU 计数 ≤ 基线的 30%（对比
   数字入 ledger）。
5. 护栏：新检测档对故意造的违规夹具能打红（mutation 验证）；
   既有三档 + selftest 全绿。
6. 全量 ctest 对照 R0 红集合 diff 为空。
