// 层：数据
#pragma once
#include "types.h"

#include <qgscoordinatereferencesystem.h>
#include <qgsprocessingalgorithm.h>
#include <qgsrasterlayer.h>

#include <QString>
#include <vector>

// 本地方向 IDW 与制图工作场的 Processing 入口。
// 数值核在 singlefactor/，这里只读图层并写栅格。
// 层：数据

// 方向84（D3）：协变量栅格按井位逐口采样（波段 1，最近像元；nodata/域外 →
// NaN；CRS 双侧有效且不一致时经 QGIS 变换，失败如实报 error）。算法与回归
// 测试共用（合成栅格断言 nodata/域外路径）。
std::vector<paleo::singlefactor::Sample> sampleCovariateAtWells(
    QgsRasterLayer *raster, const std::vector<paleo::singlefactor::Sample> &wells,
    const QgsCoordinateReferenceSystem &wellCrs, QString *error );

class LocalDirectionIdwAlgorithm : public QgsProcessingAlgorithm
{
  public:
    QString name() const override { return QStringLiteral( "paleo_local_direction_idw" ); }
    QString displayName() const override { return QStringLiteral( "Paleo: Local direction IDW" ); }
    QString group() const override { return QStringLiteral( "Single factor" ); }
    QString groupId() const override { return QStringLiteral( "singlefactor" ); }
    QString shortHelpString() const override;
    LocalDirectionIdwAlgorithm *createInstance() const override { return new LocalDirectionIdwAlgorithm(); }
    void initAlgorithm( const QVariantMap &configuration = QVariantMap() ) override;
    QVariantMap processAlgorithm( const QVariantMap &parameters, QgsProcessingContext &context,
                                  QgsProcessingFeedback *feedback ) override;
};

class CartographicWorkAlgorithm : public QgsProcessingAlgorithm
{
  public:
    QString name() const override { return QStringLiteral( "paleo_cartographic_work" ); }
    QString displayName() const override { return QStringLiteral( "Paleo: Cartographic work field" ); }
    QString group() const override { return QStringLiteral( "Single factor" ); }
    QString groupId() const override { return QStringLiteral( "singlefactor" ); }
    QString shortHelpString() const override;
    CartographicWorkAlgorithm *createInstance() const override { return new CartographicWorkAlgorithm(); }
    void initAlgorithm( const QVariantMap &configuration = QVariantMap() ) override;
    QVariantMap processAlgorithm( const QVariantMap &parameters, QgsProcessingContext &context,
                                  QgsProcessingFeedback *feedback ) override;
};

// Surfer 式全局 IDW：断层绕行测地距离替代「不可见即丢弃」（faultpath 内核）。
class SurferIdwAlgorithm : public QgsProcessingAlgorithm
{
  public:
    QString name() const override { return QStringLiteral( "paleo_surfer_idw" ); }
    QString displayName() const override { return QStringLiteral( "Paleo: Surfer IDW (fault path)" ); }
    QString group() const override { return QStringLiteral( "Single factor" ); }
    QString groupId() const override { return QStringLiteral( "singlefactor" ); }
    QString shortHelpString() const override;
    SurferIdwAlgorithm *createInstance() const override { return new SurferIdwAlgorithm(); }
    void initAlgorithm( const QVariantMap &configuration = QVariantMap() ) override;
    QVariantMap processAlgorithm( const QVariantMap &parameters, QgsProcessingContext &context,
                                  QgsProcessingFeedback *feedback ) override;
};
