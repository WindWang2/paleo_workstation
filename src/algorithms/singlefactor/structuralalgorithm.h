// 层：数据
#pragma once
#include <qgsprocessingalgorithm.h>

// 上游 drawing/single_factor/workflow.py 默认方法 structural_idw
// （「IDW 方向线与打断约束」）的 Processing 入口。
// 采集/面核在 wellacquisition.cpp / structural.cpp，这里只读图层并写栅格。
// 层：数据

class StructuralIdwAlgorithm : public QgsProcessingAlgorithm
{
  public:
    QString name() const override { return QStringLiteral( "paleo_structural_idw" ); }
    QString displayName() const override { return QStringLiteral( "Paleo: Structural IDW" ); }
    QString group() const override { return QStringLiteral( "Single factor" ); }
    QString groupId() const override { return QStringLiteral( "singlefactor" ); }
    QString shortHelpString() const override;
    StructuralIdwAlgorithm *createInstance() const override { return new StructuralIdwAlgorithm(); }
    void initAlgorithm( const QVariantMap &configuration = QVariantMap() ) override;
    QVariantMap processAlgorithm( const QVariantMap &parameters, QgsProcessingContext &context,
                                  QgsProcessingFeedback *feedback ) override;
};
