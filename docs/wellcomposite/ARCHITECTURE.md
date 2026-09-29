# 单井综合柱状图系统架构（wave/wellcomposite-deep 基线）

日期：2026-09-29。分支：`wave/wellcomposite-deep`（自 master 93b4195）。

本文件是 Phase 0 侦察产物：完整记录改造前的道类型清单、数据流、渲染管线、
模式切换器与已知的硬编码/臆造残留，作为深度升级（D1–D8）的对照基座。

## 1. 组件清单与职责

| 文件 | 层 | 行数 | 职责 |
|---|---|---|---|
| `src/domain/wellcompositemodel.{h,cpp}` | 数据 | 175 | 纯数据类型：CurveData/TextInterval/FormationInterval/StratigraphyInterval/FaciesInterval/LithologyInterval/CoreBarrel/ImageDepthItem/SymbolItem + 聚合容器 ComprehensiveWellData |
| `src/io/wellcompositexml.{h,cpp}` | 数据 | 615 | SpreadsheetML 流式解析器：按工作表名分流 10 类表（测井曲线/离散曲线/岩性道/地层单位道/砂层组道/文本道/取心数据道/符号道/标准层道/坐标） |
| `src/ui/wellcomposite/wellcompositetrack.{h,cpp}` | 视图 | 343+1662 | 10 类井道渲染器 + 岩性花纹工厂 + 沉积相纹理工厂 |
| `src/ui/wellcomposite/wellcompositecanvas.{h,cpp}` | 视图 | 163+640 | 交互画布（Header/Body 双子件 + 双滚动条）：滚轮锚点缩放、拖拽漫游、双击复位、十字准星 |
| `src/ui/wellcomposite/wellcompositepanel.{h,cpp}` | 视图 | 73+531 | 总装面板：工具条（井名徽章/比例尺/缩放/配置）+ 画布 + 底部图例条；setupTracksFromData 装配逻辑 |
| `src/ui/wellcomposite/curveconfigdialog.{h,cpp}` | 视图 | 106+779 | 道排列对话框：上移/下移/置顶/置底/重命名/显隐/曲线合并(2-4根)/解散/拆出/恢复默认 |
| `src/ui/wellcomposite/wellpositionlegendwidget.{h,cpp}` | 视图 | 149+704 | 底部综合条：物理比例尺条 + 全井迷你导航条（点击/拖拽跳深度）+ 图例弹窗 |
| `tests/tst_wellcomposite.cpp` | — | 567 | 10 个测试函数（含真实数据 QSKIP 门控） |

## 2. 道类型清单（10 类）

| 道类 | TrackType | 数据载体 | 道体渲染要点 |
|---|---|---|---|
| 标尺道 | `DepthScale` | 无（比例尺字符串） | nice-step 主/次刻度（0.5→1000 序列），右侧刻度线+深度数字 |
| 文本道 | `Text` | `TextInterval[]` | 区间块 + 类别前缀 + 自动换行 |
| 地层道 | `Formation` | `FormationInterval[]` | 色块 + 上下界线 + 居中层名 |
| 岩性道 | `Lithology` | `LithologyInterval[]` | LithologyPatternFactory 花纹纹理刷 + 半透明岩性名标签 |
| 取芯道 | `Core` | `CoreBarrel[]` | 筒号 + 绿框 + 收获率双色填充柱 |
| 图片道 | `Image` | `ImageDepthItem[]` | 深度等比 pixmap / 缺图灰块占位 |
| 曲线道 | `Curve` | `CurveData[]`（≤4） | 连续折线/离散散点/直方图三模式；4 等分虚线网格；NaN 断线 |
| 符号道 | `Symbol` | `SymbolItem[]` | 射孔梳齿/油气水圆点/测压菱形/正反旋回三角 |
| 地层组合道 | `StratigraphyCompound` | `StratigraphyInterval[]` | 系\|统\|组三列，系/统跨层合并绘制，窄列竖排字 |
| 沉积相组合道 | `FaciesCompound` | `FaciesInterval[]` | 相\|亚\|微三列，微相全纹理填充 + 白胶囊衬字 |

