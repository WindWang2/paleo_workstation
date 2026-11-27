// 层：视图
#include "workspacecomparison.h"
#include "../paleotheme.h"
#include "../decorations/paleodecorations.h"
#include "../pages/mappingreferencepanel.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QResizeEvent>
#include <QSettings>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTabBar>
#include <QToolButton>
#include <QVBoxLayout>

WorkspaceComparison::WorkspaceComparison(QWidget *mainMap, QWidget *parent)
    : QWidget(parent), m_main(mainMap), m_controls(new QWidget(this)),
      m_split(new QSplitter(Qt::Horizontal, this)), m_tabs(new QTabBar(m_controls))
{
    setObjectName(QStringLiteral("workspaceComparison"));
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto *row = new QHBoxLayout(m_controls);
    row->setContentsMargins(PaleoTheme::tokens().spacingSm, 0, PaleoTheme::tokens().spacingSm, 0);
    row->setSpacing(PaleoTheme::tokens().spacingSm);
    m_caption = new QLabel(tr("主图与参考对照"), m_controls);
    row->addWidget(m_caption);
    m_tabs->setObjectName(QStringLiteral("comparisonMapTabs"));
    m_tabs->setAccessibleName(tr("对照地图"));
    m_tabs->setDocumentMode(true);
    m_tabs->addTab(tr("主图"));
    m_tabs->addTab(tr("参考图"));
    row->addWidget(m_tabs);
    row->addStretch();
    m_settings = new QToolButton(m_controls);
    m_settings->setObjectName(QStringLiteral("comparisonReferenceSettings"));
    m_settings->setText(tr("参考设置"));
    m_settings->setCheckable(true);
    m_settings->hide();
    row->addWidget(m_settings);
    connect(m_settings, &QToolButton::toggled, this, &WorkspaceComparison::arrange);
    m_legends = new QToolButton(m_controls);
    m_legends->setObjectName(QStringLiteral("comparisonLegends"));
    m_legends->setText(tr("图例"));
    m_legends->setAccessibleName(tr("显示主图和参考图的图例"));
    m_legends->setToolTip(m_legends->accessibleName());
    m_legends->setCheckable(true);
    m_legends->setChecked(QSettings("paleo","paleo").value("mappingEvidence/comparisonLegends", false).toBool());
    m_legends->hide();
    row->addWidget(m_legends);
    connect(m_legends, &QToolButton::toggled, this, [this](bool checked) {
        QSettings("paleo","paleo").setValue("mappingEvidence/comparisonLegends", checked);
        refreshLegends();
    });
    auto *dock = new QToolButton(m_controls);
    dock->setObjectName(QStringLiteral("dockComparison"));
    dock->setText(tr("停靠面板"));
    dock->setToolTip(tr("移到可浮动的参考证据面板"));
    row->addWidget(dock);
    auto *close = new QToolButton(m_controls);
    close->setObjectName(QStringLiteral("closeComparison"));
    close->setText(tr("结束对照"));
    row->addWidget(close);
    connect(dock, &QToolButton::clicked, this, &WorkspaceComparison::dockRequested);
    connect(close, &QToolButton::clicked, this, &WorkspaceComparison::closeRequested);
    connect(m_tabs, &QTabBar::currentChanged, this, [this] { arrange(); });
    layout->addWidget(m_controls);
    m_controls->hide();
    m_split->setObjectName(QStringLiteral("comparisonSplitter"));
    m_split->setChildrenCollapsible(false);
    m_split->addWidget(m_main);
    layout->addWidget(m_split, 1);
    connect(m_split, &QSplitter::splitterMoved, this, [this] {
        if (comparing() && m_wide) m_savedSplit = m_split->saveState();
    });
}

WorkspaceComparison::~WorkspaceComparison()
{
    // Tab and splitter children can signal while QWidget tears down its layout.
    disconnect(m_tabs, nullptr, this, nullptr);
    disconnect(m_split, nullptr, this, nullptr);
    disconnect(m_legends, nullptr, this, nullptr);
    disconnect(m_settings, nullptr, this, nullptr);
    if (m_reference) disconnect(m_reference, nullptr, this, nullptr);
    if (m_primaryLegend) disconnect(m_primaryLegend, nullptr, this, nullptr);
    if (m_reference)
        for (auto *legend : m_reference->findChildren<PaleoDecorationManager *>())
            disconnect(legend, nullptr, this, nullptr);
}

