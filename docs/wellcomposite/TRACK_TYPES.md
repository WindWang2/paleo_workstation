# 道类型参考（TRACK_TYPES）

> wave/wellcomposite-deep。道类型经 `TrackRegistry` 注册表管理（D1.2）：
> 新增道类型只需 `registerType` 一条 Entry（typeId/显示名/缺省宽/工厂/捕获器），
> 即被「新建道」、配置对话框、持久化、CSV 导出、图例等全部下游能力发现。

## 1. 类型词表（D1.2 要求全覆盖）

| typeId | TrackType 枚举 | 显示名 | 缺省宽 | 数据载体 | 备注 |
|---|---|---|---|---|---|
| `depth` | DepthScale | 深度标尺道 | 64 | 比例尺串 | 不可用户新建/删除；三行道头含比例尺与单位；D6.5 TWT 副刻度列 |
| `text` | Text | 文本道 | 110 | `TextInterval[]` | 取样/试油结论文本 |
| `formation` | Formation | 地层分层道 | 80 | `FormationInterval[]` | 组段色块 |
| `litho` | Lithology | 岩性道 | 80 | `LithologyInterval[]` | ≥30 种花纹（PATTERNS.md） |
| `core` | Core | 取芯道 | 65 | `CoreBarrel[]` | 筒号/进尺/心长/收获率 |
| `image` | Image | 图片道 | 110 | `ImageDepthItem[]` | 深度等比照片 |
| `curve` | Curve | 测井曲线道 | 170 | `CurveData[]` ≤4 | 多曲线组合（D1.7 独立量程/单位/色） |
| `gr` | Curve | GR 岩性曲线道 | 120 | 同上 | curve 语义预设 |
| `discrete` | Curve | 离散实测散点道 | 160 | 同上 | 散点模式 |
| `symbol` | Symbol | 符号道 | 48 | `SymbolItem[]` | 射孔/油气水/旋回 |
| `strat` | StratigraphyCompound | 地层组合道 (系\|统\|组) | 145 | `StratigraphyInterval[]` | 系/统查国际年代色标表；未识别层名经 D3.6 指派补齐 |
| `facies` | FaciesCompound | 沉积相组合道 (相\|亚\|微) | 180 | `FaciesInterval[]` | 微相全纹理填充（相名→花纹 JSON 映射 D4.3） |

## 2. TrackSpec（数据模型侧，D1.1）

```cpp
struct TrackSpec {
  QString typeId;        // 注册表键
  QString title;
  qreal width;           // px
  bool visible;          // 显隐
  bool printIncluded;    // 打印/导出开关（D4.8）
  QVariantMap params;    // 类型参数：curves/name 列表、curveOverrides、
                         // showGrid、gridDensity、scaleRatio(depth)…
};
```

- 序列化：`toVariantMap()/fromVariantMap()`（QSettings 会话 D1.8 与 sidecar 模板 D5.6 共用）。
- 装配：`TrackRegistry::createTrack(spec)` 建渲染器壳；曲线数据体由
  `TrackOps::injectCurvesFromWellData` 按名注入（覆盖层 D3.12 经
  `applyCurveOverride` 应用，源 LAS 不动）。
- 捕获：`captureSpec(track)` 渲染器 → spec（typeId 由注册表钉死）。
- **道增删改不重建面板**：面板持 spec 列表增量同步画布
  （`applySpecsIncrementally`：顺序/宽度/显隐/打印按「类型+标题」匹配）。

## 3. 渲染器接口（WellTrack）

| 虚方法 | 语义 |
|---|---|
| `paintHeader(p, rect, currentDepth)` | 道头（建议经 `paintHeaderChrome` 三行区：标题/刻度/单位，道宽自适应截断） |
| `paintBody(p, rect, top, bottom, pxPerMeter)` | 道体（视口深度区间内绘制） |
| `headerScaleText()/headerUnitText()` | D1.11 道头第二三行文本 |
| `trackToolTip(depth)` | D1.9 tooltip |
| `isPrintIncluded()/setPrintIncluded()` | D4.8 导出开关 |

## 4. 道交互

- 道头拖拽换位（D1.3）：投影指示线 + Esc 取消 + 松手 `moveTrack`。
- 分隔线拖拽调宽（D1.4）：5px 热区，下限 24px，宽度集随会话记忆。
- 右键菜单（D1.5）：隐藏/复制/CSV/跳深度/配置/删除。
- 网格（D4.11/D1.7）：密度 0 无/1 两分/2 四分/3 十分含次网格。
- 导出档渲染（D4.12）：全抗锯齿 + 曲线 +1px 加粗（ExportEngine::CurvePenGuard）。
