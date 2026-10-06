// 层：功能
#pragma once
#include <QObject>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVector>

// Transport-neutral values: a real remote adapter can replace the substitute
// used by tests without touching QGIS, catalog registration or the page. No
// live layers cross this boundary.
//
// 方向51（goal/ai-assist-20261006）：本文件原先还定义着一个测试替身类
// （第 44 行起），已整段迁到 tests/mockremotepredictionservice.h——只被测试
// 夹具使用，生产面不再出现替身。生产装配（src/app/aiwiring.cpp）装的是
// RemotePredictionRouter：远端端点未配置时如实报「未配置」，不再悄悄跑
// 替身数据冒充远端推理。
struct RemotePredictionRequest {
  QString id, horizon, kind;
  QStringList sourceVersionIds;
  QVariantList wells, facies;
  QRectF extent;
  int columns = 64, rows = 64;
  // 可选网格输入（行主序 columns×rows；NaN=缺）。远端服务端可离线重取，
  // 本地 ORT 降级必须有它才不造假——空 = 本地降级不可用（如实失败）。
  QVector<float> samples;
};
struct RemotePredictionResult {
  RemotePredictionRequest request;
  QVector<int> cells;
  QVariantList points;
  QString method;
  // true = 结果由替身（test double）产生，不是真实推理——上层不得把它当
  // 产品结论展示。走 router/真实服务端时置 false。
  bool mock = true;
};
Q_DECLARE_METATYPE(RemotePredictionRequest)
Q_DECLARE_METATYPE(RemotePredictionResult)

class RemotePredictionService : public QObject {
  Q_OBJECT
public:
  using QObject::QObject;
  ~RemotePredictionService() override;
  virtual void start(const RemotePredictionRequest &request) = 0;
  virtual void cancel() = 0;
signals:
  void progress(const QString &requestId, int percent);
  void completed(const RemotePredictionResult &result);
  void failed(const QString &requestId, const QString &reason);
};
