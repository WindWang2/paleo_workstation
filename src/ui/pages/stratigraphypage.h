// 层：视图
#pragma once
#include <QWidget>
#include <QHash>
class StratigraphyWorkflow;
class QListWidget;
class QTableWidget;
class QLabel;
namespace WellComposite { class MultiWellView; class WellCompositePanel; }
class QBoxLayout;class QComboBox;class QLineEdit;class QDoubleSpinBox;class QCheckBox;
class StratigraphyPage : public QWidget {
 Q_OBJECT
public:
 explicit StratigraphyPage(StratigraphyWorkflow *workflow,QWidget *parent=nullptr);
protected:
 void showEvent(QShowEvent *event) override;
private:
 void refresh();
 void refreshPreview();
 void connectPreview(WellComposite::WellCompositePanel *panel, qlonglong well);
 void showConventions();
 void chooseResource();
 void buildActions(QBoxLayout* layout);
 void buildEditor(QBoxLayout* layout);
 void buildAdvanced(QBoxLayout* layout);
 void refreshVersions();
 void refreshRevisions();
 void clearSelection();
 void selectObject(qlonglong well,const QString& id,bool cycle=false);
 void showHierarchy();
 void showAlgorithms();
 void showDiagnostics();
 void showWellOrder();
 void showResults();
 void showAssignments();
 void buildRangeControls(QBoxLayout* layout);
 void fitFormation();
 void highlightSelection();
 void showGeometry();
 void showClustering();
 void startHorizons();
 void updateAvailability();
 bool confirmReplace();
 QList<qlonglong> selectedWells() const;
 StratigraphyWorkflow *m_workflow;
 QListWidget *m_wells;
 QTableWidget *m_results;
 QLabel *m_status;
 WellComposite::MultiWellView *m_section=nullptr;
 bool m_reloadPreview=true;
 bool m_refreshing=false;
 qlonglong m_selectedWell=0;
 QString m_selectedObject;bool m_selectedCycle=false;
 QLineEdit* m_name=nullptr;
 QDoubleSpinBox* m_depth=nullptr;
 QDoubleSpinBox *m_rangeTop=nullptr,*m_rangeBottom=nullptr;
 QCheckBox* m_lock=nullptr;
 QComboBox *m_level=nullptr,*m_direction=nullptr,*m_flatten=nullptr,*m_scope=nullptr,*m_template=nullptr;
 QComboBox *m_saved=nullptr,*m_revision=nullptr;
 QLabel* m_source=nullptr;
 QHash<QString,QString> m_markerIds;

};
