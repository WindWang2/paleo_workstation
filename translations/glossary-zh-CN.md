# Paleo Workstation 地质与桌面 UI 中文本地化术语表规范 (zh_CN Glossary)

> **版本**：1.0.0  
> **状态**：规范定案 (Authoritative Specification)  
> **适用范围**：`translations/paleo_zh_CN.ts` 全量 5,015 条文案、自动化本地化门禁 (`tools/check_l10n.py`)、代码内 `tr()` 调用与 UI 界面。  
> **词条规模**：193 条标准化术语（覆盖 10 大专业领域）+ 25 项疑难歧义行业裁决 + 5 项 Qt 桌面 UI 规范。

---

## 1. 规范总则

1. **唯一权威性**：所有翻译必须严格遵循本术语表中的「标准中译」，严禁自由发挥或使用「禁用/误译清单」中的词汇。
2. **上下文一致性**：同一专业概念在全仓所有 196 个上下文（Context）中必须保持译法统一，杜绝同词异译（例如 `horizon` 在全系统必须统一为「层位」，严禁出现「地平线」）。
3. **行业规范优先**：术语译名全面对接中国石油天然气行业地质地球物理标准（SY/T 标准）及中国地质学会规范。
4. **Qt 桌面规范对齐**：快捷键、标点、省略号及按钮文案完全对齐 Qt 官方 zh_CN 桌面规范。

---

## 2. 疑难歧义术语行业裁决表 (25 项硬裁决)

针对石油地质与计算机图形学中多义词的裁决标准：

| # | 英文术语 | 裁决标准中译 | 判定语境与裁决理由 | 绝对禁用误译 |
|---|---------|-------------|-------------------|-------------|
| 1 | **Project** | **工程** | 石油地质行业软件统称（如 Paleo 工程、打开工程、工程目录）；仓内 117 处源串统一用「工程」。 | 项目（严禁出现“打开项目”、“新建项目”） |
| 2 | **Horizon** | **层位** | 地震或地质反射层界面（如 T1 层位、时间层位、层位追踪）。 | 地平线、视野、视界 |
| 3 | **Layer** | **图层** (GIS/QGIS) / **层** (地质) | QGIS 地图画布要素统称「图层」（如矢量图层、栅格图层）；地质单体称「层/小层」。 | 分层、涂层 |
| 4 | **Realization** | **实现** | 地质随机建模中每次蒙特卡洛/地质统计学模拟生成的单一样本体（如 realization 1 $\rightarrow$ 实现 1）。 | 认识、领会、变现、体会 |
| 5 | **Well Top** | **井分层** | 钻井地层分界点（井顶界面深度的地质解释）。 | 井顶、井口顶部 |
| 6 | **Crossplot** | **交会图** | 测井/岩石物理双参数或多参数散点交会分析图。 | 交叉图、十字图、交会画图 |
| 7 | **Track** | **道** (测井/柱状图) | 综合柱状图中的纵向曲线道、深度道、岩性道（TrackConfigDialog）。 | 轨道、音轨、行踪、轨迹 |
| 8 | **Trace** | **地震道** | 地震剖面中的单道地震记录（单检波点或单共反射点道）。 | 追踪、痕迹、线索 |
| 9 | **Pick / Picking** | **拾取** / **解释** | 剖面上拾取断层棒或层位反射点（如 seismic pick $\rightarrow$ 地震拾取/层位解释）。 | 采摘、挑选、挖掘 |
| 10 | **Checkshot** | **校验炮** / **垂直地震测时** | 井口测时标定数据，时深转换核心数据源（TimeDepthModel）。 | 检查射击、检查炮、抽查 |
| 11 | **Gridding** | **网格化** | 散点/等值线通过数学插值生成连续规则表面（GridSolver）。 | 格子化、栅格化（栅格化专指 Rasterize） |
| 12 | **Polygonize** | **多边形矢量化** / **栅格矢量化** | 将分类栅格相图转换为多边形矢量要素（deriveFaciesPolygons）。 | 多边形化、多边形生成 |
| 13 | **Flattening** | **地层拉平** | 沿指定标志层或层位将地震/连井剖面校正为水平基准。 | 扁平化、压平 |
| 14 | **Throw** | **断距** | 断层两盘之间的垂直位移落差（Fault Throw）。 | 投掷、抛出、丢弃 |
| 15 | **Facies** | **相** / **沉积相** | 沉积环境与岩石特征的综合地质相别（相图、相分类）。 | 面貌、相貌、外观 |
| 16 | **Working Copy** | **工作副本** | 数据中枢受管资产的本地可编辑副本（防止并发脏写）。 | 工作拷贝、作业复本 |
| 17 | **Release** | **发布** / **正式发布** | 数据治理中将已定案版本晋升为受保护的只读交付版本。 | 释放、发行、免责 |
| 18 | **Tombstone** | **墓碑标记** / **软删除标记** | 存储治理中记录物理文件已被逻辑删除但保留溯源的标记。 | 墓碑、碑文、死标记 |
| 19 | **Asset** | **资产** / **数据资产** | Catalog 统一管理的地质数据单元（AssetType: horizon, well, fault）。 | 财产、财富、资金 |
| 20 | **Lineage / Provenance**| **血缘** / **数据血缘** | 衍生数据对源数据的推导谱系与反向溯源图谱（DerivationGraph）。 | 起源、出身、家系、门第 |
| 21 | **Cut** | **井断点** / **层位切割** | 井轨迹与断层的交点称「井断点」；断层与层位的相交多边形称「层位切割」。 | 剪切、伤口、切开 |
| 22 | **Base Level** | **基准面** | 层序地层学中侵蚀与沉积动态平衡的理论基准面。 | 基础水平、基底平面 |
| 23 | **Closure** | **构造幅度** / **圈闭** | 构造等值线最高点与溢出点之间的闭合垂直高度（地质构造）。 | 闭包、停工、关闭 |
| 24 | **Tie** | **标定** / **井震标定** | 井资料（声波/密度/合成记录）与地震时间剖面的层位对比对齐。 | 领带、平局、绑定 |
| 25 | **Upscale / Upscaling** | **粗化** | 将厘米级高分辨率测井曲线属性均化映射到米级地质模型网格。 | 升尺度、升级、扩大规模 |

---

## 3. 标准化专业术语全量主表 (10 大领域 · 193 词)

### 3.1 领域 1：层位与地层 (Horizon & Stratigraphy · 18 词)

