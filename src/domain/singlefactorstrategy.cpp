// 层：数据
#include "singlefactorstrategy.h"

#include <algorithm>

// 层：数据
namespace paleo::singlefactor
{
namespace
{

QVector<SurfaceMethodPack> makeSurfacePacks()
{
  QVector<SurfaceMethodPack> packs;
  packs.append( SurfaceMethodPack{
      QStringLiteral( "structural_idw" ),
      QStringLiteral( "IDW 方向线与打断约束" ),
      QStringLiteral( "gridded" ),
      QStringLiteral( "paleo:paleo_structural_idw" ),
      true,
      true,
      true,
      QStringLiteral( "解释边界平滑减弱跨线井点影响，有限原始线段决定作用范围；"
                      "自动延伸不作为地质证据。底图与新生成沉积相使用调整场；"
                      "发生穿线处用独立局部提线场绕行，图层记录数值来源，原始井值不变。" ) } );
  packs.append( SurfaceMethodPack{
      QStringLiteral( "surfer_idw" ),
      QStringLiteral( "IDW 全局（Surfer 对照）" ),
      QStringLiteral( "gridded" ),
      QStringLiteral( "paleo:paleo_surfer_idw" ),
      true,
      true,
      true,
      QStringLiteral( "全域采用可达井点的标准距离加权；有限打断线增加绕行距离。"
                      "全局各向异性由比值和角度控制；不附加方向脊、残差圆斑或背景回填。" ) } );
  packs.append( SurfaceMethodPack{
      QStringLiteral( "local_direction_idw" ),
      QStringLiteral( "本地方向插值" ),
      QStringLiteral( "gridded" ),
      QStringLiteral( "paleo:paleo_local_direction_idw" ),
      true,
      false,
      true,
      QStringLiteral( "连续局部曲线核 + 硬屏障栅格连通 + 覆盖标记；"
                      "方向线/软边界只改正权重。" ) } );
  packs.append( SurfaceMethodPack{
      QStringLiteral( "kriging" ),
      QStringLiteral( "克里金（各向异性）" ),
      QStringLiteral( "gridded" ),
      QStringLiteral( "paleo:geostat_kriging" ),
      false,
      true,
      true,
      QStringLiteral( "变差函数自动/显式拟合 + 普通克里金局部邻域求解（geostat 核）。"
                      "有效样本不足阈值时如实回落 IDW 并记 method_actual。" ) } );
  packs.append( SurfaceMethodPack{
      QStringLiteral( "local_direction_kriging" ),
      QStringLiteral( "克里金（局部方向约束）" ),
      QStringLiteral( "gridded" ),
      QStringLiteral( "paleo:paleo_local_direction_kriging" ),
      true,
      false,
      true,
      QStringLiteral( "同一本地方向插值面（成图域/硬屏障分量/覆盖标记）上用普通克里金"
                      "权重；方向线与软边界不参与克里金权重并逐条列入 issues。" ) } );
  packs.append( SurfaceMethodPack{
      QStringLiteral( "idw" ),
      QStringLiteral( "IDW 反距离加权" ),
      QStringLiteral( "gridded" ),
      QStringLiteral( "paleo:paleo_constraint_idw" ),
      true,
      false,
      true,
      QStringLiteral( "局部控制强，适合井较密的工区（本仓旧约束 IDW 保持缺省口径）。" ) } );
  return packs;
}

} // namespace

const QVector<SurfaceMethodPack> &surfaceMethodPacks()
{
  static const QVector<SurfaceMethodPack> packs = makeSurfacePacks();
  return packs;
}

const SurfaceMethodPack *surfaceMethodPack( const QString &id )
{
  for ( const SurfaceMethodPack &pack : surfaceMethodPacks() )
  {
    if ( pack.id == id )
      return &pack;
  }
  return nullptr;
}

const QVector<ContourExtractPack> &contourExtractPacks()
{
  static const QVector<ContourExtractPack> packs = {
    ContourExtractPack{ QStringLiteral( "partitioned_marching_squares" ),
                        QStringLiteral( "分区 Marching Squares（止于打断线）" ),
                        QStringLiteral( "趋势面网格 → 双线性密化 → 按分区 masked marching squares "
                                        "提线 → 同级断口桥接 → 近环闭合 → 制图级平滑 → 裁至有效域。" ),
                        QStringLiteral( "恩平类砂地比图首选：等值线在分区内连续闭合，在打断线/"
                                        "图幅边界处终止，避免跨断层或岩性界线硬连。" ),
                        true,
                        true,
                        true,
                        4,
                        3,
                        0.04,
                        1.5,
                        12.0,
                        QStringList{ QStringLiteral( "smoothing_iterations" ) },
                        QStringList{ QStringLiteral( "upsample_factor" ),
                                     QStringLiteral( "bridge_gap_ratio" ),
                                     QStringLiteral( "min_contour_length_ratio" ) } },
    ContourExtractPack{ QStringLiteral( "marching_squares_raw" ),
                        QStringLiteral( "标准 MS（原始折线，无后处理）" ),
                        QStringLiteral( "趋势面网格 → masked marching squares → 直接输出折线，"
                                        "不做密化/平滑/简化。" ),
                        QStringLiteral( "用于核对插值结果或检查虚假等值线来源；线条呈网格阶梯状。" ),
                        true,
                        true,
                        true,
                        1,
                        0,
                        0.0,
                        0.0,
                        0.0,
                        QStringList{},
                        QStringList{ QStringLiteral( "upsample_factor" ) } },
    ContourExtractPack{ QStringLiteral( "marching_squares_dense" ),
                        QStringLiteral( "密化 MS（仅上采样，不平滑）" ),
                        QStringLiteral( "趋势面网格 → 4× 双线性密化 → masked marching squares → "
                                        "桥接 → 裁至有效域。" ),
                        QStringLiteral( "在保留网格几何忠实度的同时减轻阶梯锯齿，适合井点较密工区。" ),
                        true,
                        true,
                        true,
                        4,
                        0,
                        0.0,
                        1.0,
                        10.0,
                        QStringList{},
                        QStringList{ QStringLiteral( "upsample_factor" ),
                                     QStringLiteral( "bridge_gap_ratio" ),
                                     QStringLiteral( "min_contour_length_ratio" ) } },
    ContourExtractPack{ QStringLiteral( "marching_squares_cartographic" ),
                        QStringLiteral( "制图规范 MS（密化 + 简化 + 平滑）" ),
                        QStringLiteral( "趋势面网格 → 4× 密化 → masked marching squares → "
                                        "同级桥接 → RDP 轻简化 → Chaikin/滑动平均平滑 → "
                                        "近环闭合 → 碎段清理 → 裁至有效域。" ),
                        QStringLiteral( "出版级等值线外观：线条更顺滑、嵌套闭合更好，"
                                        "适合汇报图件与打印。" ),
                        true,
                        true,
                        true,
                        4,
                        3,
                        0.10,
                        2.0,
                        14.0,
                        QStringList{ QStringLiteral( "smoothing_iterations" ),
                                     QStringLiteral( "simplify_tolerance_ratio" ) },
                        QStringList{ QStringLiteral( "upsample_factor" ),
                                     QStringLiteral( "bridge_gap_ratio" ),
                                     QStringLiteral( "min_contour_length_ratio" ) } } };
  return packs;
}

const ContourExtractPack *contourExtractPack( const QString &id )
{
  for ( const ContourExtractPack &pack : contourExtractPacks() )
  {
    if ( pack.id == id )
      return &pack;
  }
  return nullptr;
}

ContourExtractParameters resolveContourExtract( const ContourExtractPack &pack, double gridStep )
{
  ContourExtractParameters resolved;
  resolved.methodId = pack.id;
  const double step = std::max( gridStep, 1e-9 );
  resolved.upsampleFactor = std::max( 1, pack.upsampleFactor );
  resolved.smoothingIterations = std::max( 0, pack.smoothingIterations );
  resolved.simplifyTolerance = std::max( 0.0, step * pack.simplifyToleranceRatio );
  resolved.bridgeGap = std::max( 0.0, step * pack.bridgeGapRatio );
  resolved.minContourLength = std::max( 0.0, step * pack.minContourLengthRatio );
  resolved.clipToSurface = pack.clipToSurface;
  return resolved;
}

const QVector<CartographicWorkPack> &cartographicWorkPacks()
{
  static const QVector<CartographicWorkPack> packs = {
    CartographicWorkPack{ QStringLiteral( "v17_local_interpretive_detour" ),
                          QStringLiteral( "版本17 局部解释绕行工作场" ),
                          QStringLiteral( "17" ),
                          QStringLiteral( "local_interpretive_detour" ),
                          30.0,
                          12.0,
                          true,
                          true,
                          QStringLiteral( "只改实际穿线的局部区域；原分析场字节/SHA 不变，"
                                          "工作场单独登记且禁止进入定量消费。" ) },
    CartographicWorkPack{ QStringLiteral( "analysis_numeric_only" ),
                          QStringLiteral( "仅数值提线（不生成工作场）" ),
                          QString(),
                          QString(),
                          0.0,
                          0.0,
                          false,
                          true,
                          QStringLiteral( "从分析场直接提线，允许自然开放端点；"
                                          "不生成制图工作场，也不声明制图图层。" ) } };
  return packs;
}

const CartographicWorkPack *cartographicWorkPack( const QString &id )
{
  for ( const CartographicWorkPack &pack : cartographicWorkPacks() )
  {
    if ( pack.id == id )
      return &pack;
  }
  return nullptr;
}

} // namespace paleo::singlefactor
