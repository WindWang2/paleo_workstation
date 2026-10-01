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

（待块交付后追加）

## 性能实测（Oracle 6）

（待测）
