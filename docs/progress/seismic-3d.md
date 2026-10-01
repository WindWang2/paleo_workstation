# wave/seismic-3d-viz — 三维可视化与交互剖切深化

> 分支 `wave/seismic-3d-viz`（base `master@2d33c5e`）。
> 方向：地震 3D 视图从「能显示」推进到「可交互解释」——体渲染传递函数、
> 任意斜剖面/栅栏剖面、层位面与井轨迹上图、切片动画扫掠导出。
> Goal Loop: `goal-loop-prompts/03-seismic-3d-viz.md`（Oracle 7 条为完成定义）。

## 轮 0-1 勘察结论（选型定案）

现状盘点（详见下），全部选型**沿用既有形态、不引入新框架**：

| 议题 | 现状 | 决策 |
|---|---|---|
| 渲染栈 | 手写 `Seismic3DViewportWidget`(QOpenGLWidget+GL3.3Core) + `SeismicSliceRenderer` + `VolumeFrameRenderer`；纹理 RGBA8 CPU 预烘焙（`Recolorize`/`colorizeSlice`），shader 仅 `uAlpha` | 沿用手写栈；TF 走 GPU 路线 |
| 传递函数 | 无（只有全局透明度 + 值域裁剪，CPU 重烘焙路径） | **GPU 1D LUT**：TF 启用时纹理改传「归一化索引+有效掩码」(GL_RG8)，fragment 采样 256×1 RGBA8 LUT (`GL_TEXTURE_1D`)；TF 修改只重传 256B LUT——零取数零重烘焙 |
| 任意剖面 | 渲染器 `UpdateLineSlice`（多段折线原生支持）+ `UpdateLineSection`（顶面路径线）**零调用方**；服务 `startSectionExtraction` 现成 | 视口加顶面拾取模式（射线→(il,xl)）：两点=斜剖面、N 点+回车=栅栏；预览线复用 `UpdateLineSection` |
| 层位面 | 服务层 `SeismicHorizonGrid`（D4.7 IDW 网格化）+ 会话/CSV 资产 | 新 `HorizonSurfaceRenderer`：IL×XL 规则网格、twtMs→y、NaN 控制点挖洞 |
| 井轨迹 | D3.4 仅垂直井位线+标志层十字；域层 `SectionWellInfo`(surface/bottom XY + tops XYZ) 齐备 | `Seismic3DWell` 扩展折线轨迹；井名标注 paintGL 后 QPainter 叠绘 |
| 动画 | 无 | QTimer 帧驱动 + 预取窗口 N=±4（`startSliceExtraction` 预热 `SgyDataCache`，`Hits()` 验证命中）；PNG 序列 `grabFramebuffer` |
| GL 护栏 | `tst_seismic_3d::openGLHeadlessRender` QSKIP 先例；offscreen 下 NVIDIA 4.6 真渲染可用（像素断言可行） | 新 GL 测试全部照抄 QSKIP 先例；异步回调 QPointer 守卫 |
| 层树联动 | catalog 实体(井 surfaceXY/测区 inline-xl 范围) + 角色(horizon/tops/…)；面板在 datapreviewtabs 内 | 3D 面板暴露 overlay 显隐 API；datapreviewtabs 接 QGIS 层树勾选信号 |

关键既有件（避免重复造）：
- 取数全异步：`SeismicTaskService::startSliceExtraction/startSectionExtraction/
  startVoxelWindow`（≤4 并发闸、协作取消、QPointer 回调守卫）。
- 面板缓存 `cachedSlices_[3]`（values 保真）——TF 值纹理重传的原料。
- `SgyDataCache::Hits()` 命中计数——扫掠命中率门的读数。

## 交付记录（按块追加）

