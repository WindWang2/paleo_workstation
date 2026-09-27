// 层：视图
#pragma once

#include <QPointer>
#include <QWidget>
#include <QString>
#include <QVariantMap>

class ConstraintWorkflow;

// ②约束与单因素 — constraint list + run-IDW row + D61→D62 厚度样本表。
// 样本行由 MappingWorkflow 镜像到 ConstraintWorkflow 的
// "paleo.thickness.samples"/"paleo.thickness.message" 动态属性（面板只持有
// ConstraintWorkflow*）；showEvent 时重读渲染，不弹对话框。
class ConstraintPage : public QWidget
{
  Q_OBJECT
  public:
    ConstraintPage(ConstraintWorkflow *wf, QWidget *parent = nullptr);
    // 重读厚度样本属性并刷新逐井表（井名/D61 TVD/D62 TVD/层间速度或原因）。
    void refreshThicknessSamples();
  signals:
    void drawConstraintRequested(const QString &horizon, const QString &shape, int faciesCode);
    void runIdwRequested(const QString &horizon);
  protected:
    void showEvent(QShowEvent *event) override;
};
