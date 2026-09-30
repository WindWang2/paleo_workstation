// 层：数据
#pragma once
#include <qgsprocessingalgorithm.h>
#include <qgsprocessingprovider.h>

// algorithms/ — production Paleo Processing algorithms (C++ only, no Python).
// Registered on PaleoProvider (id "paleo"), distinct from the spikes provider.

// ConstraintIDW: IDW interpolation of point z-values honoring typed constraint
// line geometries (C1 semantics, 'type' column of the ConstraintStore layer):
//   · break_line — hard barrier: ROI hull contribution + grid-reachability
//     blocking (a cell only interpolates from samples on its side of the
//     barrier; barrier cells become nodata; detours around barrier ends are
//     honored at cell resolution);
//   · direction_line — anisotropy axis: length-weighted mean direction θ,
//     effective distance d² = u²/r² + v²·r² (u along θ, v across, r=ANISO_RATIO)
//     elongating the surface along the supply direction; no clip, no barrier;
//   · other/missing type — legacy convex-hull ROI clip only (pre-C1 behavior).
// Params: INPUT (points, z-field), CONSTRAINTS (lines), FACIES_CODE (int),
//         CELL_SIZE (double), ANISO_RATIO (double, optional, default 2.0),
//         OUTPUT (raster destination).
class ConstraintIDWAlgorithm : public QgsProcessingAlgorithm
{
  public:
    QString name() const override { return QStringLiteral("paleo_constraint_idw"); }
    QString displayName() const override { return QStringLiteral("Paleo: Constraint IDW"); }
    QString group() const override { return QStringLiteral("Single factor"); }
    QString groupId() const override { return QStringLiteral("singlefactor"); }
    QString shortHelpString() const override;
    ConstraintIDWAlgorithm *createInstance() const override { return new ConstraintIDWAlgorithm(); }
    void initAlgorithm(const QVariantMap &configuration = QVariantMap()) override;
    QVariantMap processAlgorithm(const QVariantMap &parameters, QgsProcessingContext &context,
                                 QgsProcessingFeedback *feedback) override;
};

// FaciesFusion: fuse N single-facies rasters into one coded facies raster.
// Priority order = parameter order; cell takes value of highest-priority
// raster whose cell is non-null/nonzero.
// Params: INPUTS (multiple rasters), OUTPUT.
class FaciesFusionAlgorithm : public QgsProcessingAlgorithm
{
  public:
    QString name() const override { return QStringLiteral("paleo_facies_fusion"); }
    QString displayName() const override { return QStringLiteral("Paleo: Facies Fusion"); }
    QString group() const override { return QStringLiteral("Composition"); }
    QString groupId() const override { return QStringLiteral("composition"); }
    QString shortHelpString() const override;
    FaciesFusionAlgorithm *createInstance() const override { return new FaciesFusionAlgorithm(); }
    void initAlgorithm(const QVariantMap &configuration = QVariantMap()) override;
    QVariantMap processAlgorithm(const QVariantMap &parameters, QgsProcessingContext &context,
                                 QgsProcessingFeedback *feedback) override;
};

// GeologicalSmoothing: majority-filter a coded raster (mode in 3x3 window),
// preserving coded values (no interpolation across facies codes).
// Params: INPUT (raster), PASSES (int), OUTPUT.
class GeologicalSmoothingAlgorithm : public QgsProcessingAlgorithm
{
  public:
    QString name() const override { return QStringLiteral("paleo_geological_smoothing"); }
    QString displayName() const override { return QStringLiteral("Paleo: Geological Smoothing"); }
    QString group() const override { return QStringLiteral("Composition"); }
    QString groupId() const override { return QStringLiteral("composition"); }
    QString shortHelpString() const override;
    GeologicalSmoothingAlgorithm *createInstance() const override { return new GeologicalSmoothingAlgorithm(); }
    void initAlgorithm(const QVariantMap &configuration = QVariantMap()) override;
    QVariantMap processAlgorithm(const QVariantMap &parameters, QgsProcessingContext &context,
                                 QgsProcessingFeedback *feedback) override;
};

// IsopachAlgorithm: thickness = top − base over two structural surface rasters.
// Where either input is nodata the output is nodata; a NEGATIVE_TO_NODATA flag
// optionally clamps inverted (base above top) cells to nodata — inverted
// thickness usually signals overlapping/mispicked surfaces rather than real
// negative thickness.
// Params: INPUT_TOP, INPUT_BASE (rasters, same grid), NEGATIVE_TO_NODATA (bool),
//         OUTPUT (raster destination).
class IsopachAlgorithm : public QgsProcessingAlgorithm
{
  public:
    QString name() const override { return QStringLiteral("paleo_isopach"); }
    QString displayName() const override { return QStringLiteral("Paleo: Isopach (thickness)"); }
    QString group() const override { return QStringLiteral("Single factor"); }
    QString groupId() const override { return QStringLiteral("singlefactor"); }
    QString shortHelpString() const override;
    IsopachAlgorithm *createInstance() const override { return new IsopachAlgorithm(); }
    void initAlgorithm(const QVariantMap &configuration = QVariantMap()) override;
    QVariantMap processAlgorithm(const QVariantMap &parameters, QgsProcessingContext &context,
                                 QgsProcessingFeedback *feedback) override;
};