| 块 | 提交 | 内容 | 测试 |
|---|---|---|---|
| 1 TF | `feat(seismic3d): 体渲染传递函数` | `Seismic3DTransferFunction`（停靠点→256×RGBA LUT）+ `Seismic3DTfEditorWidget`（拖/双击插/右键删）+ 渲染器 TF 分支（`sampler1D` LUT、值纹理 GL_RG8 索引+NaN 掩码）；面板 TF 开关/编辑对话框 + 堆叠层 values 缓存（TF 翻转重喂） | `tfModelBasics`/`tfEditorDragUpdatesAlpha`/`tfOffscreenRealtimePixels`（像素非均匀变化 + GL 零错误）/`panelTfZeroRefetch`（LUT 修改零取数） |
| 2 剖面 | `feat(seismic3d): 任意斜剖面/栅栏` | 视口顶面拾取（`glm::unProject` 窗口坐标射线→顶面交→测网格，吸附真实线号）；两点自动提交/多点回车·双击提交/Esc 取消；面板 `requestLineSection`（服务异步 + `BuildLineSection` 同步回落） | `lineSectionOffscreenPixels`（帷幕像素非空）/`viewportObliquePickCommits`/`panelLineSectionServicePath`（服务触发+贴入断言） |
| 3 层位井轨 | `feat(seismic3d): 层位面与井轨迹` | `HorizonSurfaceRenderer`（IDW 网格→面片、twt→彩虹谱逐顶点色、NaN 挖洞）；`Seismic3DWell.trajectory` 折线双描 + 井名标注 QPainter 叠绘；面板 setHorizons/显隐 API + `horizonVisibilityChanged`/`wellVisibilityChanged`（层树联动面）+「解释」overlay 菜单；datapreviewtabs 接解释会话拾取→网格化→上图 | `horizonSurfaceOffscreenPixels`/`wellTrajectoryVertexGrowth`（顶点精确计数）/`panelHorizonWellOverlayLinkage`（真实 gridPicks 域路径） |
| 4 动画 | `feat(seismic3d): 切片动画扫掠` | `startSweep(T/IL/XL, fps)` 回绕推进（IL/XL 走线号值表序）；前向预取 ±4（只暖 SgyDataCache、让路交互）；PNG 序列（grabFramebuffer，无 GL 回退导缓存帧）；UI 轴向/fps/播放/导出 | `sweepAnimationNonBlockingExport`（探针+暂停静止+PNG 一致）/`sweepCacheHitRatio`（≥0.8 门） |
| 5 护栏 | 同上 | GL 用例全走 `openGLHeadlessRender` QSKIP 先例；pending 全通道（切片/剖面/层位/TF）无 GL 暂存 + 析构干净；在途任务面板先亡走 QPointer 守卫 | `noGlTeardownClean` |

## 性能实测（Oracle 6；比率门，机器负载容差）

`sweepPerfBigFixture`（env `PALEO_SEISMIC_SWEEP_PERF` 门控；220MB 生产形状
夹具 53261 道 × 1024 采样，`make_segy_fixture.py --mb 220` 同款）：

| 指标 | 实测（本机 RTX 3080 / offscreen） | 门 |
|---|---|---|
| 体加载（索引热） | 162 ms（冷 909 ms） | —（仅记录） |
| 1s 扫掠帧推进 @30fps 目标 | 32 帧 = **107%** | ≥60% |
| 事件循环探针（1ms 档 QTimer） | 98 跳/s | ≥50（Qt 粗粒度合并下健康循环 ~10ms/跳；阻塞趋零） |
| 缓存命中 | 119 次 = **3.72/帧**（帧+预取窗口全命中口径） | mini 门 ≥0.8（实测 5.0） |

966MB 真工区体不在本环境——按 oracle「比率门而非墙钟」原则，以上比率门
对任意体量成立（取数异步 + 预取暖缓存与体量无关）；真机复测步骤同
`docs/progress/seismic.md` 的 966MB 手动验收（设 `PALEO_SEISMIC_SWEEP_PERF`
指向真体即可复跑本用例）。

## 已知边界 / 递延

- 井数据→3D 的应用侧自动接线（wellhead XY→测网格换算 + 井轨迹加载）未做
  ——面板 `setWells` API 已就绪（D3.4 起即有，本波扩展轨迹），等待井震
  解释页统一接线（与 D5 井震页合流时做）。
- 层树（QgsLayerTreeView）与 3D overlay 的双向同步：本波交付信号面
  （`horizonVisibilityChanged` 等）+ 面板「解释」菜单；解释层位尚非
  QgsMapLayer，入层树需先过 LayerManifest 声明（TODOS 登记）。
- TF 值纹理编码按切片 absMax 逐片归一（与既有 colormap 语义一致）；
  全局固定增益的 TF 会话持久化递延。
