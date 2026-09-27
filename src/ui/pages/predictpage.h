#pragma once
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QWidget>

class PredictionWorkflow;
class QgisLayerService;

// ui/pages/predictpage.h — ①智能预测页（自 pagepanels 拆出，公共形状不变：
// objectName/信号签名/ctor 签名全保留）。
// 层：视图
// ①智能预测 — horizon + algorithm selection, run button, status.
class PredictPage : public QWidget
{
  Q_OBJECT
  public:
    PredictPage(PredictionWorkflow *wf, QgisLayerService *layers, QWidget *parent = nullptr);
    void setHorizons(const QStringList &horizons);
    void setAlgorithms(const QStringList &algIds);
  signals:
    void runRequested(const QString &horizon, const QString &algorithmId, const QVariantMap &params);

  private:
    QVariantMap parseInputParams();
};
