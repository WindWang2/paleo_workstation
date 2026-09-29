# 基线功能矩阵 × 测试覆盖矩阵（Phase 0 产出）

日期：2026-09-29。master 基线：93b4195。测试基线：`tst_wellcomposite` 10 函数。

## 1. 现有功能矩阵

| 能力 | 现状 | 现有测试 |
|---|---|---|
| SpreadsheetML XML 解析（10 类表） | ✅ | testParseComprehensiveXmlRealData（真实文件 QSKIP 门控） |
| LAS 曲线装配（4 组前缀分道） | ✅ | testWellPositionLegendWidgetAndSync（部分） |
| 10 类道渲染 | ✅ | testTrackCreationAndCurveOverlayLimit / testStratigraphyCompoundTrack / testFaciesCompoundTrack（paint 冒烟） |
| 岩性花纹（6 种） | ✅ | testLithologyPatterns |
| 相纹理（11 种 + SVG 资源优先） | ✅ | testFaciesPatterns |
| 曲线道 1-4 根合并 | ✅ | testTrackCreationAndCurveOverlayLimit |
| 滚轮锚点缩放/拖拽漫游/双击复位 | ✅ | testCanvasDepthAndZooming |
| 比例尺动态换算联动 | ✅ | testDynamicScaleRatioCalculation |
| 道排列对话框（移/并/散/拆/隐） | ✅ | testCurveConfigDialogCombineAndDissolve |
| 全井迷你导航条 | ✅ | testWellPositionLegendWidgetAndSync |
| 参考井/测区井徽章语义 | ✅ | testCompositePanelAssembly |
| 视觉取证截图 | ✅ | PALEO_UI_CAPTURE 约定（不判像素） |

## 2. 交付项缺口矩阵（87 项）

### Phase 1 道系统（12 项）
| 项 | 缺口 |
|---|---|
| D1.1 模型/视图分离 | 全缺：道类数据成员直接内嵌渲染器，无 TrackSpec 抽象 |
| D1.2 类型注册表 | 全缺：TrackType 枚举 + dynamic_pointer_cast 分发 |
| D1.3 道头拖拽换位 | 全缺 |
| D1.4 分隔线拖宽 | 全缺（道宽仅对话框改） |
| D1.5 道右键菜单 | 全缺 |
| D1.6 道配置对话框 | 部分：CurveConfigDialog 有重命名/显隐，无量程/单位/颜色/打印开关 |
| D1.7 多曲线组合编辑器 | 部分：可合并/解散，无各曲线独立量程色与重叠网格开关 |
| D1.8 配置持久化 | 全缺（对话框一次性，不记忆） |
| D1.9 道 tooltip | 全缺 |
| D1.10 隐藏道管理条 | 全缺（隐藏只能进对话框） |
| D1.11 道头三行区自适应 | 全缺（道头只标题一行） |
| D1.12 多道 Y 缩放联动 | N/A（单画布统一深度轴）→ 语义落在多井视口同步锁 D2.9 |

### Phase 2 深度交互（12 项）
D2.1 吸附 ✗ / D2.2 橡皮筋区间统计 ✗ / D2.3 深度标注 ✗ / D2.4 滚轮中心缩放 ✓（已锚
点缩放，重构为可配锚点）/ D2.5 全井导航缩略条 部分（迷你条有，无拖框跳转增强语义）
/ D2.6 书签 ✗ / D2.7 Ctrl+G ✗ / D2.8 缩放上下限语义 ✗ / D2.9 视口同步锁 ✗（无多画布）
/ D2.10 Shift 加速 Ctrl 微调 ✗ / D2.11 深度读数条 部分（状态栏文字有，无大读数+最近
标志层）/ D2.12 标志层间距 gap 高亮 ✗。

### Phase 3 编辑套件（15 项）
全缺。无编辑模式、无写回、无 undo、无审计、无冲突检测。派生版本写回需 io 写函数（基
线 io 只读不写）。

### Phase 4 视觉与输出（12 项）
D4.1 国际年代色标 ✗（H1 硬编码 9 组）/ D4.2 花纹 6 种 → 需 ≥30 / D4.3 JSON 相映射 ✗
（FaciesCatalog SVG 资源已有雏形）/ D4.4 图例生成器 部分（WellLegendDialog 静态弹窗，
非自动按当前井收集）/ D4.5–D4.10 导出/打印/预设 全缺 / D4.11 网格密度自适应 ✗（固定
4 等分）/ D4.12 双渲染参数 ✗。

### Phase 5 多井对比（7 项）
全缺。WellCompositePanel 单井单例。

### Phase 6 深度变换（6 项）
全缺。仅 MD 米制。

### Phase 7 打磨可达性（10 项）
D7.8 i18n 部分（既有 tr() 覆盖好，新增面需保持）/ 其余基本全缺。

### Phase 8 测试文档（8 项）
现有 10 测试函数；需新增 ≥60；文档 0 → ≥5。

## 3. 黄金图基线策略

真实 XML（HZ28-6-1）仅存在于开发机私有目录——黄金图以**确定性合成数据**建立
（tests/golden/wellcomposite/*.png），像素抽样断言取大色块均值比对（容差），
抗字体/AA 抖动；真实数据测试维持 QSKIP 门控 + PALEO_UI_CAPTURE 取证。