| English Term | 标准中译 (zh_CN) | 领域 (Domain) | 语境与定义说明 | 禁用/误译 (Banned/Avoided) | 仓内关联上下文与代码位置 |
|--------------|-----------------|--------------|---------------|--------------------------|-------------------------|
| horizon | 层位 | Stratigraphy | 地震或地质反射层位界面 | 地平线、视野 | `EntityPanel`, `HorizonLocatorFilter` |
| layer | 图层 / 层 | Stratigraphy | GIS地图图层或地质单体层 | 分层、涂层 | `CompositionWorkflow`, `DataListPanel` |
| stratum / strata | 地层 | Stratigraphy | 具有一定层理和年代特征的沉积岩石层 | 阶层、层级 | `sequenceframework.h` |
| sublayer | 小层 | Stratigraphy | 油气田开发中的最小地质储层细分单元 | 子层、次层 | `wellstratification.h` |
| marker / marker bed | 标志层 | Stratigraphy | 特征显著、全区易于对比的地层标准层 | 标记、书签、标记床 | `sequenceframework.h` |
| isochronous surface | 等时面 | Stratigraphy | 同一地质年代沉积形成的地质时间界面 | 同步表面、等时表面 | `sequenceframework.h` |
| flattening | 地层拉平 | Stratigraphy | 剖面沿特定标志层拉平以恢复原始沉积古形态 | 扁平化、压平 | `SectionWorkbench`, `SeismicSectionDockWidget` |
| formation | 组 / 地层 | Stratigraphy | 岩石地层划分的基本单位（如沙河街组） | 形成、构成、编队 | `welltops.h` |
| member | 段 | Stratigraphy | 组的次级地层细分单元（如沙三段） | 成员、构件 | `sequenceframework.h` |
| bed | 层 / 单层 | Stratigraphy | 段内部可分辨的单一沉积岩层 | 床、苗床 | `sequenceframework.h` |
| chronostratigraphy | 年代地层学 | Stratigraphy | 以地质年代和时间跨度划分地层的学科体系 | 计时地层学 | `sequenceframework.h` |
| Wheeler diagram | Wheeler时空剖面 / 年代地层图 | Stratigraphy | 将深度轴转化为地质时间轴的地层时空展开图 | 惠勒图、轮子图 | `docs/progress/sequence-framework.md` |
| zonation | 地层划分对比 / 分带 | Stratigraphy | 区域性地层层序划分与多井对比方案 | 区域化、区划 | `wellsectionui` |
| unconformity | 不整合面 / 不整合 | Stratigraphy | 沉积间断或构造运动形成的地层不整合接触面 | 不顺从、不一致 | `sequenceframework.h` |
| conformity | 整合面 / 整合 | Stratigraphy | 连续沉积未受侵蚀或间断的平行地层接触面 | 顺从、一致性 | `sequenceframework.h` |
| top / base | 顶面 / 底面 | Stratigraphy | 单个地质层位或储集单元的顶边界与底边界 | 顶部/底部 (地质界面统一为顶面/底面) | `stratgrid.cpp`, `PropertyModelWorkflow` |
| hiatus | 沉积间断 | Stratigraphy | 地层记录中缺失的地质时间或沉积空白 | 间隙、脱落 | `sequenceframework.h` |
| stratal slice | 地层切片 | Stratigraphy | 沿上下两个等时地层界面成比例内插的地震切片 | 层切片、地层薄片 | `seismic3dviewpanel.cpp` |

### 3.2 领域 2：断层与构造地质 (Fault & Structural Geology · 18 词)

| English Term | 标准中译 (zh_CN) | 领域 (Domain) | 语境与定义说明 | 禁用/误译 (Banned/Avoided) | 仓内关联上下文与代码位置 |
|--------------|-----------------|--------------|---------------|--------------------------|-------------------------|
| fault | 断层 | Structural | 地壳岩石受力破裂且两侧发生明显位移的构造 | 错误、故障、过错 | `faultset.h`, `FaultManagerPanel` |
| fault plane | 断层面 | Structural | 断层两盘岩石相对滑动的空间几何破裂面 | 故障飞机、故障平面 | `faultsurface.h` |
| fault trace | 断层线 / 断层迹线 | Structural | 断层面在地表或层位面上的交线 | 故障追踪、断层跟踪 | `faultset.h` |
| fault stick | 断层棒 / 断层线棒 | Structural | 在地震剖面上解释拾取的断层空间折线段 | 故障棍、断层木棒 | `faultinterpretationcontroller.h` |
| fault polygon | 断层多边形 | Structural | 断层面切割地层所形成的平面断距缺失/重叠多边形 | 故障多边形 | `faultset.h` |
| fault surface | 断裂面 / 断层面 | Structural | 三维网格化重构后的断裂曲面几何体 | 故障表面 | `docs/progress/fault-surface.md` |
| fault throw | 断距 / 垂直落差 | Structural | 断层两盘同一地质界面的垂直错动距离 | 断层投掷、故障抛掷 | `FaultManagerPanel` |
| heave | 水平断距 | Structural | 断层两盘同一地质界面的水平位移跨度 | 起伏、升起、呕吐 | `FaultManagerPanel` |
| fault cut | 井断点 / 断层交切 | Structural | 井轨迹穿过断层面处出现的层位缺失或重复点 | 断层切割、故障剪切 | `welltopseditordialog.cpp` |
| hanging wall | 上盘 | Structural | 倾斜断层面之上的岩盘块体 | 挂墙、悬墙 | `faultset.h`, `tst_gridsolver.cpp` |
| footwall | 下盘 | Structural | 倾斜断层面之下的岩盘块体 | 脚墙、底墙 | `faultset.h`, `tst_gridsolver.cpp` |
| normal fault | 正断层 | Structural | 上盘相对沿断层面滑落的引张型断层 | 正常断层、普通故障 | `FaultManagerPanel` |
| reverse fault | 逆断层 | Structural | 上盘相对沿断层面逆推上升的挤压型断层 | 反向断层、倒退故障 | `FaultManagerPanel` |
| thrust fault | 逆冲断层 | Structural | 断面倾角平缓（通常小于45度）的低角度逆断层 | 推力故障、冲断层 | `FaultManagerPanel` |
| strike-slip fault | 走滑断层 | Structural | 两盘岩块主要沿断层走向作水平剪切移动的断层 | 滑动断层、击滑断层 | `FaultManagerPanel` |
| fault network | 断裂系统 / 断裂网 | Structural | 区域内成因关联的断裂构造组合网络 | 故障网络 | `faultsetstore.h` |
| fault linkage | 断裂交接 / 断层连接 | Structural | 独立断层段之间相互贯通连接的地质过程与构造 | 故障链接 | `docs/progress/fault-interpretation.md` |
| structural closure | 构造幅度 / 构造圈闭 | Structural | 构造等高线闭合区域的高度及圈闭范围 | 结构闭包、构造封闭 | `MappingWorkbench` |

