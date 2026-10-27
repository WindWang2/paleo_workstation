# Goal-Loop 账本：方向 60 高 DPI 适配（goal/highdpi-20261007）

基线 origin/master f130fb2。Oracle 六条见任务书。

## R0 普查（2026-10-06，行号 = f130fb2 口径）

### QOpenGLWidget 子类清单（rg 全 src/ui）
| 类 | 文件 | dpr 状态 |
|----|------|----------|
| Seismic3DViewportWidget | src/ui/seismic3d/seismic3dviewportwidget.cpp | **缺陷**：resizeGL:161-162 `glViewport(0,0,w,h)` 逻辑像素；FBO 是物理像素 → 高 DPI 内容只占左下/拉伸 |

全仓唯一 QOpenGLWidget 子类（3dviewpanel.cpp 仅转发引用）。

### QImage→屏幕贴图路径清单
| 路径 | 机制 | dpr 定性 |
|----|------|----------|
| wellsectionscene.cpp:816-818,851 | 手算 deviceTransform 设备像素 rect + cacheHit 键 | ✅ 已对齐（任务书确认，勿重做） |
| paleoicons.cpp:60/66/112 | 按 scale 生成 + setDevicePixelRatio | ✅ 已对齐 |
| seismicsectioncanvas.cpp:1164-1215 | `p.drawImage(imgDest, 数据分辨率栅格)`，QPainter 逻辑坐标 | ✅ Qt 原生 dpr（backing store 物理 + painter 逻辑自动缩放） |
| datapreviewtabs_internal.h:1410 | `p.drawImage(dst, m_img.scaled(...))` | ✅ 同上（数据栅格→逻辑 rect） |
| crossplotpanel.cpp:122 | `p.drawImage(r, m_density)` | ✅ 同上 |
| seismic3dfallback.cpp:79 | `p.drawImage(dst, img)` | ✅ Qt 原生 dpr，满幅成立（Oracle 3 用测试钉住） |
| seismic3dviewpanel.cpp:1391 | sweep 导出兜底：缓存切片 rgba 直接出 PNG | ⚪ 数据帧导出（注释明示「数据帧序列」），分辨率属数据语义非屏幕语义——**定性非缺陷**；不乘 dpr（乘了反而失真）。有 GL 时走 grabFramebuffer()=物理像素全幅，修复视口后自然正确 |

### 3D renderer 对账（四 renderer）
faultsurface/seismicslice/horizonsurface/volumeframerenderer：全部世界几何 + proj/view/model 矩阵渲染，
shader 无 gl_FragCoord、无 glViewport 引用、纹理为内容分辨率 → **dpr 不敏感**；唯一 dpr 入口 = widget 的 glViewport。

### 拾取数学定性
- pickTopPlaneGrid:625-662：unProject 用逻辑 vp + 逻辑鼠标坐标 → NDC 尺度不变，自洽。
- pickSliceAt:735-819：投影到 width()/height()（逻辑）与逻辑鼠标比较 → 自洽。
- 结论：修 glViewport 后拾取**无需**换算改动；经典错位 bug（物理视口+逻辑 vp 不一致）不存在。
  用 dpr=2 坐标测试钉住（Oracle 2）。

### overlay 定性
paintGL:216 `QPainter p(this)`：QOpenGLWidget paint device 由 Qt 内部 setSize(物理)+setDevicePixelRatio
（qopenglwidget.cpp beginPaint），QPainter 逻辑坐标 + 字形按物理分辨率光栅化 → **机制上 dpr 正确**。
前置缺陷是场景 glViewport 逻辑像素（内容左下角）而非 overlay 本身。修复后 overlay 与场景同幅。
用 dpr=2 像素采样测试钉住（Oracle 1 的「四角+中心非空」覆盖 overlay 主场景满幅性）。

### 测试基建口径（重要约束）
- offscreen 平台下 QOpenGLWidget **不建 GL 上下文**（tst_seismic_3dviz.cpp:898 既有结论）。
  → 3D 视口端到端 GL 验证只能走 QOffscreenSurface+QOpenGLContext+FBO 直渲模式（既有先例 tfOffscreenRealtimePixels）。
  widget 层验证 = grab() 尺寸断言 + 拾取数学单测（不依赖 GL）。
- QT_SCALE_FACTOR=2 offscreen 下有效（Qt 官方机制），影响 QWidget::grab() 物理尺寸与 devicePixelRatioF()。

## 修复方案（定案）

1. resizeGL 乘 devicePixelRatioF()：`glViewport(0, 0, qRound(w*dpr), qRound(h*dpr))`。
   dpr=1 时数值不变（qRound(w*1.0)==w）→ 行为红线满足。
2. 拾取/aspect 不动（尺度不变性证明见上）。
3. DESIGN.md 补「High DPI」节（dpr 口径 + 测试约定 + 与 pointSize token 的引用）。
4. 护栏：check_ui_invariants.py 加 `gl-dpr` 规则——src/ui 内 resizeGL 定义体内必须引用 devicePixelRatio；
   selftest 正反夹具；ctest 三档（ui_invariants / _strict / _selftest）自动覆盖。
