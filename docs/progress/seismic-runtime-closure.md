# progress — goal/seismic-runtime-closure（地震运行时与剖面收口）

> Work Package 1 · 2026-09-30 · 分支 `goal/seismic-runtime-closure-20260930-215353`

## 0. Phase 0 底账

- **BASE_SHA**: `ac882cfcfb93f606fdcc6c8813b71380759f86bd`（origin/master 2026-09-30，
  PR #64 wave/deepen-perf 合并后）。
- **open PR**：0。**最近 merged**：#64 deepen-perf / #63 ci-stabilize / #62
  seismic-chain-deep / #61 sections-time-depth / #60 io-perf-cache / #59 preview-map-canvas /
  #58 data-page-operations / #57 wellcomposite-deep / #56 prediction-facies-editing /
  #55 ui-deep-polish / #54 mapping-workflows / #53 devex-infra / #52 ux-polish /
  #51 mapping-editing / #50 seismic-engine-deep。
  review 要点（对本 lane 有影响的）：#62 建立 SECTION/3D/INTERPRETATION 三后端语义矩阵
  与 D6 并发闸/取消链；#64 A 轨把拖动 supersede、瓦片顶替、体窗合并收进服务层，
  并把「剖面 dock 裸 QThreadPool」记为 lead 裁决点（本包承接）。
- **Issue 对账**（open issues 以当前 master 逐条复核）：
  - #25 (P1 datapreview SegyReader 并发竞态)：`m_segyReaders`+池线程直调面已随服务化
    迁移移除 → **already-fixed-on-master**（不重复实现）。
  - #42-1 (P3 SegyReader 静默丢失败道)：`readInline/readCrossline` 已有逐线尺寸差校验
    +明确错误文案（previewdoc 消费侧如实上浮）；无错误通道的 `traces()` 无生产调用方 →
    **already-fixed-on-master**（口径记档）。
  - 其余 open issues（#26-#47）主体属 WP2（catalog 写路径/落盘纪律）、WP3（编图）、
    WP4（CMake/token/CI 架构）域，本包不越界。
- **远程分支重叠矩阵**：全部 `wave/*`、`codex/prediction-facies-editing`、
  `codex/sections-time-depth` ahead=0（历史分支）。`codex/simplify-duplication`
  ahead 1 / behind 18：改动集中在 metadata/mapping/maptools 通用去重 + 少量
  linkage/selectioncontext——与本 lane 无必须重叠，**记账不 cherry-pick**。
- **基线测试**：全量 ctest 129 项 = 124 直绿 + 5 项逐条归因（onnx×2 环境注入缺陷
  → 本包修复后绿；seismic perf/baseline×2 首代夹具并行竞态 → 夹具落盘后复跑绿；
  correlation_full 共享机负载计时门假红 → 单跑绿，两遍）。
- **基线构建环境特记**（本机 CachyOS 无 QGIS/无 sudo/deb 闭包脚本要 apt）：
  按 `vendor/deb-closure.lock` 直链下载 863 debs（SHA256 全验；libevent 0.1 镜像
  失链以同 ABI 0.2 补位）+ ar/tar 复刻 `dpkg-deb -x` 解包；补 Debian pool 运行库
  nettle8/assuan9/gpg-error0/selinux1；`srs.db` 以 `srs-template.db` 生成（deb 无
  postinst 环境）；链接 `-rpath-link` 三目录 + `CMAKE_BUILD_RPATH`。构建类型
  RelWithDebInfo（仓库 `paleo-dev` 惯例；Debug 会假性击穿性能预算门，A/B 需同构）。
- **不做什么**：不新增地震文件格式/远程服务；不动 catalog 写路径（WP2）、编图算法
  （WP3）、generic CMake 架构（WP4）；不 merge `codex/simplify-duplication`；
  不改 UI 信息架构；不动 vendor/sbm upstream 补丁面。

## 1. 交付项

### 1a. 剖面 dock IL/XL/Time + 卷帘 B 图迁 SeismicTaskService（TODOS P2 收口）

`extractSliceAsync` / `updateCompareSlice`（seismicsectiondockwidget）从裸
`QThreadPool::globalInstance()` 直调 `SgyVolume::ExtractSlice` 迁到既有
`SeismicTaskService::startSliceExtraction` 通道：

