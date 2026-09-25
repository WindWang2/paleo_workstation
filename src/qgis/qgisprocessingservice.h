#pragma once
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QWidget>
#include <functional>

class QgsProcessingContext;
class QgsProcessingFeedback;
class PaleoProjectStore;

// qgis/ — QgisProcessingService runs QgsProcessingAlgorithms.
// §41.2 contract: algorithm outputs go to a TEMP destination; on success the
// result is merged/committed through PaleoProjectStore (temp-then-merge,
// never direct-to-gpkg inside task code).
class QgisProcessingService : public QObject
{
  Q_OBJECT
  public:
    explicit QgisProcessingService(PaleoProjectStore *store, QObject *parent = nullptr);

    // Synchronous run (tests + small tasks). Returns algorithm outputs map.
    QVariantMap run(const QString &algorithmId, const QVariantMap &parameters, QString *error = nullptr);

    // List available paleo:* algorithm ids.
    QStringList paleoAlgorithmIds() const;

    // ---- Native Processing algorithm dialog (QGIS 4.2 widget-based API) ----
    //
    // QGIS 4.2 replaced the old QgsProcessingAlgorithmDialogBase with
    // QgsProcessingAlgorithmWidgetBase — a widget hosted in a top-level dialog
    // (or dock) by QgsDockableWidgetHelper. We create it with
    // WidgetFlag::NoDocking so it always opens as a standalone dialog, which is
    // also what makes it usable in offscreen tests.
    //
    // Creates the native algorithm-configuration widget for a registered
    // algorithm id (e.g. "paleo:paleo_constraint_idw"), populating the
    // parameter panel built from QgsGui::processingGuiRegistry() wrappers and
    // applying presetParams. \a parent is used to locate a host QMainWindow
    // (may be nullptr — the dialog is then a free top-level window).
    // Returns nullptr and fills \a error when the algorithm id is unknown.
    // The caller owns the returned widget (it is also reachable via
    // lastAlgorithmDialog() until destroyed).
    QWidget *createAlgorithmDialog(const QString &algId, const QVariantMap &presetParams,
                                   QWidget *parent, QString *error = nullptr);

    // Creates the algorithm widget and shows it as a non-blocking top-level
    // dialog (QgsProcessingAlgorithmWidgetBase::showWidget(); no exec() loop,
    // so this is safe to call under QT_QPA_PLATFORM=offscreen).
    bool showAlgorithmDialog(const QString &algId, const QVariantMap &presetParams,
                             QWidget *parent, QString *error = nullptr);

    // Last widget created by createAlgorithmDialog()/showAlgorithmDialog()
    // (test hook; auto-nulls when the widget is destroyed).
    QWidget *lastAlgorithmDialog() const { return m_lastDialog; }

  private:
    PaleoProjectStore *m_store;
    QPointer<QWidget> m_lastDialog;
};
