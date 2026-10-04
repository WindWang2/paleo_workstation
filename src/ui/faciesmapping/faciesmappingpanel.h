// 层：视图
#pragma once

#include <QWidget>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QTableWidget;
class QToolButton;

// 证据合成 + QA 报告面板（goal/facies-automapping 阶段3/4/5 的壳）。
// 只发意图：层位、井相柱来源、权重/阈值参数；链路计算在功能层
//（FaciesMappingWorkflow）。QA 报告表只呈现工作流给的结果行，点击发
// 定位意图——不在视图层做几何判断。
class FaciesMappingPanel : public QWidget
{
  Q_OBJECT
  public:
    explicit FaciesMappingPanel( QWidget *parent = nullptr );

    struct SourceWeights
    {
      double well = 1.0;
      double factor = 1.0;
      double prediction = 1.0;
    };
    SourceWeights weights() const;
    double assignThreshold() const;
    double minIslandArea() const;
    double wellCoverageRadius() const;
    double minRegionArea() const;

    // QA 报告行（工作流侧解析 JSON 后喂表；视图不读文件）。
    struct QaRow
    {
      QString type;      // issue 类型名（unclosed_ring/…）
      QString regionIds; // 分号连接
      QString related;
      double metric = 0;
      double x = 0;
      double y = 0;
    };
    void setQaRows( const QList<QaRow> &rows );

    void setBusy( bool busy );
    bool isBusy() const { return m_busy; }
    void setSummary( const QString &text );

  signals:
    void generateRequested( const QString &horizon, double wellWeight, double factorWeight,
                            double predictionWeight, double assignThreshold,
                            double minRegionArea, double minIslandArea,
                            double wellCoverageRadius );
    void cancelRequested();
    void issueSelected( const QString &regionId, double x, double y );

  public slots:
    void updateProgress( int percent, const QString &stageLabel );
    void showResult( bool ok, const QString &summary );

  private:
    void buildUi();
    void syncEnabledState();
    int addQaRow( const QaRow &row );

    QLineEdit *m_horizon = nullptr;
    QDoubleSpinBox *m_wellWeight = nullptr;
    QDoubleSpinBox *m_factorWeight = nullptr;
    QDoubleSpinBox *m_predictionWeight = nullptr;
    QDoubleSpinBox *m_assignThreshold = nullptr;
    QDoubleSpinBox *m_minRegionArea = nullptr;
    QDoubleSpinBox *m_minIslandArea = nullptr;
    QDoubleSpinBox *m_coverageRadius = nullptr;
    QToolButton *m_generate = nullptr;
    QToolButton *m_cancel = nullptr;
    QProgressBar *m_progress = nullptr;
    QLabel *m_status = nullptr;
    QTableWidget *m_qaTable = nullptr;
    QLabel *m_qaSummary = nullptr;
    bool m_busy = false;
};