| 面 | 改前（裸 QThreadPool） | 改后（服务通道） |
|---|---|---|
| 并发 | 无闸（globalInstance 无上限，与全 app 抢线程） | ≤4 地震并发闸（D6.4） |
| 取消 | progressCb 恒返 true，不可取消 | 新请求 requestCancel 顶替（逐线检查点退出，被顶替读取不再占闸） |
| 缓存 | 无 | 切片 LRU（与数据页/3D 共享；换线回头即命中） |
| 后端 | 仅直读 | 引擎 Auto（sf3c 工作区自动随机访问）→ 失败回落直读 |
| 回调安全 | 池线程裸 `this` 捕获（progressCb 调用点与完成 invokeMethod 两处 UAF 窗口） | QPointer 守卫 + 世代号 + 请求号三重判定 |
| 假进度 | 恒 true 的 progressCb + 手写百分比排队 | 任务字节进度（reportBytes）驱动 |
| 顶替语义 | 单待发槽（要等整条线读完才发下一请求） | 立即顶替；cancelled ≠ failed，陈旧结果静默丢弃不弹错 |

**顺带修复的既有反向缺陷**：任意线提取在途时切回 IL/XL/Time 模式，迟到的任意线
结果会覆盖切片显示——原 `extractSliceAsync` 从不 bump `m_generation` 也不取消
`m_extraction`。现在切片/任意线/切体三方互顶替时双向取消对端在途任务并推进世代号。

**卷帘 B 图失败语义**：原裸路径静默吞 err（帘面空白无解释）；现失败/空线给原因
文案（诚实失败契约）。

**实现注记**：
- 守卫用「请求计数器」而非「任务身份指针」：lambda 按值捕获 `QPointer<PaleoTask>`
  在其自身初始化前是 UB（实测：守卫永假→回调全被丢弃）。`m_sliceRequest` /
  `m_compareRequest` 单调计数 + 捕获快照，语义等价且无时序陷阱。
- 服务立即失败路径（体未完成索引等）同步回调：顶替时先 `m_sliceTask.clear()`
  再启新，身份/请求守卫以「计数已推进」放行如实报错。
- 直读例外保留：`showWellSideTrace`（D5.6 井旁道模态小图，同步毫秒级、带诚实
  错误框）；3D/数据页的 `ExtractSlice` 直读点均为「无服务实例时的同步回退」
  （测试路径），主路径全走服务。

### 1b. tst_onnx 环境注入修复

`ENVIRONMENT` 整体 `LD_LIBRARY_PATH=...` 覆盖外部注入值（deb 闭包路 paleo-dev
注入的 vendored QGIS/Qt 目录被吞）→ 间接依赖落系统 Qt 产生符号版本冲突。改
`ENVIRONMENT_MODIFICATION` 的 `path_list_prepend`。CMakeLists 自身在
`paleo_test_sandbox` 的注释里已警告过该坑，此为存量违例收口。

### 1c. 测试基建归因（不改阈值）

- `tst_seismic_perf`/`tst_seismic_baseline` 首次运行互相竞态：两测试共享
  `${CMAKE_CURRENT_BINARY_DIR}/seismic_perf/perf_big.sgy`，-j4 并行时各自生成
  220MB 夹具（守卫只看大小）→ 后写者使先开者的索引期 mtime 校验失败。夹具落盘
  后复跑稳定绿；本地口径=首轮先串行跑这两个再全量。
- `tst_correlation_full` 计时门在共享机负载 >16 时假红（兄弟 worktree 并行构建），
  单跑两遍绿——与 wave 前科一致，不改阈值。

## 2. 验证底账

- `tst_seismic_sectionui` 23/23（14 既有用例 + 7 个新治理用例 + init/cleanup）×3 遍全绿：
  服务通道/LRU 命中（hits 计数）/闸饱和下顶替取消（Cancelled≥1、Succeeded=1、
  无失败信号）/销毁存活/切体丢弃陈旧结果（traceCount 归属判定）/换线回头命中/
  卷帘邻线跟随/任意线迟到结果守卫。
