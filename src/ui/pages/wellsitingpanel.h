// 层：视图
#pragma once

#include <QVariantMap>
#include <QWidget>

class QTableWidget;
class WellSitingWorkflow;
class QDoubleSpinBox;
class QSpinBox;
class QLineEdit;
class QLabel;
class QPushButton;
class QVBoxLayout;

// wellsitingpanel — 井网辅助面板（方向 34，验证页「布井辅助」页签）。
// 视图只发意图/只读 workflow 结果：诊断/候选/评估/方案/导出按钮直调
// WellSitingWorkflow 动词并刷表；地图布点经 mapPlacementRequested 意图
// 信号交壳层装 PaleoSitingPickTool（工具拾取的点位由壳回调
// placePlannedAt 注入）。指标口径注记（note）如实上屏——不粉饰。
class WellSitingPanel : public QWidget
{
  Q_OBJECT
  public:
    explicit WellSitingPanel( WellSitingWorkflow *wf, QWidget *parent = nullptr );

    // 刷新面（工程打开/切换后由壳调）。
    void reloadFromWorkflow();

    // 壳回调：地图工具拾取到点位（画布系米制网格）——入 catalog 计划井。
    void placePlannedAt( double x, double y );

  signals:
    // 意图：装地图布点工具（壳负责 canvas/map tool 生命周期）。
    void mapPlacementRequested();
    // 意图：导出文件对话框由壳统一管（面板只报场景：csv/chart）。
    void exportRequested( const QString &kind, const QString &scenarioId );

  private slots:
    void runDiagnosis();
    void generateCandidates();
    void addPlannedFromInputs();
    void removeSelectedPlanned();
    void renameSelectedPlanned();
    void refreshEvaluation();
    // D.3：异步评估交付（GUI 线程）——成功上指标，失败/取消如实说明。
    void onEvaluationAsyncDone( const QVariantMap &evaluation, const QString &error );
    void saveScenarioFromInput();
    void deleteSelectedScenario();
    void exportScenarioCsv();
    void exportComparisonChart();

  private:
    void refreshPlannedTable();
    void refreshScenarioTable();
    void refreshCandidateTable();
    void refreshDiagnosisLabels();
    // D.3：方案评估段自建（ctor 拆出，file-budget 函数体规模闸）。
    void buildEvaluationSection( QVBoxLayout *lay );
    // D.3：忙面（不确定进度条 + 取消钮 + 禁重动词）与指标文本上屏。
    void setEvalBusy( bool busy );
    void applyEvaluation( const QVariantMap &evaluation );
    WellSitingWorkflow *m_wf = nullptr;
    bool m_evalBusy = false;    // 在飞评估（单飞——忙期新意图只登记待办）
    bool m_evalPending = false; // 忙期到达的新意图：落地后补跑一次
};