void WorkspaceComparison::present(QWidget *reference)
{
    if (!reference) return;
    if (m_reference != reference) {
        m_reference = reference;
        if (auto *panel = qobject_cast<MappingEvidencePanel *>(reference))
            connect(panel, &MappingEvidencePanel::workspaceViewChanged, this, &WorkspaceComparison::arrange,
                    Qt::UniqueConnection);
        m_split->addWidget(reference);
    }
    m_tabs->setCurrentIndex(1);
    m_controls->show();
    refreshLegends();
    arrange();
}

void WorkspaceComparison::release()
{
    const bool restoreFocus = isAncestorOf(QApplication::focusWidget());
    if (auto *panel = qobject_cast<MappingEvidencePanel *>(m_reference.data())) {
        disconnect(panel, &MappingEvidencePanel::workspaceViewChanged, this, &WorkspaceComparison::arrange);
        panel->setMapOnly(false);
    }
    const QSignalBlocker blocker(m_settings);
    m_settings->setChecked(false);
    if (m_primaryLegend) m_primaryLegend->setComparisonLegendVisible(true);
    if (m_reference)
        for (auto *legend : m_reference->findChildren<PaleoDecorationManager *>())
            legend->setComparisonLegendVisible(true);
    m_reference.clear();
    m_wide = false;
    m_controls->hide();
    m_main->show();
    if (restoreFocus) m_main->setFocus(Qt::OtherFocusReason);
}

QByteArray WorkspaceComparison::splitState() const { return m_savedSplit; }
void WorkspaceComparison::restoreSplit(const QByteArray &state) { m_savedSplit = state; }

void WorkspaceComparison::setPrimaryLegend(PaleoDecorationManager *legend)
{
    m_primaryLegend = legend;
    if (legend) connect(legend, &PaleoDecorationManager::legendAvailabilityChanged,
                        this, &WorkspaceComparison::refreshLegends, Qt::UniqueConnection);
    refreshLegends();
}

void WorkspaceComparison::refreshLegends()
{
    if (!m_reference) return;
    auto legends = m_reference->findChildren<PaleoDecorationManager *>();
    if (m_primaryLegend) legends << m_primaryLegend.data();
    bool available = false;
    for (auto *legend : legends) {
        connect(legend, &PaleoDecorationManager::legendAvailabilityChanged,
                this, &WorkspaceComparison::refreshLegends, Qt::UniqueConnection);
        legend->setComparisonLegendVisible(m_legends->isChecked());
        available |= legend->hasFaciesLegend();
    }
    m_legends->setVisible(available);
}

void WorkspaceComparison::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    arrange();
}

void WorkspaceComparison::arrange()
{
    if (!m_reference) return;
    const bool wide = width() >= 780;
    auto *panel = qobject_cast<MappingEvidencePanel *>(m_reference.data());
    const auto views = panel ? panel->views() : QList<MappingReferencePanel *>{};
    const auto *single = !wide && views.size() == 1 && views.first()->hasReference() ? views.first() : nullptr;
    if (panel) panel->setMapOnly(single && !m_settings->isChecked());
    m_settings->setVisible(single && m_tabs->currentIndex() == 1);
    const QString label = single ? single->compactTabTitle() : tr("参考图");
    m_tabs->setTabText(1, m_tabs->fontMetrics().elidedText(label, Qt::ElideMiddle, qMax(84, width() / 4)));
    m_tabs->setTabToolTip(1, single ? single->sourceDescription() : QString());
    m_caption->setVisible(wide);
    m_tabs->setVisible(!wide);
    m_main->setVisible(wide || m_tabs->currentIndex() == 0);
    m_reference->setVisible(wide || m_tabs->currentIndex() == 1);
    if (wide && !m_wide && (m_savedSplit.isEmpty() || !m_split->restoreState(m_savedSplit)))
        m_split->setSizes({width() / 2, width() / 2});
    m_wide = wide;
}