## 3. 数据流

```
SpreadsheetML XML ──io/wellcompositexml 流式解析──▶ ComprehensiveWellData（domain）
                                                        │
LAS 文件 ──PreviewDocService::lasAt──▶ QVector<CurveData>│
                                                        ▼
                              WellCompositePanel::loadComprehensiveXml / loadLasCurves
                                                        │  setupTracksFromData（11 步装配）
                                                        ▼
                              WellCompositeCanvas::setTracks(shared_ptr<WellTrack>[])
                                                        │
                                          ┌─────────────┴─────────────┐
                                          ▼                           ▼
                              WellCompositeHeader（置顶）   WellCompositeBody（道体+手势）
```

- 装配策略（wellcompositepanel.cpp:386-529）：有数据才摆道；地层组合道仅在系/统可
  推导或文档已提供时出现；沉积相道仅在文档提供真实相区间时出现（不臆造）。
- LAS 装配（:248-384）：曲线按助记名前缀分四组（岩性 GR/CAL/SP/BS/AZIM、三孔隙
  AC/DEN/CNL/POR、电阻率 RT/RXO/RD/RS/ILD/ILM/AT、其他），每组每 4 根一道。

## 4. 渲染管线

- Header 固定高 72px 置顶；Body 占余高；滚动条宽 10px 贴右/贴底。
- 深度坐标：`pxPerMeter = basePxPerMeter × zoomFactor`（基准由比例尺字符串换算，
  1:500 ⇒ 3779.528/500 ≈ 7.559 px/m；96dpi 每英寸 96px ⇒ 每米 3779.5 像素）。
- `depthToY/YToDepth` 线性映射；视口 = [scrollDepth, scrollDepth + bodyH/pxPerMeter]。
- 逐道 paintHeader/paintBody，道体横向由 hScrollOffset 平移；准星线横跨全宽 +
  深度气泡；右下角白卡显示比例尺与视口范围。
- 曲线 LOD：无——全量 drawPolyline（D6.6 缺口）。

## 5. 模式切换器 / 交互现状

- 比例尺：QComboBox（可编辑）5 预设 + 自适应；缩放时反向换算 `1:N` 联动回写。
- 滚轮 = 锚点缩放（光标深度为锚）；左/中键拖拽 = 纵向平移深度 + 横向平移道；
  双击 = 复位；单击（位移<4px）发 depthClicked。
- CurveConfigDialog 是唯一的道管理面（模态）。

## 6. 已知硬编码 / 臆造残留清单（Phase 0 审计）

| # | 位置 | 问题 | 处置 |
|---|---|---|---|
| H1 | track.cpp:1343-1428 `autoDeriveStratigraphy` | 珠江口盆地组名→系/统映射硬编码 if-else 链（粤海/万山/韩江/珠江/珠海/恩平/文昌/白垩…） | D4.1 国际年代色标表外置后此表退役为查表 |
| H2 | canvas.cpp:229-259 | 比例尺字符串→pxPerMeter 换算与 nice 化散落两处（resetZoom/setScaleRatio 重复） | 重构为单一换算函数 |
| H3 | panel.cpp:306-324 | LAS 曲线分组按助记名前缀硬编码三组 | D1.7 多曲线组合道编辑器放开用户自定义 |
| H4 | io xml.cpp:13-55 | 曲线颜色/单位按助记名硬编码挑选 | 保留为缺省，D3.12 覆盖层可覆盖 |
| H5 | canvas 滚轮 | 无 Shift 加速/Ctrl 微调（D2.10 缺口） | D2.10 |
| H6 | 缩放上下限 | zoomFactor 夹在 [0.2,20]，无「最大 10m 段/最小整井」语义（D2.8 缺口） | D2.8 |
| H7 | track 道头 | 无单位行、无道宽拖拽、无右键菜单（D1.3–D1.5、D1.11 缺口） | Phase 1 |
| H8 | 全部 | 无任何 undo/编辑面（Phase 3 全缺） | Phase 3 |
| H9 | 曲线渲染 | 无屏幕/导出双渲染参数、无抗锯齿分档（D4.12 缺口） | D4.12 |
| H10 | 文本道/地层道文字 | 道名随道宽无截断策略 | D1.11 自适应截断 |
| H11 | 深度轴 | 仅 MD、仅米制（D6.x 全缺） | Phase 6 |
| H12 | io 解析 | 无写回能力（派生版本无从谈起） | D3.3/D3.4 于本 wave 落 io 写函数 |

