// 层：视图
#pragma once
#include <QSet>

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
class PaleoCoordinateEditor;

// wellsitingpanel — 井网辅助面板（方向 34，验证页「布井辅助」页签）。
// 坐标输入/地图拾取经 QGIS 封装显式转换，写库仍为工程计算坐标。
class WellSitingPanel : public QWidget
{
  Q_OBJECT
  public:
    explicit WellSitingPanel( WellSitingWorkflow *wf, QWidget *parent = nullptr );

    // 刷新面（工程打开/切换后由壳调）。
    void reloadFromWorkflow();

    void setCoordinateCanvas( QWidget *canvas );
    // 旧接口接收工程局部网格；地图入口接收画布 CRS 并显式反算。
    void placePlannedAt( double x, double y );
    void placeMapPoint( double x, double y );

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
    // #820：诊断/候选异步终态。
    void onDiagnosisAsyncDone( bool ok, const QString &error );
    void onCandidatesAsyncDone( bool ok, const QString &error );
    void deleteSelectedScenario();
    void exportScenarioCsv();
    void exportComparisonChart();

  private:
    void refreshPlannedTable();
    void refreshScenarioTable();
    void refreshCandidateTable();
    void refreshDiagnosisLabels();
    void buildCoordinateSection( QVBoxLayout *lay );
    // D.3：方案评估段自建（ctor 拆出，file-budget 函数体规模闸）。
    void buildEvaluationSection( QVBoxLayout *lay );
    // D.3：忙面（不确定进度条 + 取消钮 + 禁重动词）与指标文本上屏。
    void setEvalBusy( bool busy );
    void applyEvaluation( const QVariantMap &evaluation );
    void setComputeBusy( const QString &buttonName, bool busy );
    void connectAsyncResults();
    WellSitingWorkflow *m_wf = nullptr;
    PaleoCoordinateEditor *m_pointEditor = nullptr;
    bool m_evalBusy = false;    // 在飞评估（单飞——忙期新意图只登记待办）
    bool m_evalPending = false; // 忙期到达的新意图：落地后补跑一次
    bool m_savePending = false; // #820：保存方案等待异步评估落地
    QSet<QString> m_computeBusy; // #820：在飞诊断/候选（按按钮 objectName）
};
