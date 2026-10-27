# Goal-Loop Ledger — 方向 19 井轨迹空间化（goal/well-trajectory-20261003）

协议：worktree 分支迭代 → 每轮原子提交 → Oracle 验收记账 → review 后 PR。
基线 origin/master 4741115。

## 轮0 勘察定案（2026-10-03）

### 实测事实（与 prompt 背景的差异以仓库实况为准）

1. **真工区无测斜数据**：`/home/kevin/projects/paleo_project/data/project_area`
   只有 井位/ExportWellHead.dat + 井曲线/*.Las（21 井）+ 井分层/DC.dat + 时深。
   无 deviates/测斜目录与文件 → 轮5 真工区实测 = 无测斜回退面断言（全直井路径不变）。
2. **角色已存在**：`src/catalog/roleregistry.cpp:29` 内置 `trajectory` 角色
   （well 实体、井斜轨迹、primary=当前生效版本）。**定案：复用 `trajectory`，
   不新造 `well_deviation`**（entitypanel/datalist 改角色下拉天然可见）。
3. **XML 解析器现成**：`io/wellcompositexml.h` `parseDeviationSurvey`
   （SpreadsheetML「井斜」工作表：MD | 井斜角 | 方位角）。
   文本站表解析缺 → 新写 `parseDeviationText`（SMI 风格，`# Well :` 井名 +
   -99999 哨兵，同 parseTimeDepthText 口径）。
4. **域模型现状**：`ui/wellcomposite/depthtransform.*`（D6.1）已有 MD→TVD，但
   dogleg 只看井斜差（忽略方位角，2D 近似）且无水平位移；且在视图层——
   workflow/services 依分层护栏不可 include。**定案：新建
   `src/domain/deviationsurvey.*` 完整 3D 最小曲率**（cos β = cos i1 cos i2 +
   sin i1 sin i2 cos ΔA；Δtvd/Δnorth/Δeast 三分量 + RF 比率因子）。
   视图层 DepthTransform 不动（综合图 MD 主显 + TVD 尾缀已达标，见 7）。
5. **消费面现状**：
   - **propmodel**：`algorithms/stratgrid/upscale.cpp` 已是通用轨迹求交
     （站段 x/y/z marching + 网格线/层面分割），直井假设只在
     `workflow/propertymodelworkflow.cpp:730-740` 的 stationAt
     （x/y 恒井口、z=MD）。→ 换成 trajectoryFor 站表。
   - **剖面**：canvas 已有两点轨迹线（wellhead bottomX/Y 投影；
     sectionworkbench 从不填 bottomX/Y，恒垂线简化）。→ SectionWellInfo 增
     轨迹站序列，dock 逐站投影，canvas 画折线。
   - **平面**：wells.geojson 只有井位点（catalog 层禁区不动）→ 组装根新增
     well_trajectories.geojson 线层（surface→TD 投影，无轨迹不写层）。
   - **3D**：`ui/seismic3d` Seismic3DWell.trajectory 渲染支持已在，但
     setWells 生产无喂数通道（仅测试喂数）→ **递延记档**（属 3D 喂数面）。
   - **综合图**：D6.1-6.5 已给 MD 主显 + TVD 读数尾缀 + TWT 副刻度，
     derivedsink 生产接线在 main.cpp:108-129 → 现状即目标形态，验证即可。
   - **welllogset**：头契约「只读 ~C 头不读数据体」→ 新增 `readCurveTvd`
     显式扩展契约（读指定列数据体 + survey 逐点 MD→TVD；协议/别名不动）。
6. **导入链路定案**：分类类型新增 `well_deviation`（AreaRules datPathRule：
   目录段 dev/测斜/井斜 + 文件名 deviation/trajectory/井斜；sniffWellXml 加
   「井斜」工作表判据；projectClassifierTypes 词表）→ ingestplan roleForType
   映射 `trajectory`；dataimportservice 落链接照抄 tops/time_depth 分支
   （井名 `# Well :`/文件名主名回退、不建井、primary、unresolved 诚实面）。

### API 签名定案

```cpp
// src/domain/deviationsurvey.h（层：数据）
namespace paleo {
struct DeviationStation { double md=0, inclinationDeg=0, azimuthDeg=0; };
struct TrajectoryPoint  { double md=0, tvd=0, north=0, east=0; }; // 相对井口位移
class WellDeviationSurvey {
 public:
  static std::optional<WellDeviationSurvey> fromStations(
      QVector<DeviationStation> stations, QString *error = nullptr);
  bool isValid() const;
  bool isVertical() const;                 // 全站 incl≈0
  const QVector<DeviationStation> &stations() const;
  const QVector<TrajectoryPoint> &points() const;   // 站点累计位置（最小曲率）
  double totalDepth() const;               // 末站 MD
  TrajectoryPoint pointAt(double md) const; // 站间子段最小曲率；表前/后按端站角度直线延伸
  double tvdAt(double md) const;
  double tvdToMd(double tvd) const;         // 站表线性反插（TVD 随 MD 单调时唯一）
};
}
```

```cpp
// services/projectdata.h 同族接口
std::optional<paleo::WellDeviationSurvey> trajectoryFor(const QString &wellId) const;
// primary trajectory 链接 → 文件内容嗅探（.xml→parseDeviationSurvey；文本→
// parseDeviationText）→ fromStations。无链接/解析失败/无效 → nullopt +
// lastError 记因（直井回退是显式语义）。

// services/welllogset.h TVD 域读法（协议不动，新增读体例外）
struct WellCurveTvdSamples { QString path; int column; QVector<double> md, tvd, values; };
static bool readCurveTvd(const WellCurveRef &ref,
                         const paleo::WellDeviationSurvey *survey, // null/无效 → tvd≡md
                         WellCurveTvdSamples *out, QString *error = nullptr);
```

### 轮次计划（照 prompt，按实况微调）

- 轮1 域模型 + 最小曲率 + `tst_deviation`（数值断言：垂直恒等/90° 水平/
  造斜-稳斜手算对拍/往返/误差面）。
- 轮2 导入 + trajectory 角色链接 + `tst_deviation_import`（XML/文本双格式、
  unresolved、分类判据）。
- 轮3 trajectoryFor + readCurveTvd + tst_welllogset 扩面（恒等/非恒等/多文件）。
- 轮4 消费面迁移：propmodel stationAt 换轨迹 + tst（定向穿层 > 直井）；
  剖面 SectionWellInfo 轨迹 + tst_sectiontrajectory；平面轨迹线层；
  综合图验证现状；3D 记递延。
- 轮5 真工区回退面（PALEO_REAL_PROJECT_AREA 门控）+ progress 收口。

## 轮1（域模型 + 最小曲率）

- 改动：`src/domain/deviationsurvey.{h,cpp}`（paleo_domain）+
  `cmake/extra-well-trajectory.cmake`（源登记 + tst_deviation）+
  根 CMakeLists `_paleo_extra_manifest` 登记。
- 实现：完整 3D 最小曲率（cos β = cos i1 cos i2 + sin i1 sin i2 cos ΔA，
  RF = 2/β·tan(β/2)，tvd/north/east 三分量）；站间子段参数化（t=1 恒等整段，
  站点 MD 精确命中免 ULP）；首站前/末站后按端站姿态直线；tvdToMd 二分数值
  反解（往返双精度收敛——初版站表线性反插在 1200m 站距 40° 造斜段往返差
  ~20m，实测后换掉）。
- 验证：`ctest --test-dir build -R tst_deviation` → 9/9 PASS（垂直恒等/
  90° 造斜-稳斜闭式手算 2000/π/子段手算 RF·cos45/方位全狗腿 4π RF/
  往返 1e-6/误差面 4 类/外延边界）。
- 分层：`check_layering.py --strict` 绿（exit 0，三次稳定）。
- 判定：轮1 Oracle（数值断言 1a-1e + 全狗腿口径钉死）达成。
- 下一步：轮2 导入链路。

## 轮2（导入 + trajectory 角色链接）

- 改动：domain/wellrecords（DeviationTable 记录 + WellXmlKind::WellDeviation
  嗅探「井斜*」工作表前缀，测井曲线表优先级保持）、arearules 默认表尾追加
  deviation 判据（目录段 dev/测斜/井斜 + 文件名 deviation/trajectory/井斜，
  既有四规则零扰动）、projectclassifier 词表 + xml 分流、
  io/wellfileparsers parseDeviationText（'# Well :' 井名 + 三列全有效才成站
  + -99999 哨兵整行弃）、ingestplan（roleForType→trajectory +
  proposeDeviation）、dataimportservice（well_deviation 分支照抄时深：每井名
  一条 primary 链接 / 未决不建井）、tests（tst_deviation_import +
  tst_arearules 钉死表更新至 5 规则）。
- 注：trajectory 角色在 RoleRegistry 非有序（ordered=false）——无 ordinal 面
  （井 log 的 ordinal 是 well_log 专属协议）；prompt 所指 nextOrdinalForRole
  先例在井场 XML 家族不存在，如实记。
- 验证：tst_deviation_import 8/8 PASS（分类判据含零误判面/XML 双优先级/
  文本解析含哨兵行/导入 primary 链接 + 受管回读对拍/未决不建井/plan 期
  双候选歧义）；tst_arearules 10/10（含新规则钉死）；tst_ingestplan、
  tst_domain、tst_deviation 回归绿；分层 strict 绿。
- 判定：轮2 Oracle（导入双格式 + 角色 + 未决诚实面）达成。
- 下一步：轮3 trajectoryFor/readCurveTvd。

## 轮3（MD/TVD 域服务）

- 改动：services/projectdata 增 trajectoryFor（tdTableFor 同族：primary
  trajectory 链接 → .xml 走 parseDeviationSurvey / 文本走 parseDeviationText
  → WellDeviationSurvey::fromStations；无链接 nullopt 不记错——直井回退是
  显式语义；坏文件/坏站表 nullopt + lastError 记因）。services/welllogset 增
  readCurveTvd（按列读数据体的显式例外，头注释更新；ft 折米、未知单位报错
  不猜；survey 空/无效 → tvd≡md 恒等）。tests/tst_welllogset_tvd（10 用例）。
- 验证：tst_welllogset_tvd 10/10 PASS（未链接 nullopt+无错/文本站表闭式
  对拍/XML 主链接语义/坏表拒绝/恒等/非恒等+闭式锚点/多文件逐文件对拍/
  列 0·越界·单位诚实面·ft 折算）；tst_welllogset/tst_projectdata/
  tst_sections_alignment 回归绿；分层 strict 绿。
- 判定：轮3 Oracle（MD/TVD 映射断言 + 多文件各自映射 + 直井回退显式）达成。
- 下一步：轮4 消费面迁移。

## 轮4（消费面迁移）

- 改动：
  - **propmodel**（propertymodelworkflow requestFromCatalog）：有测斜 →
    井筒站 = 曲线端点 + 区间内测斜站（x/y=井口+位移、z=TVD，严格递增去
    重）；无测斜 → 原垂直路径不动。请求结构体加 trajectoryWellCount，
    provenance 记 n_trajectory_wells（paramHash 天然含站点坐标）。
  - **剖面**：domain SectionWellInfo 增 WellTrajSample 轨迹折线（md/tvd/
    x/y/twtMs）；SectionWorkbench::sectionWells 填充（twt 优先 TVD 校准回
    退 MD）并补 bottomX/Y；dock computeWellTrajectories 逐站投影成
    TrajVertex；canvas 顶点 ≥2 画折线（halo+墨线），否则原两点简化。
  - **平面**：workflow/welltrajectorylayer 纯函数组装 GeoJSON（surface→
    站点→TD 投影 LineString；坏测斜记 error 不静默降级）；组装根
    AppContext::refreshWellTrajectoriesLayer 落盘+声明+上图
    （00_Data「井轨迹投影」）；QgisStyleService::applyTrajectoryLayerStyle
    （DESIGN.md text 墨色 #24303E 实线 0.6——实测轨迹不用虚线）。
  - **综合图**：DepthTransform 的 MD↔TVD 委托域模型（原 2D 近似 dogleg 忽
    略方位；单站外延旧实现还有越界隐患）；站表坏 → 禁用 + 原因文案（不静
    默）。公共 API 不变，D6.1 既有测试零改动通过。
  - **3D**：递延——Seismic3DWell.trajectory 渲染面已在但 setWells 生产无
    喂数通道（本仓现状仅测试喂数），接线属 seismic3d 喂数面工作。
- 测试：tst_sectiontrajectory 6/6（剖面轨迹闭式对拍/投影横向距离/无测斜
  垂直简化不变/平面线端点=井底投影/无测斜空产出）；tst_propworkflow 扩
  deviatedWellUsesTrajectoryStations +13/13（站表闭式/穿柱 ≥3 vs 1/井口
  死柱判别器：直井无值、定向井中深层有值 6.0/代表柱离开井口柱）+
  既有槽补 trajectoryWellCount==0（老工程迁移不变面）。
- 验证：17 项回归全绿（propmodel 系/剖面系/综合图系/welllogset 系/
  upscale/deviation 系/arearules/ingestplan）；分层 strict 绿。
- 判定：轮4 Oracle（消费面 4 条 + 迁移不变）达成；3D 如实递延记档。
- 下一步：轮5 真工区回退面 + progress 收口。

## 轮5（真工区回退面 + 收口）

- 改动：tests/tst_deviation_realarea（PALEO_REAL_PROJECT_AREA 门控；
  未设 env QSKIP——CI 不红）+ docs/progress/well-trajectory.md（公式口径/
  插值策略/角色与域切换面/消费面迁移清单/递延）。
- 验证（真机实测，env 指向 /home/kevin/projects/paleo_project/data/project_area）：
  `PALEO_REAL_PROJECT_AREA=... ctest -R tst_deviation_realarea` → PASS：
  20 井全部 trajectoryFor nullopt（无测斜数据回退面）、坏链接 0、平面轨迹
  层空产出（error 空——「没有测斜」不是坏数据）、剖面 sectionWells 每井
  trajectory 空（垂直简化保持）。Oracle 5/6（无测斜井回退显式 + 老工程
  行为不变）达成。
- 判定：方向 19 全 Oracle 达成，进收尾硬门禁（全量 review/构建/ctest/分层）。

## 收尾硬门禁（2026-10-03）

- **全量构建**：`cmake --build build -j8` 全目标 exit 0；非 vendor 警告仅在
  未触碰的既有测试文件（tst_catalog/tst_projectsvc/tst_services2，master 同在），
  本方向全部新增/改动文件零警告。
- **全量 ctest**：199 项，195 绿。4 失败经 master 基线复测定性，全部与本方向
  无关：
  - tst_import `failingCommitSurfacesErrorAndRollsBack`——master 同败（本机
    目录只读权限不生效的沙箱环境，测试自带的 root-QSKIP 探针测不出）；
  - tst_catalogstore `goldenJsonMigratesFieldForField`——master 同败；
  - tst_perf_catalog——并行负载抖动，串行重跑 PASS；
  - tst_singlefactor_perf（16.1s vs 15s 预算）——master 同败 16.3s，负载
    敏感且属单因素方向（与本方向零交集）。
- **分层**：`check_layering.py --strict` 绿（exit 0）。
- **diff 全量自审**：无调试残留（qDebug/TODO 扫描净）；层标记齐；自审修正
  3 处（AREA_PARAMETERS 默认表同步、DepthTransform 旧口径注释、welllogset.cpp
  契约注释），见末次 docs 提交。
- **本方向测试面**：tst_deviation 9/9、tst_deviation_import 8/8、
  tst_welllogset_tvd 10/10、tst_sectiontrajectory 6/6、tst_propworkflow 13/13
  （含定向井新槽）、tst_arearules 10/10（钉死表 5 规则）、
  tst_deviation_realarea 真机 PASS（20 井回退面）。
- 判定：门禁过，push + PR。
