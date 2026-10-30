// 层：视图
#pragma once
#include <QStringList>
#include <QVector>
#include <QWidget>
#include <vector>

#include "domain/stratigraphicunit.h"
#include "paleo_data/stratigraphic_framework.h"

class SelectionContext;
class QComboBox;
class QHBoxLayout;
class QLabel;
class QToolButton;

struct SubIntervalLabeledChoice
{
  QString label;
  QString id;
};

// ui/subintervalselectorbar — 层段分析模式的二级选择器（父单元 → S1–S4）。
//
// 与 HorizonChipBar（界面编辑模式）并列：本控件只写 SelectionContext::setActiveUnit，
// 不调用 QgisLayerService::setActiveHorizon，因此不重置地图视角、编辑会话或面板布局。
// 方案 / 修订下拉把 schemeId+revision 写入 UnitSelection；超长列表可输入过滤并分页。
// 展开区按 DS occurrence 与成果绑定显示红绿灯。缺方案时回退工程草稿。
class SubIntervalSelectorBar : public QWidget
{
  Q_OBJECT
  public:
    explicit SubIntervalSelectorBar( SelectionContext *selection, QWidget *parent = nullptr );

    // 用工程界面名单重建父单元选项（无 DS 方案时走 draftScheme）。
    void reloadFromBoundaries( const QStringList &orderedBoundaries,
                               const QString &schemeId = QStringLiteral( "project-default" ),
                               const QString &schemeName = QStringLiteral( "工程默认" ) );

    // 优先从工程 area-model 加载方案/修订；失败则回退 draftScheme(fallbackBoundaries)。
    void reloadFromProject( const QString &projectDir, const QStringList &fallbackBoundaries );

    // 测试：直接注入方案快照与可选 bindings。
    void setSchemeSnapshot( const paleo::data::StratigraphicScheme &scheme,
                            const std::vector<paleo::data::SubIntervalResultBinding> &bindings = {} );

    // 测试/状态栏：当前选中的显示名；无选择时为空。
    QString currentDisplayName() const;
    paleo::strat::UnitSelection currentSelection() const { return m_current; }

    // 父单元下四个子层段的可读状态文案（展开区，含红绿灯）。
    QStringList subIntervalStatusLines() const;

  signals:
    // 用户切换分析层段后发出（与 activeUnitChanged 同步，便于状态栏一句提示）。
    void unitSelectionCommitted( const paleo::strat::UnitSelection &unit );
    // 缺界面 / 方案不完整等可读原因。
    void readinessMessage( const QString &message );

  private:
    void rebuildSchemeCombo();
    void rebuildRevisionCombo();
    void refillSchemeComboPage();
    void refillRevisionComboPage();
    void rebuildParentCombo();
    void rebuildSubButtons();
    void rebuildStatusPanel();
    void updateSchemePageChrome();
    void updateRevisionPageChrome();
    void applySelection( const paleo::strat::UnitSelection &sel, bool broadcast );
    void onSchemeChanged( int index );
    void onRevisionChanged( int index );
    void onParentChanged( int index );
    void onSubClicked( int subIndex ); // 1..4；0 = 综合
    void onSchemeFilterEdited( const QString &text );
    void onRevisionFilterEdited( const QString &text );
    void syncFromContext( const paleo::strat::UnitSelection &unit );
    void applyLoadedScheme( const paleo::data::StratigraphicScheme &scheme, bool broadcast );
    bool loadSelectedScheme( bool broadcast );
    QString readinessText() const;
    QString statusPanelHtml() const;
    void buildSubButtonGroup( QWidget *row, QHBoxLayout *lay );
    void setStatusText( const QString &text );

    SelectionContext *m_selection = nullptr;
    QComboBox *m_schemeCombo = nullptr;
    QComboBox *m_revisionCombo = nullptr;
    QComboBox *m_parent = nullptr;
    QLabel *m_modeLabel = nullptr;
    QLabel *m_schemeIcon = nullptr;
    QLabel *m_revisionIcon = nullptr;
    QLabel *m_schemePageLabel = nullptr;
    QLabel *m_revisionPageLabel = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_expandStatus = nullptr;
    QToolButton *m_expand = nullptr;
    QToolButton *m_schemePrev = nullptr;
    QToolButton *m_schemeNext = nullptr;
    QToolButton *m_revisionPrev = nullptr;
    QToolButton *m_revisionNext = nullptr;
    QVector<QToolButton *> m_subs;
    QToolButton *m_agg = nullptr;
    paleo::data::StratigraphicScheme m_scheme;
    std::vector<paleo::data::SubIntervalResultBinding> m_bindings;
    QVector<SubIntervalLabeledChoice> m_schemeChoices;
    QVector<SubIntervalLabeledChoice> m_revisionChoices;
    QString m_schemeFilter;
    QString m_revisionFilter;
    int m_schemePage = 0;
    int m_revisionPage = 0;
    QString m_projectDir;
    QStringList m_fallbackBoundaries;
    paleo::strat::UnitSelection m_current;
    bool m_block = false;
    bool m_fromDataService = false;
};
