#pragma once
#include <QWidget>
#include <QString>
#include <QVariantMap>

// ui/pages/ — per-page right-dock panels (§42.2 inventory).
// Each panel is a plain QWidget bound to services/workflows via ctor injection.
// UI never touches Qgs* classes directly (§25) — panels emit intents; the
// shell/workflows do the GIS work. objectName set on every interactive widget
// for tests.

class PredictionWorkflow;
class ConstraintWorkflow;
class CompositionWorkflow;
class ValidationWorkflow;
class QgisLayerService;
class PaleoOnnxService;

// 数据管理 — asset import intents + asset table placeholder.
class DataPage : public QWidget
{
  Q_OBJECT
  public:
    explicit DataPage(QWidget *parent = nullptr);
  signals:
    void importRequested(const QString &kind);  // "wells" | "seismic" | "boundary"
};

// ①智能预测 — horizon + algorithm selection, run button, status.
class PredictPage : public QWidget
{
  Q_OBJECT
  public:
    PredictPage(PredictionWorkflow *wf, QgisLayerService *layers, QWidget *parent = nullptr);
    void setHorizons(const QStringList &horizons);
    void setAlgorithms(const QStringList &algIds);
  signals:
    void runRequested(const QString &horizon, const QString &algorithmId);
};

// ②约束与单因素 — constraint list + run-IDW row.
class ConstraintPage : public QWidget
{
  Q_OBJECT
  public:
    ConstraintPage(ConstraintWorkflow *wf, QWidget *parent = nullptr);
  signals:
    void drawConstraintRequested(const QString &horizon, int faciesCode);
    void runIdwRequested(const QString &horizon);
};

// ③综合编图 — factor list (checkbox rows) + fuse button.
class ComposePage : public QWidget
{
  Q_OBJECT
  public:
    ComposePage(CompositionWorkflow *wf, QgisLayerService *layers, QWidget *parent = nullptr);
    void refreshFactors();                       // re-list declared factor.* layers
  signals:
    void fuseRequested(const QStringList &factorLayerIds);
};

// ④验证 — run button + issues table + locate intent.
class ValidatePage : public QWidget
{
  Q_OBJECT
  public:
    ValidatePage(ValidationWorkflow *wf, QWidget *parent = nullptr);
    void populate();                              // run validate(), fill table
  signals:
    void locateRequested(const QString &layerId, const QString &wktLocation);
};
