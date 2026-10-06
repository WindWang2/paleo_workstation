// 层：QGIS 封装
#pragma once
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <functional>

class QgsProject;

class QgsProcessingContext;
class QgsProcessingFeedback;
class PaleoProjectStore;
// 方向 59：QWidget 只以指针/返回值出现——前向声明即可，include <QWidget>
// 会把 QtWidgets 传递灌进 11 个直接消费本头的 workflow TU（--transitive
// 黄牌实测）。成员 QPointer<QWidget> 与出参定义都在 .cpp 侧补全。
class QWidget;

// qgis/ — QgisProcessingService runs QgsProcessingAlgorithms.
// §41.2 contract: algorithm outputs go to a TEMP destination; on success the
// result is merged/committed through PaleoProjectStore (temp-then-merge,
// never direct-to-gpkg inside task code).
class QgisProcessingService : public QObject
{
  Q_OBJECT
  public:
    explicit QgisProcessingService(PaleoProjectStore *store, QObject *parent = nullptr);

    // Algorithm dialogs and output layers enumerate this project. Null falls
    // back to QgsProject::instance() for tests that use the singleton.
    void setProject(QgsProject *project);

    // 任务线程可在计算过程中请求取消，并把 0–100 的进度送回界面。
    struct ProcessingHooks
    {
        std::function<bool()> cancelled;
        std::function<void(double)> progress;
    };

    // Synchronous run (tests + small tasks). Returns algorithm outputs map.
    QVariantMap run(const QString &algorithmId, const QVariantMap &parameters, QString *error = nullptr,
                    const ProcessingHooks &hooks = {});

    // List available paleo:* algorithm ids.
    QStringList paleoAlgorithmIds() const;

    // All registered algorithm ids ("provider:alg"), sorted — for algorithm
    // pickers that need the full Processing registry, not just paleo:*.
    QStringList algorithmIds() const;

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
    // 定义在 .cpp（返回值经 QPointer 转换需 QWidget 完整类型——头文件只留
    // 前向声明，消费 TU 不再被迫吃 QtWidgets）。
    QWidget *lastAlgorithmDialog() const;

  private:
    PaleoProjectStore *m_store;
    QPointer<QgsProject> m_project;
    QPointer<QWidget> m_lastDialog;
};
