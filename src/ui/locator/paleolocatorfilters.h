#pragma once
#include <qgslocatorfilter.h>

class QgsFeedback;

class QgisLayerService;
class QgsMapCanvas;

// ui/locator/ — domain locator filters for Ctrl+K (D4: layers + wells +
// horizons + issues). Each is a thin QgsLocatorFilter; fetchResults does the
// lookup, triggerResult performs the action (zoom/select via callbacks).

// Wells: search well ids/names in a declared wells point layer; trigger selects+zooms.
class WellLocatorFilter : public QgsLocatorFilter
{
  Q_OBJECT
  public:
    // resolveWellLayer: returns the wells QgsVectorLayer + id field name on demand.
    using WellLayerProvider = std::function<QPair<class QgsVectorLayer *, QString>()>;
    WellLocatorFilter(WellLayerProvider provider, QgsMapCanvas *canvas, QObject *parent = nullptr);
    QString name() const override { return QStringLiteral("paleo_wells"); }
    QString displayName() const override { return tr("井位"); }
    Priority priority() const override { return Highest; }
    QString prefix() const override { return QStringLiteral("w"); }
    QgsLocatorFilter::Flags flags() const override { return FlagFast; }
    WellLocatorFilter *clone() const override { return new WellLocatorFilter(m_provider, m_canvas); }
    void fetchResults(const QString &string, const QgsLocatorContext &context,
                      QgsFeedback *feedback) override;
    void triggerResult(const QgsLocatorResult &result) override;
  private:
    WellLayerProvider m_provider;
    QgsMapCanvas *m_canvas;
};

// Horizons: match horizon ids/names from the manifest; trigger switches active horizon.
class HorizonLocatorFilter : public QgsLocatorFilter
{
  Q_OBJECT
  public:
    using HorizonListProvider = std::function<QStringList()>;
    using ActivateFn = std::function<void(const QString &)>;
    HorizonLocatorFilter(HorizonListProvider provider, ActivateFn activate, QObject *parent = nullptr);
    QString name() const override { return QStringLiteral("paleo_horizons"); }
    QString displayName() const override { return tr("层位"); }
    QString prefix() const override { return QStringLiteral("h"); }
    QgsLocatorFilter::Flags flags() const override { return FlagFast; }
    HorizonLocatorFilter *clone() const override { return new HorizonLocatorFilter(m_provider, m_activate); }
    void fetchResults(const QString &string, const QgsLocatorContext &context,
                      QgsFeedback *feedback) override;
    void triggerResult(const QgsLocatorResult &result) override;
  private:
    HorizonListProvider m_provider;
    ActivateFn m_activate;
};

// Issues: match validation issue codes/messages; trigger emits locate intent.
class IssueLocatorFilter : public QgsLocatorFilter
{
  Q_OBJECT
  public:
    struct IssueRef { QString code, message, layerId, wkt; };
    using IssueProvider = std::function<QList<IssueRef>()>;
    using LocateFn = std::function<void(const IssueRef &)>;
    IssueLocatorFilter(IssueProvider provider, LocateFn locate, QObject *parent = nullptr);
    QString name() const override { return QStringLiteral("paleo_issues"); }
    QString displayName() const override { return tr("验证问题"); }
    QString prefix() const override { return QStringLiteral("i"); }
    QgsLocatorFilter::Flags flags() const override { return FlagFast; }
    IssueLocatorFilter *clone() const override { return new IssueLocatorFilter(m_provider, m_locate); }
    void fetchResults(const QString &string, const QgsLocatorContext &context,
                      QgsFeedback *feedback) override;
    void triggerResult(const QgsLocatorResult &result) override;
  private:
    IssueProvider m_provider;
    LocateFn m_locate;
};
