// 层：功能
#pragma once
#include "domain/welllogfacies.h"
#include <QElapsedTimer>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QTimer>
#include <QUrl>
class QNetworkReply;

struct WellFaciesConfig {
  QUrl baseUrl = QUrl(QStringLiteral("http://118.178.238.153:3100/api/v1"));
  QByteArray apiKey;
  static QString path();
  static WellFaciesConfig load();
  bool save(QString *error) const;
};
class WellFaciesService : public QObject {
  Q_OBJECT
public:
  explicit WellFaciesService(QObject *parent = nullptr);
  ~WellFaciesService() override { cancel(); }
  void configure(const WellFaciesConfig &config);
  void fetchModels();
  void predict(const WellFaciesModel &model, const QString &wellName,
               const WellFaciesInput &input);
  void cancel();
signals:
  void modelsReady(const QVector<WellFaciesModel> &models);
  void completed(const WellFaciesResult &result);
  void failed(const QString &reason);
  void progress(int percent, const QString &message);

private:
  void request(const QString &path, const QJsonObject &body, bool post,
               bool models);
  void handlePrediction(const QJsonObject &json);
  void fail(const QString &reason);
  QNetworkAccessManager m_network;
  QPointer<QNetworkReply> m_reply;
  QTimer m_poll;
  QElapsedTimer m_elapsed;
  WellFaciesConfig m_config;
  WellFaciesInput m_input;
  QString m_wellName, m_jobId, m_modelId;
  int m_generation = 0;
};