### 3.3 领域 3：井、井轨迹与井分层 (Well, Trajectory & Tops · 20 词)

| English Term | 标准中译 (zh_CN) | 领域 (Domain) | 语境与定义说明 | 禁用/误译 (Banned/Avoided) | 仓内关联上下文与代码位置 |
|--------------|-----------------|--------------|---------------|--------------------------|-------------------------|
| well top | 井分层 / 分层点 | Well | 测井解释中在单井深度上确定的地层界面点 | 井顶、井口顶部 | `WellTopsEditorDialog`, `WellTopsMergeDialog` |
| wellbore | 井筒 | Well | 钻井钻出的地下圆柱形孔道物理实体 | 井孔、水井内径 | `welltrajectory.h` |
| wellhead | 井口 | Well | 井筒在地表或水下的起始点与井口装置 | 井头、水井源头 | `WellSitingPanel` |
| trajectory | 井轨迹 | Well | 井筒在三维地下空间中的几何延伸中心曲线 | 弹道、轨道 | `welltrajectory.h` |
| deviation survey | 测斜数据 / 井斜测量 | Well | 随深度测量的井斜角与方位角记录点列 | 偏差调查、偏差勘测 | `welltrajectory.h` |
| measured depth (MD) | 测量深度 / 井深 (MD) | Well | 沿井眼轴线实测的连续钻井长度深度（米） | 测得深度 | `SectionSetupDialog`, `DataPreviewTabs` |
| true vertical depth (TVD) | 垂深 / 真垂深 (TVD) | Well | 自井口基准面垂直向下至目标点的铅直深度（米） | 真正垂直深度 | `SectionSetupDialog`, `PaleoMainWindow` |
| subsea true vertical depth (TVDSS) | 海拔垂深 (TVDSS) | Well | 以平均海平面为基准的地下测点绝对标高负深度 | 海底真垂直深度 | `docs/progress/time-depth.md` |
| elevation | 标高 / 海拔 | Well | 井口或地面相对于平均海平面的几何高程 | 仰角、立面 | `WellSitingPanel` |
| Kelly Bushing (KB) | 补心 / 转盘面 (KB) | Well | 钻机方钻杆补心顶面高程，通常作为单井测深零点 | 凯利衬套、方补心 | `DataPreviewTabs` |
| ground level (GL) | 地面高程 (GL) | Well | 井口所在地的自然地面高程 | 地平面、接地电平 | `DataPreviewTabs` |
| inclination | 井斜角 | Well | 井筒切线方向与铅垂线之间的夹角（度） | 倾角、倾向、倾斜度 | `welltrajectory.h` |
| azimuth | 方位角 | Well | 井筒切线在水平面投影与正北方向的顺时针夹角 | 地平纬度、方位天顶角 | `welltrajectory.h` |
| dogleg severity (DLS) | 狗腿度 / 全角变化率 (DLS) | Well | 井眼轴线空间弯曲程度率（通常以度/30米计量） | 狗腿严重程度 | `welltrajectory.h` |
| bottom hole / total depth (TD) | 井底 / 完钻深度 (TD) | Well | 钻井钻达的最深钻井终点深度 | 底洞、总深度 | `DataPreviewTabs` |
| composite log | 综合柱状图 / 综合测井图 | Well | 汇集测井曲线、分层、岩性图例和成果的多道柱状图 | 复合日志、复合测井 | `WellCompositePanel`, `WellCompositeBody` |
| well section / correlation panel | 连井剖面 / 地层对比剖面 | Well | 串联多口井展布地层与测井曲线的横向对比地质剖面 | 水井截面、关联面板 | `wellsectionui`, `WellSectionWellsDialog` |
| well siting | 布井 / 井位部署 | Well | 在勘探开发区依据地质编图规划新井坐标与轨迹 | 找井、水井选址 | `WellSitingPanel` |
| log curve / curve | 测井曲线 / 曲线 | Well | 随深度连续采集记录的岩石物理响应参数序列 | 弯曲、日志曲线 | `CurveConfigDialog` |
| track | 道 / 曲线道 | Well | 综合柱状图中纵向划分的展示通道（如深度道、曲线道） | 轨道、音轨、行踪 | `TrackConfigDialog` |

### 3.4 领域 4：测井与岩石物理 (Petrophysics & Well Logging · 20 词)

| English Term | 标准中译 (zh_CN) | 领域 (Domain) | 语境与定义说明 | 禁用/误译 (Banned/Avoided) | 仓内关联上下文与代码位置 |
|--------------|-----------------|--------------|---------------|--------------------------|-------------------------|
| gamma ray (GR) | 自然伽马 (GR) | Petrophysics | 测量地层自然放射性强度的测井曲线（指示泥质含量） | 伽马射线 | `PetroPhysPanel`, `lasdoc.h` |
| spontaneous potential (SP) | 自然电位 (SP) | Petrophysics | 测量井内泥浆与地层水离子扩散电动势的测井曲线 | 自发势、自发电位 | `PetroPhysPanel` |
| caliper (CALI) | 井径 (CALI) | Petrophysics | 测量钻井井筒直径变化的机械测井曲线 | 卡尺、游标卡尺 | `PetroPhysPanel` |
| resistivity | 电阻率 | Petrophysics | 地层阻止电流通过能力的导电性物理度量 | 电阻、阻抗 | `PetroPhysPanel` |
| deep induction (ILD) | 深感应 (ILD) | Petrophysics | 探测未受泥浆侵入地层深部电阻率的电磁感应测井 | 深度归纳、深感应法 | `PetroPhysPanel` |
| shallow induction (ILS) | 浅感应 (ILS) | Petrophysics | 探测冲洗带或侵入带电阻率的浅探测感应测井 | 浅度归纳 | `PetroPhysPanel` |
| acoustic / sonic (DT) | 声波时差 (DT) | Petrophysics | 声波穿过单位距离地层所需的时间（微秒/米） | 声音、音速 | `PetroPhysPanel` |
| density (RHOB) | 补偿密度 (RHOB) | Petrophysics | 测量地层体积密度的双源双探测器放射性测井 | 密度 | `PetroPhysPanel` |
| neutron porosity (NPHI) | 中子孔隙度 (NPHI) | Petrophysics | 利用热中子减速特征测定地层含氢指数与孔隙度的测井 | 中子孔隙率 | `PetroPhysPanel` |
| photoelectric effect (PE) | 光电吸收截面指数 (PE) | Petrophysics | 测量地层平均原子序数以识别骨架岩石矿物类型的测井 | 光电效应 | `PetroPhysPanel` |
| porosity | 孔隙度 | Petrophysics | 岩石中未被固体骨架充填的孔隙体积与总体积之比 | 多孔性、多孔度 | `PetroPhysPanel` |
| permeability | 渗透率 | Petrophysics | 岩石在一定压差下允许流体通过的传导能力度量 | 可渗透性、磁导率 | `PetroPhysPanel` |
| water saturation (Sw) | 含水饱和度 (Sw) | Petrophysics | 岩石有效孔隙中地层水所占体积百分比 | 水饱和、水分饱和度 | `PetroPhysPanel` |
| shale volume (Vsh) | 泥质含量 (Vsh) | Petrophysics | 储集岩中泥质矿物占岩石骨架总体积的百分比 | 页岩体积、页岩量 | `PetroPhysPanel` |
| pay zone / net pay | 有效厚度 / 产层 | Petrophysics | 储层中具有工业开采价值并产出油气的有效净厚度 | 支付区、净支付 | `PetroPhysPanel` |
| cutoff | 门限值 / 截止值 | Petrophysics | 划分有效储层物性下限参数阈值（如孔隙度下限） | 截断、切断 | `PetroPhysPanel` |
| crossplot | 交会图 | Petrophysics | 双曲线或三曲线散点多维交会识别岩性与流体的图表 | 交叉图、十字图 | `CrossplotPanel` |
| cluster analysis | 聚类分析 | Petrophysics | 基于测井响应模式的多维样本无监督自动分类算法 | 簇分析、团簇分析 | `CrossplotPanel` |
| electrofacies | 测井相 | Petrophysics | 由一组特征测井响应组合反映出的地质与沉积相单元 | 电相、电面貌 | `CrossplotPanel` |
| upscale / upscaling | 粗化 | Petrophysics | 将沿井密集采样的连续测井曲线聚合至地层网格单元 | 升级、放大、升尺度 | `stratgrid.cpp`, `PropertyModelWorkflow` |

