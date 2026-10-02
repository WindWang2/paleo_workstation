// 层：功能
#pragma once
#include <QObject>
#include <QString>
#include <QStringList>

#include "../algorithms/velocitymodel.h"
#include "derivedassets.h"

class DataCatalog;
class QgisLayerService;

// workflow/ — 时深转换编排（goal/time-depth-velocity）。视图只发意图信号，
// 本层把井控制数据（分层/校验炮）拟合成速度模型并落 catalog；把时间域层位
// 栅格逐像元换算成深度域栅格（DERIVED 登记 + 层声明）。不持部件、不画像素。
//
// 深度基准选型（ledger 轮1）：输出 = TVD（井口基准面以下视铅直深度，米）——
// TD 表 TVD 列优先、MD 兜底（同 TimeDepthTool 口径）；TVDSS 换算需逐井 KB，
// 本轮不做臆造换算，递延项记录在 docs/progress/time-depth.md。
struct VelocityModelBuildRequest
{
  paleo::velmodel::ModelType type = paleo::velmodel::ModelType::IntervalAverage;
  QStringList topsFilePaths;     // 井分层 DC.dat 形态（多井；Time/TVD 列有效行入模）
  QStringList tdFilePaths;       // 时深 TD/*.dat（单井校验炮；井名取 # Well 行）
  QStringList wellHeadFilePaths; // 井位坐标兜底（tops 行/ TD 表不带坐标时）
};

class DepthConversionWorkflow : public QObject
{
  Q_OBJECT
public:
  explicit DepthConversionWorkflow(DataCatalog *catalog, const QString &projectDir,
                                   QObject *parent = nullptr);

  void setLayerService(QgisLayerService *layers) { m_layers = layers; }
  // 工程打开时重绑 catalog/工程目录（QObject 不可赋值；registrar 原地重建）。
  void rebind(DataCatalog *catalog, const QString &projectDir);

  // 纯建模（无 catalog 依赖——性能测试/预览可直接用）。失败返回无效模型。
  static paleo::velmodel::VelocityModel buildModel(const VelocityModelBuildRequest &req,
                                                   QString *error = nullptr,
                                                   QStringList *notesOut = nullptr);
  // 建模 + 存档（catalog DERIVED velocity_model 资产，父版本=源数据文件版本）。
  // 成功返回模型 JSON 绝对路径；失败空串 + error。
  QString buildAndStoreModel(const VelocityModelBuildRequest &req, QString *error = nullptr);

  // 时间域层位 GeoTIFF → 深度域 GeoTIFF：DERIVED depth_raster 登记 +（有
  // layer service 时）声明 "depth.<horizon>" 图层。重复转换产生同一资产递增
  // 版本，像元值位级幂等。
  bool convertRasterToDepth(const QString &horizon, const QString &timeRasterPath,
                            const QString &modelJsonPath,
                            QString *layerId = nullptr, QString *error = nullptr);

  static paleo::velmodel::VelocityModel loadModel(const QString &modelJsonPath,
                                                  QString *error = nullptr);
  // catalog 里最新 velocity_model 版本的文件路径（无 → 空串）。
  static QString latestModelPath(DataCatalog *catalog, const QString &projectDir);
  // 从 catalog 已解析关联收集建模输入（tops/time_depth/well_head 各取最新版本）。
  static VelocityModelBuildRequest requestFromCatalog(DataCatalog *catalog,
                                                      const QString &projectDir);

  // 绑定实例的便捷面（壳层不重复持有 catalog/projectDir）。
  QString latestModelPath() const;
  VelocityModelBuildRequest requestFromCatalog() const;

signals:
  void modelStored(const QString &modelPath);
  void conversionDone(const QString &horizon, const QString &layerId);
  void conversionFailed(const QString &horizon, const QString &reason);

private:
  DataCatalog *m_catalog = nullptr;
  QString m_projectDir;
  QgisLayerService *m_layers = nullptr;
  DerivedAssetRegistrar m_registrar;
};
