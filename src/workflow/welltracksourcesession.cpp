// 层：功能
#include "welltracksourcesession.h"

#include "documentsession.h"
#include "io/areamodel.h"
#include "services/areawellchart.h"
#include "services/paleotaskservice.h"
#include <algorithm>
#include <cmath>
#include <memory>

namespace paleo::welltrack {
namespace {
constexpr int MaxTracks = 8;
constexpr int WindowDebounceMs = 120;
constexpr qlonglong MaxWindowSamples = 32000;
constexpr qlonglong MaxWindowBytes = 4LL * 1024 * 1024;
}

WellTrackSourceSession::WellTrackSourceSession(document::DocumentSession *document, PaleoTaskService *tasks,
                                               const QString &projectDir, const QString &wellEntityId,
                                               qlonglong modelWellId, QObject *parent)
    : QObject(parent), m_document(document), m_tasks(tasks), m_modelDirectory(paleo::area_model::directory(projectDir)),
      m_wellEntityId(wellEntityId), m_modelWellId(modelWellId), m_epoch(document ? document->epoch() : 0),
      m_taskSession(tasks ? tasks->session() : 0) {
  m_debounce.setSingleShot(true);
  m_debounce.setInterval(WindowDebounceMs);
  connect(&m_debounce, &QTimer::timeout, this, &WellTrackSourceSession::readWindow);
  if (!document)
    return;
  connect(document, &document::DocumentSession::sourcesInvalidated, this, [this] {
    if (m_initialized) {
      refreshTracks();
      invalidateReads();
    }
  });
  connect(document, &document::DocumentSession::sessionChanged, this, [this] {
    if (m_choiceTask)
      m_choiceTask->requestCancel();
    m_choices.clear();
    refreshTracks();
    invalidateReads();
  });
}

WellTrackSourceSession::~WellTrackSourceSession() {
  if (m_choiceTask)
    m_choiceTask->requestCancel();
  if (m_readTask)
    m_readTask->requestCancel();
}

bool WellTrackSourceSession::DepthWindow::valid() const {
  return std::isfinite(top) && std::isfinite(bottom) && top < bottom && std::isfinite(bottom - top);
}

bool WellTrackSourceSession::DepthWindow::covers(const DepthWindow &other) const {
  return valid() && other.valid() && top <= other.top && bottom >= other.bottom;
}

bool WellTrackSourceSession::isCurrent() const {
  return m_document && m_tasks && m_document->isOpen() && m_document->epoch() == m_epoch &&
         m_tasks->session() == m_taskSession && !m_wellEntityId.isEmpty() && m_modelWellId > 0;
}

bool WellTrackSourceSession::canEdit() const {
  return isCurrent() && !m_document->readOnly() && !m_document->recoveryRequired();
}

void WellTrackSourceSession::requestChoices() {
  if (!canEdit()) {
    emit choicesReady({}, tr("当前井的来源曲线道不可编辑"));
    return;
  }
  if (m_choiceTask)
    return;
  const auto revision = m_document->revision();
  const auto choices = std::make_shared<QVector<CurveChoice>>();
  auto *task = m_tasks->start(tr("查询测井文件与曲线"),
      [choices, directory = m_modelDirectory, documentId = m_document->documentId(), well = m_modelWellId](PaleoTask *running) {
        QString error;
        listCurveChoices(directory, documentId, well, *choices, &error, [running] { return running->cancelRequested(); });
        return error;
      }, {}, true);
  m_choiceTask = task;
  connect(task, &PaleoTask::finished, this, [this, task, choices, revision] {
    m_choiceTask.clear();
    m_choices.clear();
    if (!canEdit() || m_document->revision() != revision) {
      emit choicesReady({}, tr("工程资料已变化，请重新选择曲线"));
    } else if (task->state() != PaleoTask::State::Succeeded) {
      emit choicesReady({}, task->state() == PaleoTask::State::Cancelled ? tr("读取已取消") : task->errorText());
    } else {
      m_choicesRevision = revision;
      m_choices = std::move(*choices);
      emit choicesReady(m_choices, {});
    }
  });
}

void WellTrackSourceSession::addCurve(const CurveChoice &choice) {
  if (!canEdit() || m_document->revision() != m_choicesRevision) {
    emit failed(tr("工程资料已变化，请重新选择曲线"));
    return;
  }
  const auto found = std::find_if(m_choices.cbegin(), m_choices.cend(), [&](const CurveChoice &entry) {
    return entry.source == choice.source && entry.curveId == choice.curveId && entry.logSetId == choice.logSetId;
  });
  if (found == m_choices.cend()) {
    emit failed(tr("所选曲线不属于当前井的来源列表"));
    return;
  }
  refreshTracks();
  if (m_tracks.size() >= MaxTracks) {
    emit failed(tr("当前井最多可添加 %1 条来源曲线道").arg(MaxTracks));
    return;
  }
  document::WellCurveTrackBinding binding;
  binding.wellEntityId = m_wellEntityId;
  binding.modelWellId = m_modelWellId;
  binding.source = found->source;
  binding.curveId = found->curveId;
  binding.logSetId = found->logSetId;
  binding.title = (found->curveName + QStringLiteral(" · ") + found->fileName).left(160);
  const auto result = m_document->createWellCurveTrack(binding, m_epoch, m_choicesRevision);
  if (!result.ok)
    emit failed(result.error);
}

void WellTrackSourceSession::removeTrack(const QString &id) {
  if (!canEdit()) {
    emit failed(tr("当前井的来源曲线道不可编辑"));
    return;
  }
  const auto result = m_document->removeWellCurveTrack(m_wellEntityId, id, m_epoch, m_document->revision());
  if (!result.ok)
    emit failed(result.error);
}

void WellTrackSourceSession::requestWindow(double topMd, double bottomMd) {
  m_requested = {topMd, bottomMd};
  if (!m_initialized)
    refreshTracks();
  scheduleRead();
}

void WellTrackSourceSession::refreshTracks() {
  const auto current = isCurrent() ? m_document->wellCurveTracks(m_wellEntityId) : QVector<document::WellCurveTrackBinding>{};
  if (m_initialized && current == m_tracks)
    return;
  m_initialized = true;
  m_tracks = current;
  QVector<Track> tracks;
  for (const auto &binding : m_tracks)
    tracks.append({binding.id, binding.title});
  emit tracksChanged(tracks);
}

void WellTrackSourceSession::invalidateReads() {
  ++m_readRevision;
  m_loaded.clear();
  m_failedWindows.clear();
  if (m_readTask)
    m_readTask->requestCancel();
  scheduleRead();
}

bool WellTrackSourceSession::needsRead(const QString &trackId) const {
  return !m_loaded.value(trackId).covers(m_requested) && m_failedWindows.value(trackId) != m_requested;
}

void WellTrackSourceSession::scheduleRead() {
  if (!isCurrent() || m_tracks.isEmpty() || !m_requested.valid()) {
    m_debounce.stop();
    if (m_readTask && !m_readTask->cancelRequested()) {
      ++m_readRevision;
      m_readTask->requestCancel();
    }
    return;
  }
  if (m_readTask) {
    if (!m_inFlight.covers(m_requested) && !m_readTask->cancelRequested()) {
      ++m_readRevision;
      m_readTask->requestCancel();
    }
    return;
  }
  const bool needsRead = std::any_of(m_tracks.cbegin(), m_tracks.cend(), [&](const auto &track) {
    return this->needsRead(track.id);
  });
  if (needsRead)
    m_debounce.start();
}

void WellTrackSourceSession::readWindow() {
  if (!isCurrent() || m_readTask || !m_requested.valid())
    return;
  struct Read {
    document::WellCurveTrackBinding track;
    document::SourceRecord source;
    DepthWindow window;
    CurveReadResult result;
  };
  auto reads = std::make_shared<QVector<Read>>();
  for (const auto &binding : m_tracks) {
    if (needsRead(binding.id))
      reads->append({binding, m_document->source(binding.source), {}, {}});
    if (reads->size() == MaxTracks)
      break;
  }
  if (reads->isEmpty())
    return;
  const double margin = (m_requested.bottom - m_requested.top) * 0.25;
  const DepthWindow padded{m_requested.top - margin, m_requested.bottom + margin};
  m_inFlight = padded.valid() ? padded : m_requested;
  const AreaWellCurveWindow window{m_inFlight.top, m_inFlight.bottom, MaxWindowSamples, MaxWindowBytes};
  const auto requested = m_inFlight;
  const auto viewport = m_requested;
  const auto revision = m_readRevision;
  auto *task = m_tasks->start(tr("读取测井曲线窗"), [reads, window, requested, viewport](PaleoTask *running) {
    const auto cancelled = [running] { return running->cancelRequested(); };
    for (auto &read : *reads) {
      if (cancelled())
        break;
      read.window = requested;
      read.result = readCurve(read.source, read.track.modelWellId, read.track.logSetId,
                              read.track.curveId, window, cancelled);
      const auto remaining = window.maxReadBytes - read.result.stats.payloadBytesRead;
      if (read.result.budgetExceeded && requested != viewport && remaining > 0 && !cancelled()) {
        // Prefetch is optional, so use the remaining budget for the visible interval.
        const AreaWellCurveWindow visible{viewport.top, viewport.bottom, window.maxSamples, remaining};
        read.window = viewport;
        read.result = readCurve(read.source, read.track.modelWellId, read.track.logSetId,
                                read.track.curveId, visible, cancelled);
      }
    }
    return QString();
  }, {}, true);
  m_readTask = task;
  connect(task, &PaleoTask::finished, this, [this, task, reads, requested, viewport, revision] {
    m_readTask.clear();
    if (!isCurrent())
      return;
    if (revision != m_readRevision || !requested.covers(m_requested)) {
      scheduleRead();
      return;
    }
    if (task->state() == PaleoTask::State::Cancelled)
      return;
    if (task->state() == PaleoTask::State::Failed) {
      emit failed(task->errorText());
      return;
    }
    for (const auto &read : *reads) {
      if (!read.window.covers(m_requested))
        continue;
      if (read.result.ok()) {
        m_failedWindows.remove(read.track.id);
        m_loaded.insert(read.track.id, read.window);
      } else {
        m_loaded.remove(read.track.id);
        // A rejected broad window must still allow a later, narrower request.
        m_failedWindows.insert(read.track.id, viewport);
      }
      emit trackReady(read.track.id, read.result.curve, read.result.error);
    }
    scheduleRead();
  });
}

} // namespace paleo::welltrack
