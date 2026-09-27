// 层：视图
#pragma once

#include <QPointer>
#include <QWidget>
#include <QString>
#include <QVariantMap>

class PredictionWorkflow;
class QgisLayerService;
class PaleoOnnxService;

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

class QShowEvent;

// ②约束与单因素 — constraint list + run-IDW row + D61→D62 厚度样本表。
// 样本行由 MappingWorkflow 镜像到 ConstraintWorkflow 的
// "paleo.thickness.samples"/"paleo.thickness.message" 动态属性（面板只持有
// ConstraintWorkflow*）；showEvent 时重读渲染，不弹对话框。
