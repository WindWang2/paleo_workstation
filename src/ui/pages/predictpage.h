// 层：视图
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
// ①智能预测 — predict type + horizon + algorithm selection, schema-driven
// parameter form, task-shaped run (busy/cancel/progress) and a per-horizon
// history list of declared prediction results.
class PredictPage : public QWidget
{
  Q_OBJECT
  public:
    PredictPage(PredictionWorkflow *wf, QgisLayerService *layers, QWidget *parent = nullptr);
    void setHorizons(const QStringList &horizons);
    void setAlgorithms(const QStringList &algIds);

  public slots:
    // m2(A) 任务化运行：busy=true → 运行禁用（带 reason tooltip）+ 取消按钮
    // + 进度条；updateProgress 收任务池回传的百分比（无进度回调的算法停在 0，
    // 不伪造进度）。
    void setRunBusy(bool busy);
    void updateProgress(int percent);
    // MAMCL 外部工具：busy=环境准备中（按钮置灰 + reason tooltip），启动结果
    // 文案由壳写 statusLabel。
    void setMamclBusy(bool busy);

  signals:
    void runRequested(const QString &horizon, const QString &algorithmId, const QVariantMap &params);
    // 取消按钮（协作式：worker 无法中断的部分由壳如实呈现）。
    void runCancelRequested();
    // 历史结果行「显示」→ 壳 instantiate + 图层树勾选 + zoomToLayer。
    void showResultRequested(const QString &layerId);
    // 外部工具「地震多属性智能分析 (MAMCL)」→ 壳走 MamclTool 编排启动。
    void mamclLaunchRequested();

  private:
    QVariantMap parseInputParams();                    // onnx:* 三控件（行为不变）
    void rebuildSchemaForm();                          // paramsFormArea 按算法 schema 重建
    bool collectSchemaParams(QVariantMap *out);        // schema 控件 → QVariantMap
    void refreshHistory();                             // historyList ← manifest 当前层位声明
};