## 7. 深度升级的目标架构（本 wave 落地）

```
src/ui/wellcomposite/
  ├─ wellcompositetrack.*      既有 10 道渲染器（保留 + 扩花纹/色标/截断）
  ├─ wellcompositecanvas.*     画布（+吸附/橡皮筋/标注/键盘/编辑拖拽钩子）
  ├─ wellcompositepanel.*      总装（+工具条扩展/编辑模式/多井容器）
  ├─ trackregistry.*  NEW      D1.2 道类型注册表（工厂+元数据+序列化 TrackSpec）
  ├─ trackconfigdialog.* NEW   D1.6/D1.7 道配置/多曲线组合编辑器
  ├─ wellcompositestore.* NEW  D1.8/D2.3/D3.6/D3.12/D4.10/D5.6 会话与 sidecar 持久化
  ├─ depthtools.* NEW          D2.1/D2.3/D2.6/D2.7/D2.11/D6.3 深度交互与换算
  ├─ intervalstatistics.* NEW  D2.2 区间统计
  ├─ editstack.* NEW           D3.8/D3.9 undo/redo + 脏状态
  ├─ topseditor.* NEW          D3.1/D3.2/D3.7 TOPs 编辑与批量导入
  ├─ intervaleditor.* NEW      D3.4/D3.5/D3.13 岩性/相区间编辑与合并拆分
  ├─ stratassignment.* NEW     D3.6 地层单元显式指派
  ├─ editsession.* NEW         D3.3/D3.10/D3.11/D3.15 编辑会话/审计/只读降级
  ├─ chronostratcolors.* NEW   D4.1 国际年代色标（≥40 单元）
  ├─ patterncatalog.* NEW      D4.2/D4.3 花纹库扩充 + JSON 相映射
  ├─ legendgenerator.* NEW     D4.4 自动图例
  ├─ exportengine.* NEW        D4.5–D4.10 PDF/PNG/SVG/打印/预设
  ├─ multiwellview.* NEW       D5.1–D5.7 多井对比容器
  └─ depthtransform.* NEW      D6.1–D6.6 MD↔TVD/KB/TWT/ft/采样/LOD
src/io/wellcompositexml.*      扩展：writeComprehensiveWellXml（派生版本序列化 +
                               审计表）+ 井斜/时深表独立解析函数（新类型定义于此头）
```

分层裁决（受文件领地约束的合规设计）：
- ui→io include 被 layering 词表白名单（仅 `io/lasdoc.h`）挡死——派生 XML 写回、
  井斜/时深解析落 io 层，由测试直接驱动全链路断言；运行时由视图发意图信号携带
  domain 文档，壳/未来服务接线后落盘（TODOS 登记接缝）。
- 会话记忆（道宽/道序/显隐）走 QSettings（测试沙箱隔离）；项目级 sidecar
  （标注/指派/量程覆盖/导出预设/对比模板）由视图层 JSON 存储助手落
  `wellcompositestore`（QtCore 文件 IO，无 io include，护栏绿；迁移服务层记 TODOS）。
- 剪贴板/CSV 解析属视图输入收集面，落视图层 helper。