### 3.5 领域 5：地震与地球物理 (Seismic & Geophysics · 24 词)

| English Term | 标准中译 (zh_CN) | 领域 (Domain) | 语境与定义说明 | 禁用/误译 (Banned/Avoided) | 仓内关联上下文与代码位置 |
|--------------|-----------------|--------------|---------------|--------------------------|-------------------------|
| seismic section | 地震剖面 | Seismic | 沿二维测线或三维提取线切出的地震双程反射剖面 | 地震部分、地震切片 | `SeismicSectionDockWidget`, `SeismicSectionCanvas` |
| seismic survey | 地震工区 / 测线 | Seismic | 规划并完成地震野外采集的三维勘探区或二维测网 | 地震调查、地震普查 | `SeismicSectionDockWidget` |
| inline | 主测线 / Inline | Seismic | 三维地震采集观测系统中沿主要激发接收方向的剖面线 | 内联、行内 | `SeismicSectionDockWidget`, `Seismic3DViewPanel` |
| crossline | 联络测线 / Crossline | Seismic | 与主测线正交垂直布置的三维地震测线剖面 | 跨线、十字线 | `SeismicSectionDockWidget`, `Seismic3DViewPanel` |
| time slice | 时间切片 / 水平切片 | Seismic | 三维地震体在固定双程旅行时（如1500ms）水平截出的振幅图 | 时间片、时间划分 | `Seismic3DViewPanel` |
| depth slice | 深度切片 | Seismic | 三维深度域地震体在恒定铅直深度截出的水平切片 | 深度片 | `Seismic3DViewPanel` |
| horizon slice | 层位切片 | Seismic | 沿三维解释层位曲面提取地震属性或振幅切片 | 地平线切片 | `Seismic3DViewPanel` |
| trace | 地震道 | Seismic | 单次地震记录检波通道接收的一维时间-振幅波形序列 | 追踪、痕迹、线索 | `SeismicSectionDockWidget` |
| two-way traveltime (TWT) | 双程旅行时 (TWT) | Seismic | 地震波从地表下行至地下反射界面再返回地表的往返时间 | 双向行程时间 | `SectionSetupDialog`, `SeismicSectionCanvas` |
| amplitude | 振幅 | Seismic | 地震反射波形偏离平衡零位线的最大摆动幅值 | 幅度、广度 | `SeismicAttrPanel` |
| phase | 相位 | Seismic | 描述地震波形正负波峰波谷循环运动的周期性角位置 | 阶段、时期 | `SeismicAttrPanel` |
| frequency | 频率 | Seismic | 地震波每秒震动的周期数（单位赫兹 Hz） | 频次、频度 | `SeismicAttrPanel`, `InversionPanel` |
| spectrum | 频谱 | Seismic | 地震信号在频域上的能量与振幅分布谱线 | 光谱、范围 | `SeismicAttrPanel` |
| wavelet | 子波 | Seismic | 具有确定形状、主频和有限有效持续时间的简短地震震动脉冲 | 小波（信号学误混） | `InversionPanel` |
| seismic attribute | 地震属性 | Seismic | 从地震振幅、频率、相位、极性等衍生计算出的几何运动学特征 | 地震特征、地震属性体 | `SeismicAttrPanel` |
| instantaneous amplitude / envelope | 瞬时振幅 / 包络 | Seismic | 希尔伯特变换计算出的总反射能量复数地震道振幅包络 | 信封、即时幅度 | `SeismicAttrPanel` |
| instantaneous frequency | 瞬时频率 | Seismic | 瞬时相位随时间的一阶导数，反映波形局部主频特征 | 即时频率 | `SeismicAttrPanel` |
| coherence / coherence volume | 相干 / 相干体 | Seismic | 衡量相邻地震道波形相似性的三维属性体（用于断层裂缝检测） | 一致性、连贯性 | `SeismicAttrPanel` |
| curvature | 曲率 | Seismic | 描述地震层位几何弯曲程度的结构属性（指示断裂弯曲变形） | 弯曲度 | `SeismicAttrPanel` |
| relative acoustic impedance (RAI) | 相对波阻抗 (RAI) | Seismic | 对地震道积分估算出的相对地层波阻抗变化趋势 | 相对声阻抗 | `InversionPanel` |
| seismic inversion | 地震反演 | Seismic | 由地震振幅数据定量反向推算地下波阻抗及物性参数的方法 | 地震倒置、地震反转 | `InversionPanel` |
| synthetic seismogram | 合成记录 / 合成地震记录 | Seismic | 利用声波测井与密度计算反射系数并与子波卷积合成的地震道 | 综合地震图 | `time-depth.md`, `InversionPanel` |
| time-depth conversion | 时深转换 | Seismic | 利用速度模型在地震双程时间域与地质深度域之间相互变换 | 时间深度转换 | `depthconversionworkflow.h`, `time-depth.md` |
| checkshot | 校验炮 / 垂直地震测时 (Checkshot) | Seismic | 在钻井中用炸药或可控震源在井下检波器精确测量时深对应点 | 检查炮、抽查 | `time-depth.md`, `velocitymodel.h` |

