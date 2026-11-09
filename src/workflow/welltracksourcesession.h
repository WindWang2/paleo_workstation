// 层：功能
#pragma once

#include "services/welltracksource.h"
#include <QObject>
#include <QHash>
#include <QPointer>
#include <QTimer>

class PaleoTask;
class PaleoTaskService;
namespace paleo::document { class DocumentSession; }

namespace paleo::welltrack {

struct Track {
  QString id;
  QString label;
};

class WellTrackSourceSession : public QObject {
  Q_OBJECT
public:
  WellTrackSourceSession(document::DocumentSession *document, PaleoTaskService *tasks,
                         const QString &projectDir, const QString &wellEntityId, qlonglong modelWellId,
                         QObject *parent = nullptr);
  ~WellTrackSourceSession() override;
  bool canEdit() const;
  void requestChoices();
  void addCurve(const CurveChoice &choice);
  void removeTrack(const QString &id);
  void requestWindow(double topMd, double bottomMd);

signals:
  void choicesReady(const QVector<CurveChoice> &choices, const QString &error);
  void tracksChanged(const QVector<Track> &tracks);
  void trackReady(const QString &id, const WellComposite::CurveData &curve, const QString &error);
  void failed(const QString &error);

private:
  struct DepthWindow {
    double top = 0;
    double bottom = 0;
    bool valid() const;
    bool covers(const DepthWindow &other) const;
    bool operator==(const DepthWindow &) const = default;
  };

  bool isCurrent() const;
  void refreshTracks();
  void invalidateReads();
  void scheduleRead();
  void readWindow();
  bool needsRead(const QString &trackId) const;

  QPointer<document::DocumentSession> m_document;
  QPointer<PaleoTaskService> m_tasks;
  QString m_modelDirectory;
  QString m_wellEntityId;
  qlonglong m_modelWellId;
  quint64 m_epoch;
  quint64 m_taskSession;
  quint64 m_readRevision = 0;
  quint64 m_choicesRevision = 0;
  QTimer m_debounce;
  DepthWindow m_requested;
  DepthWindow m_inFlight;
  QHash<QString, DepthWindow> m_loaded;
  QHash<QString, DepthWindow> m_failedWindows;
  QVector<document::WellCurveTrackBinding> m_tracks;
  QVector<CurveChoice> m_choices;
  QPointer<PaleoTask> m_choiceTask;
  QPointer<PaleoTask> m_readTask;
  bool m_initialized = false;
};

} // namespace paleo::welltrack
