# Goal Loop — goal/seismic-3d-viz：3D 可视化与交互剖切深化（新功能）

你接手一个自治迭代循环。方向：**把地震 3D 视图从「能显示」推进到「可交互解释」**——
体渲染传递函数、任意剖切/栅栏剖面、层位面与井轨迹上图、切片动画导出。

## 背景事实

- 现有：`tst_seismic_3d`/`tst_seismic_3dui`/`tst_seismic_section`/`tst_seismic_sectionui`
  测试名存在 → src 下有对应 3D/剖面组件；OpenGL 3.3 Core Profile 已统一
  （main.cpp 里 QSurfaceFormat 先决，为 QtWebEngine/MapCanvas/3D 共享上下文）。
- 真机 966MB 体读路径实测无冻结（64³ 体窗 46ms、对角剖面 97ms）——数据后端扛得住。
- UI 面板族有 LOD/瓦片增量先例（canvas 瓦片增量、panel LOD 控件——ledger 记过）。
- 视角/相机状态、选择信号、色标体系需勘察后定（不要假定现有 API）。

## 范围（按依赖序推进，做到 Oracle 为止）

1. **体渲染传递函数编辑器**：opacity/color TF（线性分段），实时作用于 3D 体
   （GLSL uniform/1D LUT 纹理），面板驱动走信号→服务，视图不干活。
2. **交互剖切**：轴对齐 IL/XL/T 剖面拖拽移动已有则深化；新增任意斜剖面
   （两点拾取起止线 → 后端取数 → 剖面贴入 3D 场景）、栅栏（多段折线）剖面原型。
3. **层位面 + 井轨迹上图 3D**：层位网格→3D 面片（色标映射属性），井轨迹折线+
   井名标注（QOpenGL 路径或 Qt3D 视现状定），与 catalog 层角色联动开显隐。
4. **切片动画**：T 向/IL 向扫掠播放（帧率可调、可暂停、可导出 PNG 序列），
   播放中读路径走异步预取（前后 N 片缓存窗口），UI 线程零阻塞。
5. **GL 环境护栏**：offscreen 测试无 GL 时测试按既有 GL-skip 先例处理
   （tst_seismic_3d teardown UB 轮盘教训：worker 析构序要抄条目注册表
   shared_ptr 所有权模式）。

## Oracle

1. TF 编辑器改不透明度曲线，3D 体实时响应（offscreen 下断言渲染像素非均匀变化）。
2. 任意斜剖面：两点拾取→出剖面图元，像素级非空断言 + 取数路径断言（服务调用被触发）。
3. 层位/井轨迹上图：fixture 层位与井轨迹渲染进 3D 场景，显隐联动层树。
4. 动画扫掠：帧推进→取数异步化→UI 不阻塞（断言播放期间 UI 事件循环可响应）。
5. 无 GL 环境下相关测试优雅 skip 不 crash（含 teardown 干净，无 5-6/8 挂死先例回归）。
6. 性能：966MB 体扫掠 1 秒实测帧率/取数命中率入 docs/progress/seismic-3d.md；
   比率门而非墙钟。
7. ctest 全绿；`check_layering --strict` 绿；ledger；push + `gh pr create`。

## 勘察指引

- `src/ui/seismic*`、`src/services/seismicmapping.*`、`seismictaskservice.*` 现状接口；
  `tools/seismic_showcase.cpp`（demo 壳）可作 3D 行为试验台。
- ledger 先例记载：`datapreviewtabs` 两段式加载、`SgyVolume` 体窗 API、瓦片坐标约定
  （x=xline 索引偏移，行 0=最大 inline 显示向）。
- Qt3D vs 手写 QOpenGL：先查 src 现状沿用既有选型，不引入新框架。
- 井轨迹数据源：wellcomposite/timedepthmodel 域对象；层位网格看 catalog/层位目录资产。

## 禁区

- 不改数据读路径内部实现（性能范畴若发现瓶颈只记录移交）。
- 不动 DESIGN.md token；3D 场景配色走主题变量。
- GL 上下文共享策略不动 main.cpp 的 CoreProfile 先决（改坏 QtWebEngine 连锁）。
- 视图层红线不破：面板发信号，取数/计算全在服务与数据层。

## 迭代协议

- 轮0-1：勘察 3D 栈现状（渲染器形态/数据源接口/测试基建）→ 选型定案入 ledger。
- 中段：TF → 剖切 → 层位井轨 → 动画 顺序推进，每块单测+实测+ledger。
- 完成定义 = Oracle 7 条全绿。