// FaciesPolygonize — §36 raster → editable facies polygons.
// Recode → drop sub-threshold parts on the grid → GDALPolygonize → dissolve →
// GEOS coverage simplify (each shared edge once) → boundary-graph rebuild so
// both faces reference the same arc → optional constraint conflation.
// Params: INPUT (raster), MIN_AREA, SIMPLIFY, SNAP_TOLERANCE, ANGLE_TOLERANCE,
//         CONSTRAINTS (optional lines/polygons/points), OUTPUT (vector).
class FaciesPolygonizeAlgorithm : public QgsProcessingAlgorithm
{
  public:
    QString name() const override { return QStringLiteral("paleo_facies_polygonize"); }
    QString displayName() const override { return QStringLiteral("Paleo: Facies polygons"); }
    QString group() const override { return QStringLiteral("Composition"); }
    QString groupId() const override { return QStringLiteral("composition"); }
    QString shortHelpString() const override;
    FaciesPolygonizeAlgorithm *createInstance() const override { return new FaciesPolygonizeAlgorithm(); }
    void initAlgorithm(const QVariantMap &configuration = QVariantMap()) override;
    QVariantMap processAlgorithm(const QVariantMap &parameters, QgsProcessingContext &context,
                                 QgsProcessingFeedback *feedback) override;
};

// PaleoWellDistance — welldist 单因素核（T11）：输出栅格每格值 = 该格中心
// 到最近井点的精确欧氏距离（INPUT 的 CRS 单位）。网格 = 输入范围四边外扩
// 10%（与 ConstraintIDW 同一约定；退化轴补一格防零宽）。精确直算
// O(rows*cols*points)——井点规模（工区级几十~几百口）下毫秒到秒级，不做
// cell-snake 近似；大点集的空间索引/两遍 EDT 是文档化优化路径。
// 契约为草案（paleo:paleo_welldist）——单因素注册表（singlefactordef/
// algoparamschema，编图方向所有）尚未翻转 processingAlgId；参数面无 FIELD
// （距离不需要属性值）。
// Params: INPUT (wells point layer), CELL_SIZE (double), OUTPUT (raster).
class PaleoWellDistanceAlgorithm : public QgsProcessingAlgorithm
{
  public:
    QString name() const override { return QStringLiteral("paleo_welldist"); }
    QString displayName() const override { return QStringLiteral("Paleo: Well distance"); }
    QString group() const override { return QStringLiteral("Single factor"); }
    QString groupId() const override { return QStringLiteral("singlefactor"); }
    QString shortHelpString() const override;
    PaleoWellDistanceAlgorithm *createInstance() const override { return new PaleoWellDistanceAlgorithm(); }
    void initAlgorithm(const QVariantMap &configuration = QVariantMap()) override;
    QVariantMap processAlgorithm(const QVariantMap &parameters, QgsProcessingContext &context,
                                 QgsProcessingFeedback *feedback) override;
};

// PaleoDistanceTransform — welldist 单因素的绕障距离引擎（C5；id/参数面按
// SingleFactorContracts::welldistEngineId() 冻结契约）：无 CONSTRAINTS 时
// 逐格精确欧氏距离（与 paleo_welldist 同口径）；CONSTRAINTS 含 break_line
// 时为 8 邻接 Dijkstra 栅格路径距离（边权 1/√2 格，井格多源起 0；屏障格与
// 无路可达格 → nodata）。direction_line/无 type 的约束线对距离面不参与
// （与 ConstraintIDW 的 typed 语义一致）。
// Params: INPUT (wells point layer), CONSTRAINTS (lines, optional),
//         CELL_SIZE (double), OUTPUT (raster destination).
class PaleoDistanceTransformAlgorithm : public QgsProcessingAlgorithm
{
  public:
    QString name() const override { return QStringLiteral("paleo_distance_transform"); }
    QString displayName() const override { return QStringLiteral("Paleo: Distance transform"); }
    QString group() const override { return QStringLiteral("Single factor"); }
    QString groupId() const override { return QStringLiteral("singlefactor"); }
    QString shortHelpString() const override;
    PaleoDistanceTransformAlgorithm *createInstance() const override { return new PaleoDistanceTransformAlgorithm(); }
    void initAlgorithm(const QVariantMap &configuration = QVariantMap()) override;
    QVariantMap processAlgorithm(const QVariantMap &parameters, QgsProcessingContext &context,
                                 QgsProcessingFeedback *feedback) override;
};

// Production provider — replaces the spikes provider for real algorithms.
class PaleoProvider : public QgsProcessingProvider
{
  public:
    QString id() const override { return QStringLiteral("paleo"); }
    QString name() const override { return QStringLiteral("Paleo Workbench"); }
    void loadAlgorithms() override;
};
