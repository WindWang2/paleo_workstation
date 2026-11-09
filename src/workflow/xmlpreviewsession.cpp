// 层：功能
#include "xmlpreviewsession.h"
#include "documentsession.h"
#include "../catalog/datacatalog.h"
#include "../services/areawellchart.h"
#include "../services/futuretask.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QTemporaryDir>
#include <QtConcurrent>

namespace {
struct ChartSelection {
  AreaWellChartQuery query;
  QString error,summary;
  const AreaWellChartQuery *request() const {return query.context || query.allowSourceTrackFallback ? &query : nullptr;}
};
ChartSelection chartSelection(const paleo::document::DocumentSession *session,const QString &wellId) {
  using namespace paleo::document;
  ChartSelection result;
  if(!session || wellId.isEmpty())return result;
  const auto selection=session->interpretationSelection();
  result.error=selection.error;
  if(!selection.context)return result;
  auto context=*selection.context;
  context.purpose=RelationPurpose::WellColumn;
  result.query.context=context;
  if(!context.schemeId.isEmpty())result.summary=QObject::tr("分层方案 %1 · 修订 %2")
      .arg(context.schemeId,context.schemeRevision);
  const auto track=session->interpretationRelation(wellId,RelationKind::StratigraphicTrack,RelationPurpose::WellColumn);
  if(track.state==RelationInput::State::Selected)result.query.intervalTrack=track.request;
  else if(track.state==RelationInput::State::Invalid)result.error=track.error;
  const auto td=session->interpretationRelation(wellId,RelationKind::TimeDepth);
  if(td.state!=RelationInput::State::Unselected) {
    const auto note=td.state==RelationInput::State::Selected?
        QObject::tr("时深关系 #%1").arg(td.request.object.id):td.error;
    if(!result.summary.isEmpty())result.summary+=QStringLiteral(" · ");
    result.summary+=note;
  }
  return result;
}
}
void XmlPreviewSession::setDocumentSession(paleo::document::DocumentSession *session,const QString &wellId) {
  m_document=session;m_wellId=wellId;
  if(session)connect(session,&paleo::document::DocumentSession::interpretationChanged,this,[this](const QString &changedWell) {
    if(!changedWell.isEmpty() && changedWell!=m_wellId)return;
    ++m_generation;
    if(m_verificationCancelled)m_verificationCancelled->store(true);
  });
}