5. golden：新测试文件 tst_seismic_highdpi.cpp：
   - QT_SCALE_FACTOR=2（setenv 于 QGuiApplication 构造前）下 grab() 物理尺寸=逻辑×2（面板/回退件）；
   - FBO 直渲：物理尺寸视口 + helper 数学 → 四角+中心非背景像素；
   - 拾取坐标换算：dpr=2 模拟点击中心 → pickTopPlaneGrid 命中场景中心网格；
   - 2D fallback 满幅：dpr=2 下 grab() 各 cell 有非背景内容;
   - dpr=1 尺寸恒等式（qRound 恒等断言）。

## 轮次记录

第0轮 | R0 普查完成：唯一确定性缺陷=resizeGL；渲染器/拾取/overlay/2D 显示路径定性清楚 | 普查产出本文 | 通过 | 进入修复

## 第1轮（修复 + 护栏 + 测试）

改动清单：
1. `src/ui/seismic3d/seismic3dviewportwidget.{h,cpp}`：新增静态
   `physicalViewportSize(w,h,dpr)`（qRound 单一真源）；`resizeGL` 用它设物理视口。
   dpr=1 时 qRound 恒等 → 行为红线。
2. `tools/check_ui_invariants.py`：新增 `gl-dpr` 规则（resizeGL 定义体内必须引用
   devicePixelRatio；只扫 .cpp 定义，声明不扫）；selftest 正反夹具更新。
   mutation 实测：还原缺陷代码 → `FAIL .../seismic3dviewportwidget.cpp:161` exit=1；
   恢复 → clean exit=0。ctest 三档（ui_invariants/_strict/_selftest）自动带上。
3. `DESIGN.md`：新增「High DPI」节 + Decisions Log 2026-10-06 条目。
4. `tests/tst_seismic_highdpi.cpp`（新）：见文件头注释。子进程重入模式
   （QT_SCALE_FACTOR 只在 QGuiApplication 构造时读）。
5. `cmake/extra-seismic.cmake`：注册 tst_seismic_highdpi（LIBS paleo_ui + 夹具宏）。

自审五维（第1轮）：
- dpr 数学：qRound(w*dpr) 与 Qt FBO 尺寸（QSize::operator*(qreal) 逐维 qRound）同口径 → glViewport 与 FBO 严格等尺寸。
- 拾取一致：逻辑 vp + 逻辑鼠标，NDC 尺度不变；测试对拍 dpr=1 vs dpr=2。
- dpr=1 不回归：qRound 恒等 + 既有全量测试（待跑）。
- 护栏有效：mutation 红/绿已实测。
- DESIGN 同步：节内引用 physicalViewportSize / gl-dpr / QT_SCALE_FACTOR 与实现一致。

待验证：构建 + tst_seismic_highdpi 全绿 + 全量 ctest 对照。

### 第1轮验证证据（本机 Windows，受控环境=run_env.bat：极简 PATH + deps-Qt 6.11 前 + TEMP 钉回 + offscreen）

- 环境（坑位实测，与 layer-split ledger 一致）：
  - `QCoreApplication::applicationFilePath()` 混链环境返回空串 → 测试内
    `selfExePath()` Windows 走 GetModuleFileNameW，其余平台走 Qt 口径。
  - `QImage::fill(QRgb)` 对 Format_RGBA8888 按**格式字节序**解释（qRgb 红变蓝）
    → 用 QColor 重载。
  - 小夹具（3×4 网格）上中心+邻近点可能同格去重 → 次击取既有用例证实的
    必异格点 (100,250)。
  - offscreen QPA 在 Windows 不建 GL 上下文（既有结论复证）→ GL 槽
    ctest-offscreen 下确定性 SKIP（与既有 GL 用例同形态，Linux CI 承载）。
- ctest `tst_seismic_highdpi`：**Passed**（5 pass + 4 skip：GL 槽 skip + 3 个
  child-only 槽在父进程 skip——设计内）。
- GL 满幅本机取证（`-platform windows` 真实 WGL，`glPhysicalViewportFullFrame`
  单槽）：**PASS**——物理视口（physicalViewportSize(400,300,2)=800×600，与
  widget 同口径）四象限中心+帧中心非背景、覆盖 >90%（实际 ~100%）；
  对照实验：同 FBO 设逻辑视口 400×300 → 右上背景化、覆盖骤降至 ~25%
  （<1/3 断言过）——缺陷形态可检出（测试自证）。
- 拾取对拍：dpr=1 中心点击 (200,150)→(il=1001,xl=2002)；dpr=2 子进程同点
  同值（QVERIFY child==baseline 过）→ 拾取 dpr 无关 ✓。
- 护栏 mutation：还原缺陷代码 → `FAIL src/ui/seismic3d/seismic3dviewportwidget.cpp:161
  resizeGL 未引用 devicePixelRatio`（exit 1）；恢复 → clean（exit 0）。
