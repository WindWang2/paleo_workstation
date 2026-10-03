// 层：数据
#pragma once
#include <qgsprocessingalgorithm.h>

// 本地方向 IDW 与制图工作场的 Processing 入口。
// 数值核在 singlefactor/，这里只读图层并写栅格。
// 层：数据

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