void XmlPreviewSession::open(const QString &path, const QString &expectedSha,
                             const QString &modelProject, qlonglong modelSource, qlonglong modelWell)
{
  if (m_verificationCancelled) m_verificationCancelled->store(true);
  m_verificationCancelled = std::make_shared<std::atomic_bool>(false);
  const auto cancelled = m_verificationCancelled;
  const auto generation = ++m_generation;
  const auto interpretation=chartSelection(m_document,m_wellId);
  // 会话独占副本：摘要和解析都只看这份，不回读调用方路径。
  m_directory = std::make_shared<QTemporaryDir>(QDir::tempPath() + QStringLiteral("/paleo-xml-XXXXXX"));
  const auto directory = m_directory;
  auto *watcher = new QFutureWatcher<XmlPreviewData>(this);
  connect(watcher, &QFutureWatcherBase::finished, this,
          [this, watcher, generation] {
            const auto data = watcher->future().isCanceled() ||
                                      !watcher->future().resultCount()
                                  ? XmlPreviewData{}
                                  : watcher->result();
            watcher->deleteLater();
            if (generation == m_generation)
              emit ready(data);
          });
  const auto future = QtConcurrent::run(
      paleo::tasks::pool(m_tasks), [path, expectedSha, cancelled, directory, modelProject, modelSource, modelWell,interpretation] {
        XmlPreviewData data;
        const auto refuse = [&data](const QString &reason) {
          data.chartError = reason;
          data.table.error = reason;
          return data;
        };
        if (modelSource > 0) {
          data.interpretationSummary=interpretation.summary;
          PreviewDocService::areaPreviewSource(modelProject, modelSource, &data.table,
              interpretation.error.isEmpty()?&data.chart:nullptr,&data.chartOk,&data.chartError,modelWell,
              [cancelled]{return cancelled->load();},interpretation.request());
          if(!interpretation.error.isEmpty())data.chartError=interpretation.error;
          return cancelled->load() ? XmlPreviewData{} : data;
        }
        if (PreviewDocService::areaPreviewAt(path, &data.table, &data.chart,
                                             &data.chartOk, &data.chartError))
          return cancelled->load() ? XmlPreviewData{} : data;
        const QFileInfo info(path);
        if (!directory || !directory->isValid())
          return refuse(QStringLiteral("无法创建预览临时目录"));
        if (!info.isFile() || info.isSymLink())
          return refuse(QStringLiteral("不是普通文件：%1").arg(path));
        const QString suffix = info.suffix();
        const QString copyName = suffix.isEmpty()
                                     ? QStringLiteral("source")
                                     : QStringLiteral("source.") + suffix;
        const QString copyPath = directory->filePath(copyName);
        if (!QFile::copy(info.absoluteFilePath(), copyPath))
          return refuse(QStringLiteral("无法读取源文件：%1").arg(path));
        if (!expectedSha.isEmpty()) {
          QString hashError;
          const QString actual = DataCatalog::sha256FileHex(
              copyPath, &hashError, [cancelled] { return cancelled->load(); });
          if (cancelled->load())
            return XmlPreviewData{};
          if (actual.isEmpty())
            return refuse(hashError.isEmpty()
                              ? QStringLiteral("无法读取源文件：%1").arg(path)
                              : hashError);
          if (actual.compare(expectedSha, Qt::CaseInsensitive) != 0)
            return refuse(
                QStringLiteral("源文件与入库时的 SHA-256 不一致，请重新导入"));
        }
        data.chartOk = PreviewDocService::wellCompositeAt(copyPath, &data.chart,
                                                          &data.chartError);
        data.table = PreviewDocService::xmlDataListAt(copyPath);
        return data;
      });
  auto *task = paleo::tasks::track(m_tasks, watcher, future,
                                   tr("解析测井 XML 预览"), true);
  if (task)
    connect(task, &PaleoTask::cancelAsked, watcher,
            [cancelled] { cancelled->store(true); });
  watcher->setFuture(future);
}

void XmlPreviewSession::openModelWell(const QString &modelProject, qlonglong modelWell,
                                      const QString &wellName)
{
  if (m_verificationCancelled) m_verificationCancelled->store(true);
  m_verificationCancelled = std::make_shared<std::atomic_bool>(false);
  const auto cancelled = m_verificationCancelled;
  const auto generation = ++m_generation;
  auto interpretation=chartSelection(m_document,m_wellId);
  if (m_document && !m_wellId.isEmpty()) {
    interpretation.query.allowSourceTrackFallback = true;
    interpretation.query.maxDefaultPlotValues = 1'000'000;
  }
  auto *watcher = new QFutureWatcher<XmlPreviewData>(this);
  connect(watcher, &QFutureWatcherBase::finished, this,
          [this, watcher, generation] {
            const auto data = watcher->future().isCanceled() || !watcher->future().resultCount()
                                  ? XmlPreviewData{}
                                  : watcher->result();
            watcher->deleteLater();
            if (generation == m_generation)
              emit ready(data);
          });
  const auto future = QtConcurrent::run(
      paleo::tasks::pool(m_tasks), [modelProject, modelWell, cancelled,interpretation] {
        XmlPreviewData data;
        data.interpretationSummary=interpretation.summary;
        PreviewDocService::areaPreviewWell(modelProject,modelWell,&data.table,
            interpretation.error.isEmpty()?&data.chart:nullptr,&data.chartOk,&data.chartError,
            [cancelled]{return cancelled->load();},interpretation.request());
        if(!interpretation.error.isEmpty())data.chartError=interpretation.error;
        return cancelled->load() ? XmlPreviewData{} : data;
      });
  auto *task = paleo::tasks::track(m_tasks, watcher, future,
                                   tr("读取井道图 %1").arg(wellName), true);
  if (task)
    connect(task, &PaleoTask::cancelAsked, watcher,
            [cancelled] { cancelled->store(true); });
  watcher->setFuture(future);
}
