# wave/seismic-engine-deep — 进度与接缝登记

> 分支 `wave/seismic-engine-deep`（base `origin/master@027524d`）。
> 主管线 QuickOpen 秒开 / 瓦片渐进时间片 / .sf3p 分页通道 / 渐进 LOD /
> ReadVoxelWindow / 转码 UX / 性能闸门 / 引擎矩阵测试。
> TODOS.md 不动（按交接约定），合并时统一对账。

## 交付一览（按提交）

| 提交 | 内容 |
|---|---|
| P5+引擎测试 | vendor 补丁 P5（生产道字回退）；tst_seismic_engine 引擎矩阵 15 用例 |
| 服务层 | SeismicTaskService 新通道（QuickOpen/Paged 转码/瓦片/体素/LOD/后端探测/invalidate）+ 服务级测试 4 用例 |
| 析构竞态修复 | 数据集条目注册表 shared_ptr 共享所有权（worker 排队锁时服务析构的 UB） |
| UI | 两段式秒开、瓦片贴图、3D 渐进 LOD、转码双通道进度/取消/状态 |
| 性能闸门 | make_segy_fixture.py --mb + tst_seismic_perf（220MB 生产形状夹具） |

## 语义决策（合并评审重点）

1. **Auto 永不自动升级 .sf3p（引擎语义，UI 层显式选择）**：`Dataset::Open(Backend::Auto)`
   只发现 `<sgy>.sf3c.meta` 伴生；`.sf3p` 需显式路径或 `Backend::Paged`。Paleo 侧策略：
   时间切片页检测到 `<sgy>.sf3p` 存在（或转码完成回调）即显式启用 paged 通道——
   这是应用层决策，不改引擎 Auto 语义。文档化的语义差异：
   - `.sf3c`：Auto 自动升级，切片/剖面随机访问；
   - `.sf3p`：显式通道，LOD 映射 + 瓦片渐进；两通道可并存（.sf3c 走 Auto，
     .sf3p 走显式）。
2. **瓦片 vs 整图（ReadTimeSliceTiled vs ReadCachedTimeSlice/整图）取舍**：
   paged 冷缓存首见走瓦片流（引擎焦点优先，中心瓦片先上屏、边角后补）；
   直读后端与热缓存路径一次性整图更省（少一次逐瓦片回调与贴图）。
   `ReadCachedTimeSlice` 需 `timeCachePath`（临时精确时间缓存）——本 wave 未
   启用该参数，路径上等价于 miss；后续如启用再评估预算。
3. **sf3c 转码取消的语义**：引擎按设计把续跑元数据留在原位（meta 可能存在但
   分片不全）；消费侧安全网是 SeismicTaskService 的「引擎读失败→volume 直读
   回落」。UI 只在任务成功后宣告「工作区已就绪」；完整重跑后 Auto 稳定升级。
4. **P5 生产道字约定**：INLINE@189/CROSSLINE@193 两字恒 0 的工区体（966MB 真实
   体、synthetic_4x5 夹具）回退 field record@8 + CDP@20 取道字（见
   `vendor/sbm/PATCHES.md` P5）。上游规范文件逐位不变。

## 共享文件接缝登记（合并时注意）

| 文件 | 接缝 | 说明 |
|---|---|---|
| `src/ui/datapreview/datapreviewtabs.h` | 成员追加 | `m_tiledSignalService/m_tiledCanvas/m_tiledSample`（瓦片信号路由，地震页专用）；additive-only |
| `src/ui/datapreview/datapreviewtabs.cpp` | include 追加 | `services/paleotaskservice.h`（转码进度/取消）、`<QProgressBar>` |
| 同上 | 地震段改写 | 转码按钮区（~L2609-2830）、体加载两段式（~L2780-2960）、时间片请求（~L2960-3060）——共享区（LAS/测区地图/PDF/通用 tab 框架、attachDoc、openSeismicLine 以北）零改动 |
| `tests/tst_datapreview.cpp` | 用例调整 | `seismicDecodeRunsThroughTaskService`：解码任务按「解码剖面」标题定位（工区打开新增秒开/体加载任务，序号假设失效） |
| `services/previewdoc.{h,cpp}` | **未触碰** | 新引擎信号（瓦片）走 SeismicTaskService 直连（与既有时间片请求同模式），previewdoc 接缝本 wave 无需追加 |
| `paleomainwindow*` | **无触点需求** | 秒开/瓦片/LOD/转码全部封在地震资产页内，壳零改动 |

## 966MB 真工区手动验收步骤

前置：真工区 SEG-Y（966MB，ordinal 道字约定）置入工程并导入。

1. **秒开**：数据页打开该地震资产 → 2D 页在 1-2 秒内出现中央测线真振幅缩略
   （quickInfo 行显示「秒开 N ms · IL … · 预览 IL …」）；随后解码任务把所选
   测线替换为全分辨率剖面。
2. **后台索引**：任务页出现「加载地震体」任务，完成后切「三维立体 (3D)」/「水平
   时间切片」页即刻可用（无 UI 卡顿——同步 Load 已退役）。
3. **sf3c 通道**：时间片工具条「转码工作区 (.sf3c)」→ 确认 → 进度条有百分比、
   「取消」可中断；中断后再点可续跑（跳过已写分片）；完成后后端状态标签变
   「后端：.sf3c 工作区（已热切换）」，切片/任意剖面明显提速。
4. **sf3p 通道**：「转码分页工作区 (.sf3p)」→ 确认 → 三阶段（L0/L1/L2）进度；
   完成后状态标签「后端：.sf3p 分页工作区 · L…（已热切换）」，时间切片走瓦片
   渐进（中心先出、边角后补），3D 页工具条出现「LOD …」质量标签。
5. **渐进 LOD**：3D 页拖动 inline/xline 滑杆——按下期间质量标签切到粗层
   （L2），松手静止 ~0.35s 后升回 L0 且三槽切片自动重取为全分辨率。
6. **取消无半成品**：任一转码取消后，Auto 后端不应把未完成工作区当可用——
   读失败经服务回落直读；`.sf3p` 成品文件在取消时不发布（只有 `.partial`）。
7. **二次打开**：关闭重开资产/应用——`.sgyidx` 伴生 + 工作区文件使首屏与
   切片进一步提速（QuickOpen 仍先出，体加载秒级完成）。

## 已知边界 / 递延

- `ReadCachedTimeSlice` 的 `timeCachePath` 精确时间缓存未启用（语义决策 #2）。
- paged 通道的瓦片时间片要求 L0 激活层（引擎契约）；LOD 粗层激活时时间切片
  页自动走 3D 面板同源的切片提取（不走瓦片）。
- 瓦片投递目标用 QPointer（关停期极窄竞态，只影响瓦片丢弃，不崩溃不挂死）。
- vendor 未动 `Engine/RoiWorkspaceBuilder.cpp`（上游实验路径，SDK 链路不经过）。
