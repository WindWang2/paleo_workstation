// 层：视图
#pragma once
#include <QStringList>
#include <QVector>
#include <QWidget>

#include "domain/stratigraphicunit.h"
#include "paleo_data/stratigraphic_framework.h"

class SelectionContext;
class QComboBox;
class QLabel;
class QToolButton;

// ui/subintervalselectorbar — 层段分析模式的二级选择器（父单元 → S1–S4）。
//
// 与 HorizonChipBar（界面编辑模式）并列：本控件只写 SelectionContext::setActiveUnit，
// 不调用 QgisLayerService::setActiveHorizon，因此不重置地图视角、编辑会话或面板布局。
// 缺方案 / 未确认解释 / 时深不明等状态用可读中文提示，不弹冗长向导。
class SubIntervalSelectorBar : public QWidget
{
  Q_OBJECT
  public:
    explicit SubIntervalSelectorBar( SelectionContext *selection, QWidget *parent = nullptr );

    // 用工程界面名单重建父单元选项（无 DS 方案时走 draftScheme）。
    void reloadFromBoundaries( const QStringList &orderedBoundaries,
                               const QString &schemeId = QStringLiteral( "project-default" ),
                               const QString &schemeName = QStringLiteral( "工程默认" ) );

    // 测试/状态栏：当前选中的显示名；无选择时为空。
    QString currentDisplayName() const;
    paleo::strat::UnitSelection currentSelection() const { return m_current; }

    // 父单元下四个子层段的可读状态文案（展开区）。
    QStringList subIntervalStatusLines() const;

  signals:
    // 用户切换分析层段后发出（与 activeUnitChanged 同步，便于状态栏一句提示）。
    void unitSelectionCommitted( const paleo::strat::UnitSelection &unit );
    // 缺界面 / 方案不完整等可读原因。
    void readinessMessage( const QString &message );

  private:
    void rebuildParentCombo();
    void rebuildSubButtons();
    void rebuildStatusPanel();
    void applySelection( const paleo::strat::UnitSelection &sel, bool broadcast );
    void onParentChanged( int index );
    void onSubClicked( int subIndex ); // 1..4；0 = 综合
    void syncFromContext( const paleo::strat::UnitSelection &unit );
    QString readinessText() const;

    SelectionContext *m_selection = nullptr;
    QComboBox *m_parent = nullptr;
    QLabel *m_modeLabel = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_expandStatus = nullptr;
    QToolButton *m_expand = nullptr;
    QVector<QToolButton *> m_subs; // S1..S4 + 可选综合
    QToolButton *m_agg = nullptr;
    paleo::data::StratigraphicScheme m_scheme;
    paleo::strat::UnitSelection m_current;
    bool m_block = false;
};