- 地震邻域套件（`ctest -R 'seismic|section|welltie|layering|correlation|preview'`）
  全绿（correlation_full/previewmap_perf 负载假红单跑复核绿）。
- 踩坑记录：**单目标构建后跑 ctest 会命中陈旧链接二进制**（paleo_ui 重编但未
  relink 的测试仍带旧缺陷）——本包流程定为全量 ninja 后再 ctest。

## 3. 性能 A/B（同机 BASE_SHA 对照）

对照构建：`pw-seismic-baseline` worktree @ BASE_SHA，同 vendor prefix、同
RelWithDebInfo、同 rpath 配置；两侧串行复跑两次取观测。负载 10–20（兄弟
worktree 并行构建），绝对值偏保守但两侧同条件。

| 门（预算） | BASE | HEAD（本分支） | 判定 |
|---|---|---|---|
| QuickOpen 首屏 (<5000ms) | 6 ms | 6 ms | 持平 |
| Open+首 IL 切片 (<5000ms) | 9+7 / 9+7 ms | 9+8 / 10+7 ms | 持平（±1ms 噪声） |
| 时间切片 (<8000ms) | 11 / 14 ms | 8 / 9 ms | 略优 |
| 任意线 (<5000ms) | 13 ms | 12–13 ms | 持平 |
| 切片 miss (<500ms) | 8.0 ms | 7.0 ms | 持平 |
| 切片 hit (<50ms) | 7.0 ms | 7.0 ms | 持平 |
| 3D fps LOD (≥15) | 15,000 | 17,143 | 略优 |
| 3D fps stack16 (≥15) | 9,231 | 13,333 | 略优 |
| 并发闸 max | 4 | 4 | 一致 |

**结论：无任何门 >10% 劣化**；时间片/3D 帧率的差值在同机噪声带内反复复测
方向一致偏优（dock 直读路径迁移后与数据页/3D 共享 LRU，暖读面变大）。
tst_seismic_perf / tst_seismic_budgets 两侧 6/6、9/9 全过，阈值未动。

## 4. 独立 review 结论与修复（reviewer subagent，未参与实现）

- **P1（已修）**：`extractSliceAsync` 的 `++m_generation` 位于去抖判定之前——重复请求
  （本意「什么都不做」）会毒化它要去重的在途任务（回调被世代号丢弃→进度条不收、
  无失败信号、画布停旧）；`onSectionModeChanged(3)` 同病（只推世代号不清切片任务，
  3→0 回切去抖命中已毒化请求→静默空白）。修复：世代号推进移到去抖之后；模式 3
  分支与 setVolume 同构（取消摘牌切片/卷帘 + 去抖键作废）。
- **P2（已修）**：进度连接无请求身份——被顶替任务的 `changed()` 连接存活到终态，
  会把旧线百分比写进新请求的进度条且每请求积一条死连接。修复：进度 lambda 捕获
  请求号，`m_sliceRequest != request` 即 no-op。
- **P3（已修）**：①dock 析构不取消在途任务（注入共享服务时占闸到读完）——析构体
  补三任务 requestCancel；②任意线守卫测试用网外点+空折线（断言弱）——改测网内
  真实线号 + 非空 mapPolyline 使 `hasRoute()` 判定有效；③文档计数 8→7 修正。
- **P3（记档不修）**：dock 切片现与 3D/数据页共享 PaleoTaskService 池与 ≤4 闸
  （BASE 用独立 globalInstance 池）——被顶替任务入池即退出 + 在途逐线检查点退出，
  放大有限；共享闸是有意收敛（三路径同一治理域），记入 SECTION §6。
- 确认干净面：回调全主线程投递（守卫态无数据竞争）；同步失败路径与守卫次序
  兼容；完成回调→updateCompareSlice 递归有界；QPointer 生命周期全覆盖；
  Windows 无敌面；无新增违规 include。

## 5. 递延 / 不属于本包

- perf/baseline 共享夹具的**首代竞态根治**（夹具改名或生成加文件锁）：属测试基建
  域（WP4 邻域），本包以运行口径规避并记档。
- `codex/simplify-duplication` 的通用去重：跨 lane seam，由 WP4 域裁决。
- SEG-Y 字节表编辑器、TimePlaneCache 等远期 TODO：不属收敛范围。
