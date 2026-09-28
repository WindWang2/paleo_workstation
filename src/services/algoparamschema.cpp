// 层：数据
// services/algoparamschema.cpp — 见 algoparamschema.h 头注释。字段值逐一对照
// 算法注册表 initAlgorithm 的真值：
//   paleo_geological_smoothing : PASSES        int    def 1    min 0
//   paleo_constraint_idw       : FIELD         string（INPUT 的数值字段名）
//                                FACIES_CODE   int    可选元数据
//                                CELL_SIZE     double def 1.0
//   paleo_facies_fusion        : （仅 INPUTS/OUTPUT 图层型 → 零标量字段）
//   paleo_isopach              : NEGATIVE_TO_NODATA bool → InList 是/否
//   paleo_facies_polygonize    : MIN_AREA      double def 0.0  min 0
//                                SIMPLIFY      double def 0.0  min 0
//                                SNAP_TOLERANCE double def 0.0 min 0
//                                ANGLE_TOLERANCE double def 15.0 min 0 max 90
// bool 没有独立类型词表——按 inlist 呈现（是/否，userData false/true），与
// QgsProcessingParameterBoolean 的取值面一致。
#include "algoparamschema.h"

#include <QCoreApplication>

namespace
{
  QVector<AlgorithmParamField> smoothingFields()
  {
    AlgorithmParamField passes;
    passes.key = QStringLiteral( "PASSES" );
    passes.label = QCoreApplication::translate( "AlgorithmParamSchema", "滤波次数" );
    passes.type = AlgorithmParamField::Int;
    passes.defaultValue = 1;
    passes.hasMin = true;
    passes.minValue = 0.0;
    passes.hasMax = true; // UI 实用上限：算法本体只设 min 0，超大值拖垮整页运行
    passes.maxValue = 99.0;
    return { passes };
  }

  QVector<AlgorithmParamField> constraintIdwFields()
  {
    AlgorithmParamField field;
    field.key = QStringLiteral( "FIELD" );
    field.label = QCoreApplication::translate( "AlgorithmParamSchema", "Z 值字段名" );
    field.type = AlgorithmParamField::String;
    field.defaultValue = QStringLiteral( "z" );
    field.required = true; // 空字段名进不了插值（主线7：页面收集侧拒收）

    AlgorithmParamField faciesCode;
    faciesCode.key = QStringLiteral( "FACIES_CODE" );
    faciesCode.label = QCoreApplication::translate( "AlgorithmParamSchema", "相代码（元数据）" );
    faciesCode.type = AlgorithmParamField::Int;
    faciesCode.defaultValue = 1;
    faciesCode.hasMin = true;
    faciesCode.minValue = 0.0;
    faciesCode.hasMax = true;
    faciesCode.maxValue = 99.0;

    AlgorithmParamField cellSize;
    cellSize.key = QStringLiteral( "CELL_SIZE" );
    cellSize.label = QCoreApplication::translate( "AlgorithmParamSchema", "网格单元大小" );
    cellSize.type = AlgorithmParamField::Double;
    cellSize.defaultValue = 1.0;
    cellSize.hasMin = true; // 正数（算法默认 1.0；0 会导致无穷网格）
    cellSize.minValue = 0.0001;
    cellSize.decimals = 4;

    return { field, faciesCode, cellSize };
  }

  QVector<AlgorithmParamField> isopachFields()
  {
    AlgorithmParamOption no, yes;
    no.label = QCoreApplication::translate( "AlgorithmParamSchema", "否" );
    no.value = false;
    yes.label = QCoreApplication::translate( "AlgorithmParamSchema", "是" );
    yes.value = true;

    AlgorithmParamField negToNodata;
    negToNodata.key = QStringLiteral( "NEGATIVE_TO_NODATA" );
    negToNodata.label = QCoreApplication::translate( "AlgorithmParamSchema", "负厚度置为无数据" );
    negToNodata.type = AlgorithmParamField::InList;
    negToNodata.defaultValue = false;
    negToNodata.options = { no, yes };
    return { negToNodata };
  }

  QVector<AlgorithmParamField> polygonizeFields()
  {
    auto scalar = []( const QString &key, const QString &label, double def,
                      double minV, bool hasMaxV, double maxV, const QString &suffix ) {
      AlgorithmParamField f;
      f.key = key;
      f.label = label;
      f.type = AlgorithmParamField::Double;
      f.defaultValue = def;
      f.hasMin = true;
      f.minValue = minV;
      f.hasMax = hasMaxV;
      f.maxValue = maxV;
      f.decimals = 2;
      f.suffix = suffix;
      return f;
    };
    return {
      scalar( QStringLiteral( "MIN_AREA" ),
              QCoreApplication::translate( "AlgorithmParamSchema", "最小图斑面积" ), 0.0, 0.0,
              false, 0.0, QStringLiteral( " m²" ) ),
      scalar( QStringLiteral( "SIMPLIFY" ),
              QCoreApplication::translate( "AlgorithmParamSchema", "化简容差" ), 0.0, 0.0,
              false, 0.0, QString() ),
      scalar( QStringLiteral( "SNAP_TOLERANCE" ),
              QCoreApplication::translate( "AlgorithmParamSchema", "约束吸附距离" ), 0.0, 0.0,
              false, 0.0, QString() ),
      scalar( QStringLiteral( "ANGLE_TOLERANCE" ),
              QCoreApplication::translate( "AlgorithmParamSchema", "约束角度容差" ), 15.0, 0.0,
              true, 90.0, QStringLiteral( "°" ) ),
    };
  }
} // namespace

bool AlgorithmParamSchema::knows( const QString &algorithmId )
{
  return !fieldsFor( algorithmId ).isEmpty() ||
         algorithmId == QStringLiteral( "paleo:paleo_facies_fusion" );
}

QVector<AlgorithmParamField> AlgorithmParamSchema::fieldsFor( const QString &algorithmId )
{
  if ( algorithmId == QStringLiteral( "paleo:paleo_geological_smoothing" ) )
    return smoothingFields();
  if ( algorithmId == QStringLiteral( "paleo:paleo_constraint_idw" ) )
    return constraintIdwFields();
  if ( algorithmId == QStringLiteral( "paleo:paleo_isopach" ) )
    return isopachFields();
  if ( algorithmId == QStringLiteral( "paleo:paleo_facies_polygonize" ) )
    return polygonizeFields();
  if ( algorithmId == QStringLiteral( "paleo:paleo_facies_fusion" ) )
    return {}; // 仅图层型参数（INPUTS/OUTPUT）——零标量字段
  return {};
}

QStringList AlgorithmParamSchema::registeredAlgorithms()
{
  return {
    QStringLiteral( "paleo:paleo_geological_smoothing" ),
    QStringLiteral( "paleo:paleo_constraint_idw" ),
    QStringLiteral( "paleo:paleo_facies_fusion" ),
    QStringLiteral( "paleo:paleo_isopach" ),
    QStringLiteral( "paleo:paleo_facies_polygonize" ),
  };
}
