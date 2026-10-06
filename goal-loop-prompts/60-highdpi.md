# Goal-Loop 方向 60：高 DPI 适配——3D 视口确定性缺陷修复 + 护栏

## 背景（实测事实，勿再勘察；行号为 2026-10-06 master `f130fb2` 口径）

- **3D 视口不乘 dpr（确定性渲染缺陷）**：`src/ui/seismic3d/
  seismic3dviewportwidget.cpp:161-162` `resizeGL(int w, int h)`
  直接 `glViewport(0, 0, w, h)`——w/h 是**逻辑像素**，未乘
  `devicePixelRatioF()`。QOpenGLWidget 在高 DPI 屏上 FBO 是物理
  像素尺寸，视口按逻辑像素设置导致渲染内容仅占左下角或拉伸
 （平台相关）。paintGL 内 overlay 的 `QPainter p(this)` +
  QFont（`:211-217` 附近）同样无 dpr 处理。
- **2D fallback 拼接无 dpr**：`src/ui/seismic3d/
  seismic3dviewpanel.cpp:1391` GL 看门狗失败后的 CPU 拼接
  `QImage(c.rgba.data(), c.width, c.height, ...)` 无 dpr 乘法。
- **已做对的两处（对齐口径，勿重做）**：图标层
  （`src/ui/paleoicons.cpp:60/66/112` 按 scale 生成）；剖面
  地震栅格（`src/ui/wellsection/wellsectionscene.cpp:816-818`
  用 `p->deviceTransform()` 映射设备像素 rect，`:851` QImage
  随设备分辨率走 + cacheHit 键）。
- **护栏缺失**：`tools/check_ui_invariants.py` 无 dpr 检查项；
  DESIGN.md 无高 DPI 条款（仅 `:29/:124` pointSize 跟随 DPI）；
  golden 测试按 `grab()` 比对，不覆盖分数缩放。

## 环境接线（Windows 本机实测口径）

```powershell
cd C:\Users\wangj.KEVIN\projects\paleo_workstation
git fetch origin
git worktree add .worktrees\highdpi -b goal/highdpi-20261007 origin/master
cd .worktrees\highdpi
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo `
  -DQGIS_PREFIX=C:/Users/wangj.KEVIN/paleo-qgis-prefix `
  -DCMAKE_PREFIX_PATH="C:/deps/Qt/6.8.0/msvc2022_64;C:/Users/wangj.KEVIN/paleo-qgis-deps/Library;C:/deps/qscintilla-install" `
  -DQSCINTILLA_PREFIX=C:/deps/qscintilla-install
cmake --build build -j8
```

坑位同方向 58。离屏测试可用 `QT_SCALE_FACTOR=2` 环境变量模拟
分数缩放（Qt 官方机制，offscreen 下有效）。

## 目标形态（建议按序）

1. **R0 全面普查**：全 `src/ui` 的 `QOpenGLWidget` 子类与
   `QImage`→屏幕贴图路径清单化（rg `resizeGL|paintGL|devicePixelRatio`）；
   用 `QT_SCALE_FACTOR=2` offscreen 逐个截图取证（哪些面破）。
2. **3D 视口修复**：`resizeGL`/`paintGL` 乘 `devicePixelRatioF()`
  （QOpenGLWidget 标准口径：`width()*dpr`/`height()*dpr`）；
   鼠标拾取坐标反算同步（逻辑↔物理换算一致性——拾取错位是
   dpr 修复的经典次生 bug，测试必须覆盖）。
3. **overlay 修复**：fps/井名标注的 QPainter 路径 dpr 对齐。
4. **2D fallback 修复**：CPU 拼接 QImage 按 dpr 放大采样或
   直接换算目标 rect。
5. **3D 渲染器对账**：四个 renderer（faultsurface/seismicslice/
   horizonsurface/volumeframe）的 Update 一次性上传面是否
   dpr 敏感逐个定性（纹理内容分辨率 vs 视口尺寸）。
6. **护栏**：`check_ui_invariants.py` 加「QOpenGLWidget 子类
   resizeGL 必须引用 devicePixelRatio」静态检查（正反夹具）；
   DESIGN.md 补「高 DPI」节（dpr 口径 + 测试约定）。
7. **golden 用例**：`QT_SCALE_FACTOR=2` 下 3D 视口与剖面
   snapshot 的尺寸断言（grab() 输出物理像素尺寸 = 逻辑×2）。

## 通用纪律（方向内全程有效）

- **分层**：全部在 `src/ui`（视图层）+ tools 护栏 + DESIGN 文档；
  `check_layering.py --strict` 绿。
- **DESIGN.md**：视觉口径先补节再实现——dpr 属渲染正确性非
  视觉决策，但新节要引用既有 token 口径。
- **行为红线**：dpr=1 路径逐像素不变（对拍断言）；修复只影响
  dpr≠1 场景。
- **资源**：`-j8`；ctest 串行。
- **无人值守**：普查与修复序自行定案记 ledger。
- **ledger**：`.goal-loop-ledger-highdpi.md`。
- **多轮 review（硬要求）**：每批 → 测试全绿 → diff 自审
  （dpr 数学正确性/拾取一致性/dpr=1 不回归/护栏有效/DESIGN
  同步 五维）→ 修复 → 再 review，至少两轮零 High/Medium；
  Low 记 PR body。
- **提交**：原子提交，中文 conventional 前缀。

## Oracle 验收（逐条需验证证据）

1. 3D 视口：`QT_SCALE_FACTOR=2` offscreen 下 grab() 物理尺寸
   = 逻辑×2（断言）；渲染内容满幅（非左下角）——像素采样
   断言（四角与中心均非空）。
2. 拾取一致：dpr=2 下模拟点击视口中心，拾取命中场景中心
  （坐标换算测试）。
3. 2D fallback：看门狗失败路径的拼接图在 dpr=2 下满幅。
4. dpr=1 回归：既有 golden/快照测试全绿（对拍零差异）。
5. 护栏：静态检查对故意不乘 dpr 的夹具打红（mutation）；
   DESIGN.md「高 DPI」节与实现一致。
6. 全量 ctest 对照 R0 红集合 diff 为空；layering 三档绿。
