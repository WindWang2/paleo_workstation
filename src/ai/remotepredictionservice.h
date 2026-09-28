// 层：功能
#pragma once
#include <QObject>
#include <QRectF>
#include <QTimer>
#include <QVariantMap>
#include <QVector>

// Transport-neutral values: a real remote adapter can replace the mock without
// touching QGIS, catalog registration or the page. No live layers cross this
// boundary.
struct RemotePredictionRequest {
  QString id, horizon, kind;
  QStringList sourceVersionIds;
  QVariantList wells, facies;
  QRectF extent;
  int columns = 64, rows = 64;
};
struct RemotePredictionResult {
  RemotePredictionRequest request;
  QVector<int> cells;
  QVariantList points;
  QString method;
  bool mock = true;
};
Q_DECLARE_METATYPE(RemotePredictionRequest)
Q_DECLARE_METATYPE(RemotePredictionResult)

class RemotePredictionService : public QObject {
  Q_OBJECT
public:
  using QObject::QObject;
  virtual void start(const RemotePredictionRequest &request) = 0;
  virtual void cancel() = 0;
signals:
  void progress(const QString &requestId, int percent);
  void completed(const RemotePredictionResult &result);
  void failed(const QString &requestId, const QString &reason);
};

class MockRemotePredictionService : public RemotePredictionService {
  Q_OBJECT
public:
  explicit MockRemotePredictionService(QObject *parent = nullptr);
  void start(const RemotePredictionRequest &request) override;
  void cancel() override;

private:
  QTimer m_timer;
  RemotePredictionResult m_result;
  int m_step = 0;
};
