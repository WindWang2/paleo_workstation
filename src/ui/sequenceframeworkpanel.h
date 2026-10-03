// 层：视图
#pragma once
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

#include "../domain/frameworkdiagnostics.h"
#include "../domain/sequenceframework.h"
#include "../services/frameworkservice.h"

class QPushButton;
class QSplitter;
class QTabWidget;
class QTableWidget;
class QTreeWidget;
class QTreeWidgetItem;

class SequenceFrameworkColumnView;

// ui/ — 层序地层格架工作台面板（方向 28 目标 1/2/4/5/6 的操作面）。
// 左：格架树（层序→体系域两级）+ CRUD 与排序按钮；右：格架柱状视图 +
// 标志层 / 诊断 / 建议三个页签。
//
// 分层纪律：本面板只做渲染与交互，模型、诊断、建议、落库分别在 domain /
// domain / algorithms / catalog，编排在 services。所有写库动作都经
// FrameworkService —— 建议页签里的候选默认不勾（未确认），只有勾选过的才
// 会经 commitAccepted() 落盘。
//
// 无头可测：CRUD/排序/保存都有不弹对话框的程序化入口（addSequence /
// addSystemsTract / renameUnit / removeUnit / moveUnit / saveFramework），
// 由 tst_sequenceframework_ui 直接驱动。
class SequenceFrameworkPanel : public QWidget
{
  Q_OBJECT
  public:
    explicit SequenceFrameworkPanel( QWidget *parent = nullptr );

    // 接线：宿主在工程打开时把服务接进来（可重接；空 = 解绑并清空）。
    void setService( SequenceFramework::FrameworkService *service );

    // ---- 程序化入口（按钮与测试共用）----
    bool addSequence( const QString &name, const QString &topBoundary,
                      const QString &baseBoundary, double thickness = 0.0 );
    bool addSystemsTract( const QString &parentId, const QString &name,
                          const QString &topBoundary, const QString &baseBoundary,
                          double thickness = 0.0 );
    bool renameUnit( const QString &unitId, const QString &name );
    bool removeUnit( const QString &unitId );
    bool moveUnit( const QString &unitId, int delta );
    bool addMarker( const QString &name, const QString &unitId, const QStringList &layerNames );
    bool removeMarker( const QString &markerId );
    bool saveFramework();
    bool reloadFramework();

    // 建议：只生成候选（不写库）；applySuggestions 只应用勾选过的。
    QVector<SequenceFramework::SuggestionCandidate> suggestions() const { return m_suggestions; }
    void refreshSuggestions();
    bool applySuggestions();

    // 诊断：当前报告（refreshDiagnostics 后有效）。
    SequenceFramework::DiagnosticReport diagnosticReport() const { return m_report; }
    void refreshDiagnostics();
    bool exportReport( const QString &path, QString *error = nullptr ) const;

    QString currentUnitId() const;
    void setCurrentUnit( const QString &unitId );

    SequenceFrameworkColumnView *columnView() const { return m_column; }

  signals:
    // 与剖面/平面联动：当前格架单元变化（树选择/柱状图点击/外部设置）。
    void currentUnitChanged( const QString &unitId );
    void frameworkDirtyChanged( bool dirty );
    void statusMessage( const QString &text );

  private slots:
    void onTreeSelectionChanged();
    void onColumnUnitChanged( const QString &unitId );
    void onAddSequence();
    void onAddTract();
    void onRename();
    void onRemove();
    void onMoveUp();
    void onMoveDown();
    void onSave();
    void onReload();
    void onRefreshSuggestions();
    void onApplySuggestions();
    void onRefreshDiagnostics();

  private:
    void rebuildTree();
    void rebuildMarkers();
    void rebuildSuggestionTable();
    void rebuildDiagnosticTree();
    void refreshColumn();
    void markDirty( bool dirty );
    QString selectedUnitId() const;
    QTreeWidgetItem *itemForUnit( const QString &unitId ) const;

    SequenceFramework::FrameworkService *m_service = nullptr;
    SequenceFramework::Framework m_draft;
    SequenceFramework::DiagnosticReport m_report;
    QVector<SequenceFramework::SuggestionCandidate> m_suggestions;
    bool m_dirty = false;

    QSplitter *m_splitter = nullptr;
    QTreeWidget *m_tree = nullptr;
    SequenceFrameworkColumnView *m_column = nullptr;
    QTabWidget *m_tabs = nullptr;
    QTableWidget *m_markerTable = nullptr;
    QTableWidget *m_suggestionTable = nullptr;
    QTreeWidget *m_diagTree = nullptr;
    QPushButton *m_saveButton = nullptr;
};