### 3.6 领域 6：沉积相与地质建模 (Sedimentary Facies & Geological Modeling · 20 词)

| English Term | 标准中译 (zh_CN) | 领域 (Domain) | 语境与定义说明 | 禁用/误译 (Banned/Avoided) | 仓内关联上下文与代码位置 |
|--------------|-----------------|--------------|---------------|--------------------------|-------------------------|
| facies | 相 / 沉积相 | Modeling | 沉积岩石的生物、物理与化学总体地质相态特征 | 面貌、相貌、外观 | `CompositionWorkflow`, `PredictPage` |
| sedimentary facies | 沉积相 | Modeling | 一定沉积环境中所形成的具有特定特征的沉积物或沉积岩组合 | 沉积面貌 | `CompositionWorkflow` |
| lithofacies | 岩相 | Modeling | 反映特定沉积成因的岩石成分、结构与岩性特征组合 | 岩石面貌 | `CrossplotPanel` |
| depositional system | 沉积体系 | Modeling | 空间上具有成因联系并相互过渡的三维沉积相组合系统 | 沉积系统 | `sequenceframework.h` |
| channel | 河道 | Modeling | 古沉积体系中流体与砂体聚集搬运的条带状地质体 | 渠道、通道、频道 | `PredictPage` |
| levee | 天然堤 | Modeling | 河流或浊流溢出岸槽后沿两岸堆积沉淀的泥砂隆起堤岸 | 堤坝、防洪堤 | `PredictPage` |
| mouth bar | 河口坝 | Modeling | 三角洲平原水流注入湖盆或海洋时在河口前缘卸载形成的砂坝 | 口吧、嘴坝 | `PredictPage` |
| point bar | 边滩 / 点坝 | Modeling | 曲流河弯道凸岸水流回旋减速沉积形成的侧积砂体 | 点条、点状吧 | `PredictPage` |
| floodplain | 泛滥平原 | Modeling | 洪水溢出河槽并在河间沉积平原沉积的细粒泥质沉积带 | 洪泛区、淹没平原 | `PredictPage` |
| delta | 三角洲 | Modeling | 河流携砂注入静水盆地时由于流速减缓而在河口形成的向外突出的砂体 | 增量、三角形 | `PredictPage` |
| alluvial fan | 冲积扇 | Modeling | 暂态山洪从山谷狭口流出并在山口形成的扇形松散碎屑堆积体 | 冲积风扇 | `PredictPage` |
| turbidite | 浊积岩 | Modeling | 深水重力流（浊流）迅速沉积形成的具有鲍马序列特征的砂泥互层 | 浑浊岩 | `PredictPage` |
| carbonate platform | 碳酸盐岩台地 | Modeling | 在温暖浅海环境中由生物碎屑和化学沉淀广泛发育的厚层碳酸盐隆起区 | 碳酸盐平台 | `PredictPage` |
| property modeling | 属性建模 | Modeling | 在三维地质网格中定量充填孔隙度、渗透率、饱和度等储层物性 | 财产建模、性能建模 | `PropertyModelPanel`, `PropertyModelWorkflow` |
| variogram / semivariogram | 变差函数 / 半变异函数 | Modeling | 地质统计学中度量空间变量在不同距离下空间相关性与变异程度的函数 | 差异图、方差图 | `PropertyModelPanel` |
| realization | 实现 | Modeling | 随机地质模拟（如序贯模拟）中基于相同统计参数生成的一个等可能三维三维空间数值实体 | 认识、领悟、变现 | `PropertyModelWorkflow` |
| sequential Gaussian simulation (SGS) | 序贯高斯模拟 (SGS) | Modeling | 用于连续储层物性参数（如孔隙度）三维空间非均质分布的随机模拟方法 | 连续高斯模拟 | `PropertyModelPanel` |
| sequential indicator simulation (SIS) | 序贯指示模拟 (SIS) | Modeling | 用于离散岩相或骨架类型空间几何形态分布的条件指示概率模拟 | 连续指示模拟 | `PropertyModelPanel` |
| stochastic modeling | 随机建模 | Modeling | 考虑地下不确定性并利用地质统计学生成多套等可能地质模型的建模方法 | 概率建模 | `PropertyModelPanel` |
| deterministic modeling | 确定性建模 | Modeling | 基于确定物理数学方程或直接内插法则生成单一确定解的地质建模 | 决断建模 | `PropertyModelPanel` |

### 3.7 领域 7：编图、网格化与曲面运算 (Mapping, Gridding & Surface Operations · 18 词)

