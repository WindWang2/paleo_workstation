# goal-loop ledger：方向 59 头文件卫生（header-hygiene-20261007）

基线：origin/master `f130fb2`；worktree `.worktrees/header-hygiene`。

## R0 基线（2026-10-06 实测，master 口径）

### 三档闸门 + 既有诊断档

- `check_layering.py --strict`：绿（无层违规；baseline 已归零）。
- `check_layering.py --transitive`（QGIS_PREFIX=C:/Users/wangj.KEVIN/paleo-qgis-prefix）：
  - 功能层黄牌 **11 处**，全部 `qgis/qgisprocessingservice.h -> <QWidget>@8`：
    workflow/{compositionworkflow, constraintfactorjobs(×6 拆分 TU),
    constraintworkflow, mappingworkbench, predictionworkflow, workflows}.cpp
  - 视图层 QtWidgets 边记录 6 处（视图不禁 QtWidgets，仅记录）。

### 扇出分析（include 闭包实测，repo 边 + 存在性校验；白名单 io/lasdoc.h 剔除）

- `services/previewdoc.h` 消费面：**36 文件**（31 直接 include，5 经中间头）。
  其中 ui TU 27 个 + ui 中间头 6 个（datapreviewtabs.h /
  datapreviewtabs_internal.h / datapage.h / pagepanels.h / entitypanel.h /
  dataopsimportqueue.h）+ services/previewdoc.cpp。（方向书「26 TU + 4 中间头」
  为方向 49 时点口径；本方向实测略多，方法学：闭包含 previewdoc.h 的
  .cpp/.h 全量。）
- io/lasparser.h 公共头扇出面（修前）：
  - `services/previewdoc.h:19`（→ 27 ui TU）
  - `io/lascache.h:11`（→ services/crossplotsamples.cpp / crossplotsources.cpp 等）
  - `io/dlisparser.h:8` / `io/lisparser.h:8` / `io/welllogread.h:7`
    （→ services/welllogset.cpp 等非 io TU 传递携带）
  - 契约类型 LasHeaderInfo/LasIssue 定义在 lasparser.h（不在 lasdoc.h），
    收口需先迁类型后换 include。
