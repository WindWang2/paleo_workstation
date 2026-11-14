// 层：功能
#pragma once
#include "aichattoolrunner.h"
#include "capabilityrecovery.h"
#include <memory>
#include <QJsonArray>

// One owner-thread orchestration, with value-only input/result work in PaleoTaskService.
// Every remote segment is a durable external operation. Cancelling cannot imply remote rollback.
class WellFaciesAgentRun:public QObject {
  Q_OBJECT
public:
  WellFaciesAgentRun(AiChatToolContext context,WellFaciesService *service,PaleoTaskService *tasks,
                    WellFaciesConfig config,QString trustedChatSession,std::function<QString()> validate,QObject *parent);
  PaleoTask *start(const QJsonObject &args);
  void cancel();
signals:
  void finished(bool ok,const QJsonObject &result);
private:
  QString current() const;
  void resolveModel();
  void verifyStoredModel();
  void prepare(const WellFaciesModel &model);
  void restore(const QJsonObject &reference);
  void inputReady(std::shared_ptr<paleo::wellfacies::PreparedInput> input,const QJsonObject &reference,bool replay);
  void next();
  void completed(const WellFaciesResult &result);
  void attach(const WellFaciesResult &result,const QByteArray &bytes,const QString &hash);
  void attachBytes(const QByteArray &bytes,int count);
  void restoreCompletedResult();
  void resultUnavailable(const QString &reason);
  void unknown(const QString &reason);
  void finish(bool ok,const QString &code,const QJsonObject &extra={});
  AiChatToolContext m_context;
  WellFaciesConfig m_config;
  QPointer<WellFaciesService> m_service;
  QPointer<PaleoTaskService> m_tasks;
  QPointer<PaleoTask> m_phase,m_worker;
  QPointer<paleo::capability::CapabilityRecovery> m_recovery;
  std::function<QString()> m_validate,m_ownerValidation;
  QJsonObject m_args,m_parentPlan;
  paleo::wellfacies::Request m_request;
  std::shared_ptr<paleo::wellfacies::PreparedInput> m_input;
  paleo::capability::CapabilityRecovery::Accepted m_segment;
  QString m_scope,m_endpointId,m_parentId,m_remoteJobId,m_chatSession,m_projectDir;
  QJsonArray m_results;
  int m_index=0;
  bool m_done=false,m_externalActive=false,m_send=false;
  bool m_resultReceived=false,m_cancelAfterResult=false,m_ignoreService=false;
  bool m_modelVerified=false;
  bool m_remoteFailed=false;
  bool m_resultRecoveryOnly=false;
  bool m_parentCancelled=false;
};
