// 层：功能
#pragma once
#include "tileoperation.h"
#include "services/hostcallcontext.h"
#include <QObject>
#include <QPointer>
class PaleoTask;
class PaleoTaskService;
namespace paleo::capability {class CapabilityHost;struct ExecuteResult;struct FrozenOperation;}
namespace paleo {
class TileOperationService:public QObject {
  Q_OBJECT
public:
  using SourceProvider=std::function<bool(const QString &,FrozenRasterInput &,QString *)>;
  using PresentationProvider=std::function<QJsonObject(const QString &,const QString &,QString *)>;
  explicit TileOperationService(QObject *parent=nullptr);
  void configure(capability::CapabilityHost *,PaleoTaskService *,PaleoOnnxService *,
                 SourceProvider,PresentationProvider);
  QString readiness(const QJsonObject &,bool forListing) const;
  PaleoTask *start(const QJsonObject &intent,const QString &trustedScope,QObject *subscriber,
      std::function<void(capability::ExecuteResult)> finished,QString *error);
private:
  PaleoTask *prepare(const QJsonObject &,const QString &,QObject *,
      std::function<void(capability::ExecuteResult)>,QString *);
  capability::FrozenOperation operation(const QJsonObject &,const QString &) const;
  QPointer<capability::CapabilityHost> m_host;
  QPointer<PaleoTaskService> m_tasks;
  QPointer<PaleoOnnxService> m_onnx;
  SourceProvider m_source;
  PresentationProvider m_presentation;
};
}