| English Term | 标准中译 (zh_CN) | 领域 (Domain) | 语境与定义说明 | 禁用/误译 (Banned/Avoided) | 仓内关联上下文与代码位置 |
|--------------|-----------------|--------------|---------------|--------------------------|-------------------------|
| gridding | 网格化 | Mapping | 将离散点集通过数学插值算法离散化为规则二维或三维节点网格 | 格子化、栅格化 | `tst_gridsolver.cpp`, `MappingWorkbench` |
| grid | 网格 / 格网 | Mapping | 由规则间距的正交网格线及其交点构成的空间离散表面 | 格子、电网 | `stratgrid.cpp`, `MappingWorkbench` |
| interpolation | 插值 | Mapping | 利用已知样本点数值估算空间未知网格节点数值的数学过程 | 内插、篡改 | `ConstraintWorkflow`, `velocitymodel.h` |
| kriging | 克里金插值 / 克里金 | Mapping | 考虑空间自相关结构的最优线性无偏地质统计空间插值算法 | 克里金法、克立金 | `sf-kriging.md`, `MappingWorkbench` |
| ordinary kriging | 普通克里金 | Mapping | 假设未知局部恒定均值的基准克里金空间估值算法 | 寻常克里金 | `sf-kriging.md` |
| inverse distance weighting (IDW) | 反距离加权 (IDW) | Mapping | 样本点权系数与空间距离成反比例幂次的确定性空间平滑插值方法 | 倒数距离加权 | `ConstraintWorkflow`, `velocitymodel.h` |
| minimum curvature | 最小曲率 | Mapping | 求解四阶双调和方程以保证内插表面总弯曲能量最小的经典连续平滑算法 | 极小弯曲 | `gridding-surface-ops.md` |
| spline | 样条函数 / 样条 | Mapping | 由多段连续多项式光滑拼接而成的低应变能量插值平滑曲线/曲面 | 齿条、键条 | `gridding-surface-ops.md` |
| contour / isoline | 等值线 | Mapping | 在编图曲面上将数值相等的相邻点连接构成的闭合或开闭等值曲线 | 轮廓线、等高线(非高程图禁用) | `MappingWorkbench` |
| isochore map | 等厚图 | Mapping | 展现地层单元垂向总厚度空间连续分布等值线变化的平面地质图件 | 等容线图 | `gridding-surface-ops.md` |
| isopach map | 等真厚图 / 等厚图 | Mapping | 展现校正地层倾角后垂直地层界面真厚度分布特征的等值线图 | 厚度图 | `gridding-surface-ops.md` |
| structure map | 构造图 / 等深图 | Mapping | 展现特定地质反射层顶面或底面地下绝对高程起伏与断裂的构造纲要图 | 结构图 | `MappingWorkbench` |
| facies map | 沉积相图 / 相图 | Mapping | 展现不同沉积相、亚相与微相平面展布区划轮廓的综合地质平面图 | 面貌图 | `CompositionWorkflow`, `PredictPage` |
| single-factor map | 单因素图 | Mapping | 仅表达砂厚、砂地比或孔隙度等单一地质沉积变量的专用成果图件 | 单因子图 | `single-factor-native.md` |
| evidence fusion | 证据合成 / 证据融合 | Mapping | 多源地质、测井与地震属性证据权重叠加计算综合沉积概率 | 证据熔合 | `CompositionWorkflow` |
| polygonize | 多边形矢量化 / 栅格矢量化 | Mapping | 将像元相图或栅格区域边缘追踪聚合为封闭几何多边形矢量图层 | 多边形化 | `CompositionWorkflow` |
| raster | 栅格 | Mapping | 按正交行列像元阵列存储属性数值的空间地理数据结构 | 光栅、光栅图 | `CompositionWorkflow`, `QgisLayoutService` |
| vector | 矢量 | Mapping | 由点、线、多边形几何坐标及属性表构成的离散空间几何数据结构 | 向量(GIS领域禁用) | `CompositionWorkflow`, `QgisEditingService` |

### 3.8 领域 8：层序地层学 (Sequence Stratigraphy · 15 词)

| English Term | 标准中译 (zh_CN) | 领域 (Domain) | 语境与定义说明 | 禁用/误译 (Banned/Avoided) | 仓内关联上下文与代码位置 |
|--------------|-----------------|--------------|---------------|--------------------------|-------------------------|
| sequence framework | 层序格架 | Sequence | 由各级层序界面、体系域及等时标志层搭建的地质时间对比三维骨架 | 序列框架 | `sequenceframework.h`, `SequenceFrameworkPanel` |
| depositional sequence | 沉积层序 | Sequence | 顶底受层序界面约束的相对连续成因的年代地层单元 | 沉积分裂 | `sequenceframework.h` |
| sequence boundary (SB) | 层序界面 (SB) | Sequence | 区域不整合面及其可对比整合面，代表相对海/湖平面急剧下降形成的侵蚀暴露面 | 序列边界 | `sequenceframework.h`, `roleregistry.cpp` |
| maximum flooding surface (MFS) | 最大海泛面 / 最大湖泛面 (MFS) | Sequence | 海侵/湖侵达到最大范围、沉积基准面最高的等时凝缩段地层界面 | 最大洪水面 | `sequenceframework.h` |
| transgressive surface (TS) | 初始海泛面 / 初始湖泛面 (TS) | Sequence | 低位体系域结束、水体开始快速大面积侵进的第一个显著海退转海侵界面 | 越界表面 | `sequenceframework.h` |
| systems tract | 体系域 | Sequence | 同一海/湖平面变化周期内在空间上共生共存的一组同步沉积相体系三维组合 | 系统束、系统道 | `sequenceframework.h` |
| lowstand systems tract (LST) | 低位体系域 (LST) | Sequence | 基准面下降至最低或缓慢初升时期沉积的层序下部沉积组合（如下切谷充填、盆底扇） | 低水位系统道 | `sequenceframework.h` |
| transgressive systems tract (TST) | 海侵体系域 (TST) | Sequence | 水体快速进退扩展、基准面急剧上升时期沉积的明显向上变深层序组合 | 侵入系统道 | `sequenceframework.h` |
| highstand systems tract (HST) | 高位体系域 (HST) | Sequence | 基准面上升速率变缓直至稳定、进积作用占主导时沉积的向上变浅层序组合 | 高水位系统道 | `sequenceframework.h` |
| parasequence | 准层序 | Sequence | 受海泛面或湖泛面约束的向上变浅、变粗的小型相沉积旋回副层序 | 副层序、半层序 | `sequenceframework.h` |
| parasequence set | 准层序组 | Sequence | 具有确定进积、退积或加积几何堆叠形态的成因准层序垂直组合 | 副层序集 | `sequenceframework.h` |
| accommodation space | 可容纳空间 | Sequence | 沉积物沉淀可资利用的理论潜在物理空间高度（受海面升降与构造沉降控制） | 住宿空间、融通空间 | `sequenceframework.h` |
| base level | 基准面 | Sequence | 地表侵蚀与沉积作用维持动态平衡的理论动态起伏基准曲面 | 基础水平、底线水平 | `sequenceframework.h` |
| progradation | 进积 / 前积 | Sequence | 沉积物供给速率大于可容纳空间增长速率导致沉积相带向盆地方向推进延伸 | 推进 | `sequenceframework.h` |
| retrogradation | 退积 | Sequence | 可容纳空间增长速率显著大于沉积供给导致相带向物源方向后退收缩 | 逆行、后退 | `sequenceframework.h` |

### 3.9 领域 9：数据中枢、版本与存储治理 (Data Fabric, Versioning & Storage Governance · 18 词)