- layering 三档 / i18n / ui_invariants 三档：全绿。

第1轮 | 修复+护栏+测试落地；ctest 单测绿 + GL 满幅取证 + mutation 红绿 + 拾取对拍 | 见上 | 通过 | 全量 ctest 对照（修复态 vs R0 基线）

### 全量 ctest 对照（修复态 vs R0 基线，串行，受控环境）

- 修复态：324 项，红 193（`build/redlist-fixed.txt`）。
- R0 基线（stash 后重建）：324-1=323 项（无新测试），红 192（`build/redlist-r0.txt`）。
- diff：仅 `tst_singlefactor_guilatency`（修复态 Timeout / R0 全量跑绿）。
- **定性：与本方向无关**——基线构建上 standalone 复跑 3/3 全部 Timeout
  （180s 超时），该测试在全量序列与 standalone 两种负载形态下表现漂移，
  是本机 GUI 延迟量测类测试的负载敏感既有项（先例：layer-split ledger
  环境红定性口径）。本方向 diff（3D 视口 dpr/tools/DESIGN/新测试）与其
  无接触面。
- 结论：红集合 diff = 环境噪声，无本方向引入的新失败。

## 第2轮（review 回修）

第一遍对抗性 review（独立代理，源码级对证 Qt 6.8 头/6.11 运行时）：
High 0 / Medium 2 / Low 3。核心三维（dpr 数学/拾取一致/dpr=1 不回归）无发现。

采纳回修：
- [M1] 护栏注释绕过：gl-dpr 的函数体子串检查先剥注释再判——注释里出现
  devicePixelRatio 而代码没真乘不再过检；selftest 加注释绕过反例夹具。
- [M2] 子进程空转绿灯：dpr2ChildGrabSize/dpr2ChildFallbackFullFrame 补
  标记行（PALEO_HIDPI_GRAB / PALEO_HIDPI_FB），父槽断言解析到标记并
  核对尺寸——QT_SCALE_FACTOR 失效时子进程全 skip → 父槽必红（防 vacuous green）。
- [L3] DESIGN.md 数据出口措辞：区分「数据位图=数据分辨率」与「屏幕抓帧
  （grabFramebuffer）=物理像素回读，所见即所得」——与 sweep/截图实现一致。

记 PR body 不改代码：
- [L1] gl-dpr 规则只认 `::resizeGL(` 限定名定义——类内 inline 定义漏报
  （现库无此形态；新代码走头文件声明+cpp 定义惯例）。
- [L2] 注释/字符串中的 `::resizeGL(` 理论上可误报（`;` 守卫滤掉多数）。

### 第2轮验证证据（回修后全绿）

- ctest tst_seismic_highdpi：Passed（1.38s）。
- GL 满幅（-platform windows 真实 WGL）：PASS（831ms）。
- ui_invariants --selftest：PASS（gl-dpr ×2 + motion ×2 + qss-hex ×1 ——
  含新的注释绕过反例）；--strict：clean。
- layering --strict 绿；i18n 绿。
- mutation（升级版）：resizeGL 改为 `QSize(w, h); // devicePixelRatio 由上层处理`
  （只在注释里提词）→ FAIL exit=1；恢复 → clean。注释绕过空档已封。
- dpr=1 函数级指纹（tst_seismic_3dui 全槽）：4 pass（含 fallbackWidgetPaints）
  + 1 skip（GL offscreen）+ 8 fail——8 个全部是 writeTestSegy 夹具写盘失败
  （沙箱写约束既有环境红，与本 diff 零接触面）；与全量红集合对照互证
  （该测试在 R0/修复态两次全量跑中同为红）。
- master 前进 8 提交（zh-cn-l10n 方向）与本方向文件零重叠，PR 无冲突面。

## 终局（完成协议）

- 第二遍验证（防侥幸）：ctest tst_seismic_highdpi Passed / layering 三档绿 /
  ui_invariants 三档绿 / GL 满幅（-platform windows）PASS——关键项全部双跑。
- 第二轮 review：High 0 / Medium 0 / Low 2（字符串字面量绕过=已知启发式边界
  记 PR body；Decisions Log 摘要滞后已顺手修）。两轮零 High/Medium 达标。
- 原子提交 ×5（fix/test/guard/docs(design)/docs(goal-loop)，中文 conventional）。
- 脚手架清理（*.bat / fp-*.txt）；分支已推 \`goal/highdpi-20261007\`。
- **PR #254**：https://github.com/WindWang2/paleo_workstation/pull/254

| 轮 | 改动 | 验证 | 结果 | 下一步 |
|----|------|------|------|--------|
| 2 | review 回修 M1/M2/L3 + DESIGN 日志行 | 全套绿 + mutation 升级版红/绿 | 通过 | 提交/PR |
| 终局 | 原子提交×5 + 推分支 + PR #254 | 第二遍验证全绿 | 完成 | — |