- 跨模块传递携带 io/algorithms 重头的 TU 全量 ~85 处（140 含模块内正常传递；
  其中 algorithms/* 经 workflow 头进 ui/app 的大面为**方向 49 已递延债面**
  （ledger 记档 38 处类），本方向不动，只收 lasparser/qgisprocessingservice
  两族）。

### 编译面（touch src/io/lasparser.h → 重编 TU 计数）

| 项 | 修前（R0） | 修后 | 门 |
|---|---|---|---|
| 解析闭包重编 TU（src+tests+tools 全 .cpp） | **87** | **21**（24.1%） | ≤ 30% ✓ |
| ninja 实测重编 TU（worktree settle→touch） | 主 checkout deps 失效不可测（见下） | 34（死 include 清仓前）→ 21（清仓后解析口径） | — |

方法学注记：
- R0=87 与修后=21 均为**解析式 include 闭包**精确计数（含 tests/tools；
  方向书「~30+」是方向 49 只扫 src/ui 的口径，实际全库面更大）。
- 修后 ninja 实测（worktree，deps 正常）：34 TU（= 解析口径逐一对上：
  io 12 + services 2 + workflow 3 + selfcheck 1 + tools 1 + tests 15）；
  死 include 清仓（13 处）后解析口径 21，与 ninja 集合一致（差集 =
  清仓文件）。
- 主 checkout build 目录的 ninja 头依赖失效（deps=gcc 解析 cl 本地化
  输出，touch 仅 2 个 mocs 重编——该 build 目录的历史遗留问题，非本
  方向引入；worktree 全新 configure 后 deps 正常，实测有效）。

## Oracle 验收取证（逐条）

| # | 验收项 | 证据 | 判定 |
|---|---|---|---|
| 1 | previewdoc.h io/domain 重 include 清零 | rg：头内 include 仅 Qt 标准头 ×10 + `../io/lasdoc.h`（白名单纯类型门面，moc QList<LasCurve> 墙所需，见技术发现 1）；lasparser.h/sectiontrace/wellrecords/wellcompositemodel 全部清零。消费 TU 编译：全树构建绿 ×3（含 27 ui 直接消费 TU + tst_previewdoc） | ✓ |
| 2 | 井曲线三头不再传递 lasparser.h | rg：dlisparser.h/lisparser.h/welllogread.h（+lascache.h）均 include lasdoc.h；全仓 lasparser.h 剩余 includer 全部是真调用方（LasParser:: 用户）；--transitive 修复态 lasparser 出闸边 0 | ✓ |
| 3 | qgisprocessingservice.h 无 QWidget；黄牌清零 | rg 无 `#include <QWidget>`（前向声明）；--transitive 功能层 QtWidgets 黄牌 **11→0**。剩余重头出闸 47 边逐条列因（--transitive 输出全量）：algorithms 经 workflow/services 公共头（workflows.h→25、propertymodelworkflow→16、faciesmappingworkflow→16 等，方向 49 递延债面）+ io cache 家族（lascache→cachecore/inflight/lrucache→5）+ seismicmapping.h→horizonbinner/timedeptool/wellfileparsers（→8）+ segyreader→segyindexstore（→3）；**无一条由本方向引入** | ✓ |
| 4 | touch lasparser.h 重编 TU ≤ 基线 30% | 解析闭包 87→21（24.1%）；ninja 实测 21（与解析逐一对上） | ✓ |
| 5 | 护栏 mutation 打红 + 三档/selftest 全绿 | mutation：previewdoc.h 塞回 lasparser.h → `黄牌(重头出闸) io/lasparser.h 经 services/previewdoc.h 出闸 → 29 TU`（还原后消失）；--strict 绿 ×多次；--selftest 绿（+3 重头出闸夹具）；ctest layering 三档在 ctest 全量内 | ✓ |
| 6 | 全量 ctest 红集合 diff 为空 | 三层对拍：全量 228 vs 229（唯一差异 tst_singlefactor_perf 复跑双侧同红=计时抖动）；方向相关子集 27=27 diff 空；tst_previewdoc 函数级逐行一致；layering 三档双侧绿 | ✓ |

## ctest 对照（Windows 本机，323 项；既有红基线口径）

本机测试环境两个已知干扰（非本方向引入，双侧同配方同现）：
- Qt 双栈冲突：exes 链官方 Qt 6.8.0，qgis_core.dll 链 deps Qt——官方 Qt
  优先配方下 QGIS 依赖测试 0xc0000139 启动红；
- tst_* 内 QTemporaryDir 系统性失效（独立探针双向验证 Qt 本身正常，
  疑进程级环境；纯 include 手术不可能影响，双侧一致红）。

对拍三层证据（配方完全一致：同 PATH 序/QGIS_PREFIX_PATH/offscreen）：

| 层 | main（R0 原树） | worktree（修后） | diff |
|---|---|---|---|
| 全量 323 项（官方 Qt 优先配方，串行） | 红 228 | 红 229 | 仅 `tst_singlefactor_perf`——复跑**双侧同红**（229s/217s 双超时，计时阈值抖动，singlefactor/algorithms 本方向零改动） |
| 方向相关 29 项子集（deps Qt 优先配方，测试真正执行） | 红 27 | 红 27 | **空** |
| tst_previewdoc 函数级（qtest 输出逐行） | 5 PASS/8 FAIL | 5 PASS/8 FAIL | **逐行一致**（仅耗时 ms 差） |

绿面补充：layering/layering_strict/layering_selftest 三档 ctest 两侧
全绿（含新增 3 重头出闸夹具）。

结论：**红集合 diff 为空**（唯一差异项经复跑定性为计时抖动，双侧同红，
非本方向所致）。

## 全量构建时长（仅记录，非门）

- R0（worktree 全新 configure + 全量 1752 步，-j8）：约 45 分钟（观察值）。
- 修后增量重建：383 → 271 → 244 → 250 步（各轮 commit 后），单轮 ≤8 分钟。
- 观察口径：无净增编译耗时；新文件 1 个（sectiondoc.h，仅 3 个 TU 消费）。

## 迭代账

| 轮 | 改动 | 验证 | 判定 |
|---|---|---|---|
| 1 | R0 记档（--strict/--transitive/扇出分析/消费矩阵） | 输出见上 | 通过 |
| 2 | 契约收口（commit 1）：LasHeaderInfo/LasIssue 迁 lasdoc.h；lasparser.h 只留入口；dlis/lis/welllogread/lascache 换 include；三 .cpp 补直接 | 全量构建 1752/1752 绿 | 通过 |
| 3 | qgisprocessingservice 去 QWidget + 出线（commit 3） | --transitive 功能层 QtWidgets 黄牌 11→0（源级） | 通过 |
| 4 | 护栏升级 + selftest 3 夹具（commit 4） | --selftest 绿；--strict 绿 | 通过 |
| 5 | previewdoc 瘦身 + sectiondoc 拆头（commit 2）：首建撞 moc 墙 → lasdoc.h 保留定案 → PIMPL 回退 | 重建绿（271 步） | 通过（迭代一次） |
| 6 | tst_previewdoc 补 wellrecords/sectiondoc include | 重建绿 | 通过 |
| 7 | 死 include 清仓 13 处（commit 5） | 闭包 34→21；ninja 实测=解析=21；重建绿 | 通过 |
| 8 | R0 测量修正：主 checkout ninja deps 失效（历史遗留）→ R0 改解析闭包口径 87；worktree 侧 ninja 实测双向吻合 | 数字见编译面表 | 通过 |

## 关键技术发现（记档供 review）

1. **moc metatype 墙**：`qt_incomplete_metaTypeArray` 对信号参数
   `QList<LasCurve>`（不完全值类型容器）实例化 QDebug operator<< 时触发
   `__is_base_of<QList<T>, T>`（MSVC C2139，硬错非 SFINAE）。裸前向声明
   类型（SectionDoc const&）可过、QList<不完全> 不可过 ⇒ **lasdoc.h
   （白名单纯类型门面）必须保留在 previewdoc.h**。方向书 oracle 1 的
   证据形态「只剩 Qt 标准头 + 前向声明」按此修正为「Qt 标准头 +
   lasdoc.h（白名单门面）+ 前向声明」——重头（lasparser/domain 三头）
   清零的实质达成；lasdoc.h 即方向书认可的「纯类型门面」本身。
2. **QHash 值成员需完整类型**（Qt 6.8 类体 static_assert
   is_nothrow_destructible_v<T>）——若走全前向声明路线则 LAS 私有态需
   PIMPL；随 lasdoc.h 保留，LasDoc 完整，PIMPL 不再必要（已回退，
   previewdoc.cpp 净 diff = 3 行 include）。
3. 主 checkout R0 touch 测量被两事污染：首次直跑 ninja 无 MSVC env
   （cstdint 找不到）+ 并发 cmake re-glob 撞锁卡死。修正流程：单持有者
   串行收敛构建 → 确认 no-work → touch → 计数 → 恢复 mtime。
