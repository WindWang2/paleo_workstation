// 层：视图
#pragma once
#include <QWidget>
class StratigraphyWorkflow;
class QListWidget;
class QTableWidget;
class QLabel;
class WellCorrelationPanel;
class StratigraphyPage : public QWidget {
 Q_OBJECT
public:
 explicit StratigraphyPage(StratigraphyWorkflow *workflow,QWidget *parent=nullptr);
protected:
 void showEvent(QShowEvent *event) override;
private:
 void refresh();
 void showConventions();
 void chooseResource();
 StratigraphyWorkflow *m_workflow;
 QListWidget *m_wells;
 QTableWidget *m_results;
 QLabel *m_status;
 WellCorrelationPanel *m_section;
 bool m_refreshing=false;
 bool m_markerMove=false;
};