| English Term | 标准中译 (zh_CN) | 领域 (Domain) | 语境与定义说明 | 禁用/误译 (Banned/Avoided) | 仓内关联上下文与代码位置 |
|--------------|-----------------|--------------|---------------|--------------------------|-------------------------|
| asset | 资产 / 数据资产 | Data Governance | 由项目 Catalog 统一注册编目管理的不可变地质数据实体 | 财产、财富、资金 | `DataListPanel`, `frameworkstore.h` |
| catalog | 编目 / 资产目录 | Data Governance | 管理工程所有数据资产元数据、版本链与引用依赖的 SQLite 核心索引 | 目录、产品目录 | `catalog-sqlite.md`, `DataListPanel` |
| dataset | 数据集 | Data Governance | 具有同构数据模型或逻辑归属的地质数据实体集合 | 资料集 | `DataListPanel` |
| working copy | 工作副本 | Data Governance | 用户在编辑会话中持有的临时本地可变拷贝，提交前与母本隔离 | 工作拷贝、作业复本 | `CompositionWorkflow`, `PaleoProjectStore` |
| commit journal | 提交日志 / 事务日志 | Data Governance | 记录数据写入阶段与原子状态转移的预写事务日志（确保崩溃安全回滚） | 提交期刊 | `PaleoProjectStore` |
| rollback | 回滚 | Data Governance | 事务中断或编辑放弃时撤销未定案修改、使状态恢复至历史检查点的操作 | 滚回、翻转 | `PaleoProjectStore` |
| snapshot | 快照 | Data Governance | 捕获工程特定时刻全部受管资产与元数据状态的冻结只读副本 | 快照照片、截图 | `paleoprojectstore.h` |
| provenance / lineage | 血缘 / 数据血缘 | Data Governance | 记录派生数据是由哪些原始数据、何种算法、何种参数衍生生成的推导谱系 | 起源、出处、门第 | `DerivationGraph`, `DerivationPanel` |
| lineage graph | 血缘图谱 | Data Governance | 将资产派生关系可视化展现为有向无环图（DAG）的拓扑图 | 家谱图、系谱图 | `DerivationGraph` |
| release | 发布 / 正式发布 | Data Governance | 将内部草稿工作版本正式晋升并打上标签的只读对外交付流程 | 释放、发行、免责 | `ReleasePanel`, `releasestore.h` |
| layer manifest | 图层清单 | Data Governance | 声明地图工程中所有可见图层源路径、符号化样式与渲染顺序的清单规范 | 图层宣言、图层载货单 | `layermanifest.h`, `QgisProjectService` |
| storage governance | 存储治理 | Data Governance | 负责磁盘配额预算、孤立文件检测清理、去重及存储空间回收的后台服务 | 存储统治、仓储管理 | `StorageGovernanceDialog` |
| deduplication | 去重 / 数据去重 | Data Governance | 基于 SHA-256 内容哈希避免重复物理落盘的单实例存储优化机制 | 重复删除 | `StorageGovernanceDialog` |
| tombstone | 墓碑标记 / 软删除标记 | Data Governance | 标记数据已被逻辑删除但保留历史版本标识以便同步与归档的元数据哨兵 | 墓碑、碑文、死标记 | `StorageGovernanceDialog` |
| checksum | 校验和 | Data Governance | 用于验证文件或数据包传输落盘前后完整性的哈希散列值 | 检验和 | `StorageGovernanceDialog` |
| project | 工程 | Data Governance | 包含全部地质资产、地图、解释会话与元数据的统一工作空间 | 项目（全局严格禁用） | `PaleoMainWindow`, `PaleoProjectStore` |
| work area / area | 工区 | Data Governance | 具有统一地理坐标系统与空间范围的地质勘探作业区域实体 | 工作区、作业区 | `PaleoMainWindow` |
| metadata | 元数据 | Data Governance | 描述地质数据来源、创建者、坐标系、哈希及版本历史的结构化属性 | 变质数据 | `metadata` |

### 3.10 领域 10：桌面 UI 与 Qt 规范 (Desktop UI & Qt Conventions · 22 词)

| English Term | 标准中译 (zh_CN) | 领域 (Domain) | 语境与定义说明 | 禁用/误译 (Banned/Avoided) | 仓内关联上下文与代码位置 |
|--------------|-----------------|--------------|---------------|--------------------------|-------------------------|
| dock widget | 停靠窗口 | UI Conventions | 可在主窗口边缘吸附停靠、自由浮动或 Tab 堆叠的子面板窗口 | 码头窗口、浮动板 | `SeismicSectionDockWidget` |
| canvas | 画布 / 地图画布 | UI Conventions | 承载交互式图形渲染与缩放平移漫游的视图核心工作区 | 帆布 | `SeismicSectionCanvas`, `QgisLayoutService` |
| locator | 定位器 / 全局搜索 | UI Conventions | 支持快捷键呼出并快速检索图层、井位、菜单动作的全局检索输入框 | 找寻器 | `HorizonLocatorFilter`, `PaleoMainWindow` |
| browser | 资源管理器 / 浏览器 | UI Conventions | 展现本地文件树与 Catalog 空间资产的树状导航视图 | 网页浏览器 | `DataListPanel` |
| colormap / palette | 色标 / 颜色表 / 调色板 | UI Conventions | 将数值物理量（如振幅、孔隙度）映射至连续或离散色带的色彩方案 | 彩色地图、彩色图 | `WellLegendDialog` |
| coordinate reference system (CRS) | 坐标参考系统 (CRS) | UI Conventions | 定义二维或三维空间椭球体基准与地图投影参数的空间参考框架 | 坐标参照系 | `CrossplotPanel`, `welltrajectory.h` |
| datum | 大地基准面 / 基准面 | UI Conventions | 测量空间坐标与高程所依据的确定性参考椭球曲面（如 WGS84、北京54） | 数据点、已知数 | `welltrajectory.h` |
| projection | 地图投影 / 投影 | UI Conventions | 将地球椭球面经纬度数学映射到平面正交坐标网（如高斯-克吕格、UTM）的法则 | 投射、规划 | `welltrajectory.h` |
| OK | 确定 | UI Conventions | 对话框首要积极肯定执行操作按钮 | 好、行、确认 | 全仓标准按钮 |
| Cancel | 取消 | UI Conventions | 放弃当前对话框变更并关闭窗口的操作按钮 | 作废、撤销 | 全仓标准按钮 |
| Apply | 应用 | UI Conventions | 执行当前配置并保持对话框继续打开以预览效果的操作按钮 | 申请、敷用 | 全仓标准按钮 |
| Save | 保存 | UI Conventions | 将当前文件或工程改动同步写入持久化存储介质的操作按钮 | 存盘、拯救 | 全仓标准菜单 |
| Save As | 另存为 | UI Conventions | 提示用户选择新路径或新名称另行写入副本的保存动作 | 保存为 | 全仓标准菜单 |
| Export | 导出 | UI Conventions | 将工程内部数据或图件渲染输出为外部标准交换格式（PDF、PNG、GeoTIFF） | 输出 | 全仓标准菜单 |
| Import | 导入 | UI Conventions | 解析外部数据文件（LAS、SEG-Y、Shapefile）并注册进入工程中枢 | 输入 | 全仓标准菜单 |
| Undo | 撤销 | UI Conventions | 回退最近一次用户交互编辑操作栈步进 | 取消（严禁与Cancel混用） | 全仓撤销动作 |
| Redo | 重做 | UI Conventions | 重新执行被撤销操作的栈恢复动作 | 重试（严禁与Retry混用） | 全仓重做动作 |
| Preferences / Settings | 首选项 / 设置 | UI Conventions | 管理系统全局配置、插件、显示风格与快捷键的集中控制面板 | 偏好、喜好 | 全仓设置菜单 |
| Close | 关闭 | UI Conventions | 退出当前停靠面板、标签页或对话框的动作 | 关门 | 全仓通用动作 |
| Help | 帮助 | UI Conventions | 呼出用户使用指南、快捷键清单及软件关于信息的主菜单项 | 援助 | 全仓菜单 |
| Tool / Toolbar | 工具 / 工具栏 | UI Conventions | 执行特定操作的交互模式（如框选、漫游）或承载操作图标的条形容器 | 器具、工具条 | 全仓工具栏 |
| Status Bar | 状态栏 | UI Conventions | 位于主窗口最底部用于显示当前坐标、提示信息和后台任务进度的信息条 | 现状栏 | 全仓状态栏 |

---

## 4. Qt zh_CN 桌面 UI 规范标准

### 4.1 菜单加速键与快捷键格式规范
所有主菜单项与动作必须遵循 Qt 官方 zh_CN 加速键书写契约：
1. **中文字符后紧接半角括号与大写字母**：格式固定为 `功能名称(&字母)`。
   - 示例：`文件(&F)`, `编辑(&E)`, `视图(&V)`, `新建工程(&N)…`, `打开工程(&O)…`, `保存(&S)`, `退出(&X)`。
2. **包含缩写词时加速键置于英文字母前**：
   - 示例：`导出为 &PNG…`, `导出为 &PDF…`, `导出为 &SVG…`。
3. **严禁格式错误**：
   - 错误：`文件 (&F)`（括号前带有空格）
   - 错误：`文件(F)`（缺少 `&` 符号）
   - 错误：`文件（&F）`（使用了全角中文括号）

### 4.2 省略号规范
当点击某个菜单项或按钮会弹出对话框要求用户进一步输入参数才能执行时，必须添加省略号：
1. **必须使用 Unicode 独立省略号字符**：`…` (`U+2026 HORIZONTAL ELLIPSIS`)。
2. **严禁连续输入三个半角点**：禁止使用 `...` (`U+002E * 3`) 或两个点 `..`。
3. **省略号位置固定紧随加速键之后**：
   - 正确：`新建工程(&N)…`, `另存为(&A)…`, `从模板加载(&L)…`
   - 错误：`新建工程…(&N)`, `新建工程(&N)...`

### 4.3 标点符号与全半角规范
1. **UI 标签提示冒号**：
   - 针对中文提示文本，一律使用全角冒号 `：` (`U+FF1A`)。
     - 示例：`断层名称：`, `模型路径：`, `层位：`, `网格大小：`
   - 针对极短的纯坐标单字母代号，允许使用半角冒号加空格：
     - 示例：`X: `, `Y: `, `Z: `
2. **解释说明括号**：
   - 针对中文补充说明文本，一律使用中文全角括号 `（` 和 `）` (`U+FF08`, `U+FF09`)。
     - 示例：`（必填）`, `（可选）`, `（已禁用）`, `（默认）`
   - 针对带单位的数值，或者英文缩写，保留半角括号：
     - 示例：`TVD (m)`, `TWT (ms)`, `孔隙度 (%)`
3. **数值范围波浪号与短横线**：
   - 数值区间提示使用全角波浪号 `～` (`U+FF5E`) 或 En-dash `–` (`U+2013`)：
     - 示例：`1000 ～ 2000 ms`, `X %1–%2, Y %3–%4`。

### 4.4 参数占位符规范 (`%1`..`%9`)
1. **严格保持集合与编号等价**：
   - 源文包含多少个占位符，译文必须包含完全相同数量且编号一致的占位符。
   - 严禁删除 `%1`、严禁将 `%1` 重命名为 `%2`，严禁颠倒逻辑顺序。
2. **中文语境中的空格纪律**：
   - 汉字与占位符之间不需要人为填充多余空格。
     - 正确：`已成功加载模型“%1”`
     - 错误：`已成功加载模型 “ %1 ” `
   - 若占位符两侧原本是英文单词或数值单位，则保留半角空格：
     - 正确：`共 %1 口井`，`进度：%1%`

### 4.5 常用按钮与标准动作中文对照表
| 英文标识符 | 标准中译 | 英文标识符 | 标准中译 |
|-----------|---------|-----------|---------|
| OK | 确定 | Cancel | 取消 |
| Apply | 应用 | Close | 关闭 |
| Reset | 重置 | Restore Defaults | 恢复默认值 |
| Yes | 是 | No | 否 |
| Retry | 重试 | Ignore | 忽略 |
| Abort | 中止 | Discard | 放弃修改 |
| Browse… | 浏览… | Select All | 全选 |
| Deselect All | 全不选 | Invert Selection | 反选 |
| Clear | 清空 | Remove | 移除 |
| Delete | 删除 | Add | 添加 |

---

## 5. 自动化门禁检验格式标准 (`tools/check_l10n.py` 消费规范)

为了满足 `ORIGINAL_REQUEST.md` R5 以及验收指标中「随机抽检 40 条译文与 glossary 零冲突」的要求，本文件第 3 节中所有表格均采用机器可解析格式：
- 列 1：`English Term`（英文字符串，大小写不敏感匹配）
- 列 2：`标准中译 (zh_CN)`（中文标准译名，提取 `/` 前第一个词或指定词组）
- 列 5：`禁用/误译 (Banned/Avoided)`（提取顿号或斜杠分隔的黑名单列表）

门禁脚本 `tools/check_l10n.py` 解析逻辑：
1. 读取 `translations/glossary-zh-CN.md` 中的表格行。
2. 对提取出的 193 组对照规则建立映射字典。
3. 抽检 `.ts` 文件中的 `<translation>` 节点：
   - 若译文命中黑名单中的「禁用/误译」，直接报错退出；
   - 确保核心地质词汇（如层位、断层、井分层、交会图、网格化、层序格架）在对应上下文中 100% 吻合。
