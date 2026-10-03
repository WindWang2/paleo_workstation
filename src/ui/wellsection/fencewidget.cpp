// 层：视图
#include "fencewidget.h"

#include "catalog/datacatalog.h"
#include "linkage/selectioncontext.h"
#include "metadata/wellsectionstore.h"
#include "services/paleotaskservice.h"
#include "ui/paleoicons.h"
#include "ui/paleotheme.h"
#include "wellsectiondialogs.h"
#include "workflow/wellsectionworkflow.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPen>
#include <QSet>
#include <QSpinBox>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {
// 预览剖面线色：数据符号色板（不进 UI token；纸面符号语义同 zoneColor）。
QVector<QColor> previewPalette()
{
    return {QColor(QStringLiteral("#1F77B4")), QColor(QStringLiteral("#D62728")),
            QColor(QStringLiteral("#2CA02C")), QColor(QStringLiteral("#9467BD")),
            QColor(QStringLiteral("#FF7F0E")), QColor(QStringLiteral("#8C564B"))};
}

// 井网预览：井点 + 各剖面折线（按剖面取色），交点井（多条剖面共用）画
// 主色描边放大点。坐标系 = choices 井位归一到部件矩形（等比）。
class FencePreview : public QWidget
{
  public:
    explicit FencePreview(QWidget *parent = nullptr) : QWidget(parent)
    {
        setMinimumHeight(160);
    }
    void setModel(QVector<WellSectionPanel::WellChoice> choices,
                  QVector<QStringList> sections)
    {
        m_choices = std::move(choices);
        m_sections = std::move(sections);
        update();
    }

  protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        const auto &t = PaleoTheme::tokens();
        double minX = qQNaN(), minY = qQNaN(), maxX = qQNaN(), maxY = qQNaN();
        for (const auto &c : m_choices) {
            if (!c.hasCoordinates)
                continue;
            minX = std::isfinite(minX) ? qMin(minX, c.x) : c.x;
            minY = std::isfinite(minY) ? qMin(minY, c.y) : c.y;
            maxX = std::isfinite(maxX) ? qMax(maxX, c.x) : c.x;
            maxY = std::isfinite(maxY) ? qMax(maxY, c.y) : c.y;
        }
        p.setPen(QPen(t.textDisabled, 1));
        if (!std::isfinite(minX) || !std::isfinite(maxX) || maxX <= minX ||
            maxY <= minY) {
            p.drawText(rect(), Qt::AlignCenter, QObject::tr("井位坐标缺失"));
            return;
        }
        const double pad = 18.0;
        const double sx = (width() - 2 * pad) / (maxX - minX);
        const double sy = (height() - 2 * pad) / (maxY - minY);
        const double s = qMin(sx, sy);
        const double ox = (width() - (maxX - minX) * s) * 0.5;
        const double oy = (height() - (maxY - minY) * s) * 0.5;
        const auto mapX = [&](double x) { return ox + (x - minX) * s; };
        const auto mapY = [&](double y) { return height() - oy - (y - minY) * s; };

        // 交点井集合。
        QSet<QString> dups;
        {
            QSet<QString> seen;
            for (const QStringList &ids : m_sections)
                for (const QString &id : ids)
                    (seen.contains(id) ? dups : seen).insert(id);
        }
        const auto palette = previewPalette();
        // 剖面折线：井序连线（同色成网，条带间不交叉由布点保证）。
        for (int i = 0; i < m_sections.size(); ++i) {
            QPen pen(palette.at(uint(i) % palette.size()).darker(120), 2.0);
            p.setPen(pen);
            QPointF prev;
            bool has = false;
            for (const QString &id : m_sections.at(i)) {
                const auto *c = choiceById(id);
                if (!c)
                    continue;
                const QPointF pt(mapX(c->x), mapY(c->y));
                if (has)
                    p.drawLine(prev, pt);
                prev = pt;
                has = true;
            }
        }
        // 井点与名字。
        QFont f = p.font();
        f.setPointSize(PaleoTheme::kLabelPt);
        p.setFont(f);
        for (const auto &c : m_choices) {
            if (!c.hasCoordinates)
                continue;
            const QPointF pt(mapX(c.x), mapY(c.y));
            const bool shared = dups.contains(c.id);
            p.setPen(QPen(shared ? PaleoTheme::tokens().primary
                                 : PaleoTheme::tokens().text,
                          shared ? 2.0 : 1.0));
            p.setBrush(QBrush(shared ? PaleoTheme::tokens().primary
                                     : PaleoTheme::tokens().text));
            p.drawEllipse(pt, shared ? 4.5 : 3.0, shared ? 4.5 : 3.0);
            p.setPen(PaleoTheme::tokens().textMuted);
            p.drawText(QRectF(pt.x() + 6, pt.y() - 8, 60, 14),
                       Qt::AlignLeft | Qt::AlignVCenter, c.name);
        }
    }

  private:
    const WellSectionPanel::WellChoice *choiceById(const QString &id) const
    {
        for (const auto &c : m_choices)
            if (c.id == id)
                return &c;
        return nullptr;
    }
    QVector<WellSectionPanel::WellChoice> m_choices;
    QVector<QStringList> m_sections;
};
} // namespace

WellSectionFenceWidget::WellSectionFenceWidget(const Params &params,
                                               QWidget *parent)
    : QWidget(parent), m_params(params)
{
    setWindowTitle(tr("栅状图（连井剖面网）"));
    setObjectName(QStringLiteral("wellSectionFenceWidget"));

    // ---- 工具行 ----
    auto *bar = new QWidget(this);
    bar->setObjectName(QStringLiteral("wellSectionFenceToolBar"));
    PaleoTheme::applyThemedStyleSheet(bar, [] {
        const auto &t = PaleoTheme::tokens();
        return QStringLiteral(
                   "#wellSectionFenceToolBar { background: %1; border-bottom: "
                   "1px solid %2; }")
                   .arg(t.surfaceAlt.name(), t.border.name()) +
               PaleoTheme::toolButtonStyleSheet();
    });
    auto *barLay = new QHBoxLayout(bar);
    barLay->setContentsMargins(4, 2, 4, 2);
    barLay->setSpacing(4);

    auto *autoLabel = new QLabel(tr("条带数"), bar);
    auto *autoSpin = new QSpinBox(bar);
    autoSpin->setObjectName(QStringLiteral("wellSectionFenceBands"));
    autoSpin->setRange(1, 8);
    autoSpin->setValue(2);
    auto *autoBtn = new QToolButton(bar);
    autoBtn->setObjectName(QStringLiteral("wellSectionFenceAutoButton"));
    autoBtn->setIcon(PaleoIcons::qgisTheme(
        QLatin1String("mActionElevationProfile.svg")));
    autoBtn->setIconSize(QSize(18, 18));
    autoBtn->setAutoRaise(true);
    autoBtn->setToolTip(tr("按井位自动布点（主轴条带 + 最小交叉走线）"));
    barLay->addWidget(autoLabel);
    barLay->addWidget(autoSpin);
    barLay->addWidget(autoBtn);
    barLay->addSpacing(8);
    m_addBtn = new QToolButton(bar);
    m_addBtn->setObjectName(QStringLiteral("wellSectionFenceAddButton"));
    m_addBtn->setIcon(PaleoIcons::qgisTheme(QLatin1String("mActionAdd.svg")));
    m_addBtn->setIconSize(QSize(18, 18));
    m_addBtn->setAutoRaise(true);
    m_addBtn->setToolTip(tr("手工添加一条剖面"));
    m_editBtn = new QToolButton(bar);
    m_editBtn->setObjectName(QStringLiteral("wellSectionFenceEditButton"));
    m_editBtn->setIcon(PaleoIcons::qgisTheme(QLatin1String("mActionOpenTable.svg")));
    m_editBtn->setIconSize(QSize(18, 18));
    m_editBtn->setAutoRaise(true);
    m_editBtn->setToolTip(tr("编辑当前剖面的井与顺序"));
    m_removeBtn = new QToolButton(bar);
    m_removeBtn->setObjectName(QStringLiteral("wellSectionFenceRemoveButton"));
    m_removeBtn->setIcon(
        PaleoIcons::qgisTheme(QLatin1String("mActionRemoveLayer.svg")));
    m_removeBtn->setIconSize(QSize(18, 18));
    m_removeBtn->setAutoRaise(true);
    m_removeBtn->setToolTip(tr("移除当前剖面"));
    barLay->addWidget(m_addBtn);
    barLay->addWidget(m_editBtn);
    barLay->addWidget(m_removeBtn);
    barLay->addStretch(1);

    // ---- 主体：左列表 + 中 tabs + 右预览 ----
    m_list = new QListWidget(this);
    m_list->setObjectName(QStringLiteral("wellSectionFenceList"));
    m_list->setMinimumWidth(180);
    m_tabs = new QTabWidget(this);
    m_tabs->setObjectName(QStringLiteral("wellSectionFenceTabs"));
    m_tabs->setTabsClosable(false);
    m_preview = new FencePreview(this);
    m_preview->setObjectName(QStringLiteral("wellSectionFencePreview"));
    m_preview->setMinimumWidth(220);
    PaleoTheme::applyThemedStyleSheet(m_preview, [] {
        return QStringLiteral("#wellSectionFencePreview { background: %1; }")
            .arg(PaleoTheme::tokens().surface.name());
    });
    m_hint = new QLabel(this);
    m_hint->setObjectName(QStringLiteral("wellSectionFenceHint"));
    PaleoTheme::applyThemedStyleSheet(
        m_hint, [] { return PaleoTheme::mutedCaptionStyleSheet(); });

    auto *body = new QHBoxLayout;
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(0);
    body->addWidget(m_list, 0);
    body->addWidget(m_tabs, 1);
    body->addWidget(m_preview, 0);

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addWidget(bar);
    lay->addLayout(body, 1);
    lay->addWidget(m_hint);

    connect(autoBtn, &QToolButton::clicked, this,
            [this, autoSpin] { autoPlan(autoSpin->value()); });
    connect(m_addBtn, &QToolButton::clicked, this,
            [this] { addManualSection(); });
    connect(m_editBtn, &QToolButton::clicked, this, [this] {
        editSectionWells(m_list->currentRow());
    });
    connect(m_removeBtn, &QToolButton::clicked, this, [this] {
        removeSection(m_list->currentRow());
    });
    connect(m_list, &QListWidget::currentRowChanged, m_tabs,
            &QTabWidget::setCurrentIndex);
    connect(m_tabs, &QTabWidget::currentChanged, m_list,
            qOverload<int>(&QListWidget::setCurrentRow));

    loadFromStore();
    rebuild();
}

QStringList WellSectionFenceWidget::sectionWellIds(int index) const
{
    return (index >= 0 && index < m_wellIds.size()) ? m_wellIds.at(index)
                                                    : QStringList();
}

WellSectionPanel *WellSectionFenceWidget::sectionPanel(int index) const
{
    return (index >= 0 && index < m_sections.size())
               ? m_sections.at(index).panel
               : nullptr;
}

void WellSectionFenceWidget::autoPlan(int targetSections)
{
    QVector<wellsection::Well> wells;
    for (const auto &c : m_params.choices)
        if (c.hasCoordinates) {
            wellsection::Well w;
            w.id = c.id;
            w.x = c.x;
            w.y = c.y;
            wells << w;
        }
    const wellsection::FencePlan plan =
        wellsection::planFence(wells, targetSections);
    if (!plan.ok()) {
        m_hint->setText(
            plan.status == wellsection::FencePlan::Status::MissingCoords
                ? tr("井位坐标不全，无法自动布点")
                : tr("两口以上的井才能组成栅状图"));
        return;
    }
    QVector<QStringList> sections;
    for (const wellsection::FenceSection &sec : plan.sections)
        sections << sec.wellIds;
    setSections(sections);
}

void WellSectionFenceWidget::setSections(const QVector<QStringList> &sections)
{
    m_wellIds.clear();
    for (const QStringList &ids : sections)
        if (!ids.isEmpty())
            m_wellIds << ids;
    rebuild();
}

void WellSectionFenceWidget::rebuild()
{
    // 断开旧面板（QObject 父子链统一回收）。
    for (auto &sec : m_sections) {
        delete sec.panel;
        delete sec.wf;
    }
    m_sections.clear();
    m_tabs->clear();
    m_list->clear();

    for (int i = 0; i < m_wellIds.size(); ++i) {
        SectionCtl ctl;
        ctl.wf = new WellSectionWorkflow(m_params.catalog, this);
        ctl.wf->setTaskService(m_params.tasks);
        ctl.panel = new WellSectionPanel(m_params.selection, this);
        ctl.panel->setWellChoices(m_params.choices);

        const int index = i;
        connect(ctl.panel, &WellSectionPanel::dataRequested, this,
                [this, index](const QStringList &ids,
                              const QStringList &mnemonics) {
                    auto &sec = m_sections[index];
                    sec.panel->setBusy(true);
                    sec.lastGen = sec.wf->request(ids, mnemonics);
                });
        connect(ctl.wf, &WellSectionWorkflow::sectionReady, this,
                [this, index](int gen, const QVector<wellsection::Well> &wells,
                               const QStringList &warnings) {
                    auto &sec = m_sections[index];
                    if (gen != sec.lastGen)
                        return; // 陈旧世代丢弃
                    sec.panel->setBusy(false);
                    sec.panel->setSection(wells);
                    sec.panel->setWarnings(warnings);
                });
        // 用户在剖面内改井序/移除 → 权威态同步 + 落库 + 列表刷新。
        connect(ctl.panel, &WellSectionPanel::wellIdsChanged, this,
                [this, index](const QStringList &ids) {
                    if (index < m_wellIds.size() && m_wellIds[index] != ids) {
                        m_wellIds[index] = ids;
                        m_list->item(index)->setText(
                            tr("剖面 %1（%2 口井）").arg(index + 1).arg(ids.size()));
                        m_tabs->setTabText(
                            index, tr("剖面 %1（%2 口井）")
                                       .arg(index + 1)
                                       .arg(ids.size()));
                        saveToStore();
                        emit sectionsChanged();
                    }
                });

        // 先入表再喂井集：setWellIds 同步发 dataRequested，回调按 index
        // 取 m_sections——push 前访问会越界。
        m_sections.push_back(ctl);
        m_tabs->addTab(ctl.panel, tr("剖面 %1").arg(i + 1));
        ctl.panel->setWellIds(m_wellIds.at(i));
        m_list->addItem(tr("剖面 %1（%2 口井）")
                            .arg(i + 1)
                            .arg(m_wellIds.at(i).size()));
    }
    updateSync();
    saveToStore();

    m_tabs->setVisible(!m_wellIds.isEmpty());
    if (m_wellIds.isEmpty())
        m_hint->setText(tr("用「自动布点」或「+」手工添加剖面"));
    else {
        int shared = 0;
        QSet<QString> seen;
        QSet<QString> dups;
        for (const QStringList &ids : m_wellIds)
            for (const QString &id : ids)
                (seen.contains(id) ? dups : seen).insert(id);
        shared = dups.size();
        m_hint->setText(shared > 0
                            ? tr("%1 条剖面 · 交点井 %2 口（改动同步）")
                                  .arg(m_wellIds.size())
                                  .arg(shared)
                            : tr("%1 条剖面").arg(m_wellIds.size()));
    }
    static_cast<FencePreview *>(m_preview)->setModel(m_params.choices,
                                                     m_wellIds);
}

// 交点井同帧联动：任一剖面点名 → 其余剖面立即选中同一井（Oracle #3）。
void WellSectionFenceWidget::updateSync()
{
    // 直连：sender panel 的 wellClicked → 其余 panel selectWell。
    for (int i = 0; i < m_sections.size(); ++i) {
        auto *src = m_sections[i].panel;
        for (int j = 0; j < m_sections.size(); ++j) {
            if (i == j)
                continue;
            auto *dst = m_sections[j].panel;
            connect(src, &WellSectionPanel::wellClicked, dst,
                    [dst](const QString &id) { dst->selectWell(id); });
        }
    }
}

void WellSectionFenceWidget::saveToStore()
{
    if (!m_params.store)
        return;
    if (m_persistedIds == m_wellIds)
        return; // 无变化不落盘（版本只随真实编辑推进）
    // fence-<n> 节逐条写（版本随每次落盘推进）；条数收缩 → 尾节删除。
    QString err;
    for (int i = 0; i < m_wellIds.size(); ++i)
        m_params.store->save(QStringLiteral("fence-%1").arg(i + 1),
                             m_wellIds.at(i), {}, &err);
    const QStringList ids = m_params.store->sectionIds(&err);
    for (const QString &id : ids)
        if (id.startsWith(QLatin1String("fence-"))) {
            const int n = id.mid(QLatin1String("fence-").size()).toInt();
            if (n > m_wellIds.size())
                m_params.store->remove(id, &err);
        }
    m_persistedIds = m_wellIds;
}

void WellSectionFenceWidget::setStore(metadata::WellSectionStore *store)
{
    if (m_params.store == store)
        return;
    m_params.store = store;
    m_persistedIds.clear(); // 换库后首存不等值短路失效，强制重写
    saveToStore();
}

void WellSectionFenceWidget::loadFromStore()
{
    if (!m_params.store)
        return;
    QString err;
    // 按 n 升序收全部 fence-n 节（节号有洞也继续——save 侧失败可能留洞）。
    QVector<QPair<int, QStringList>> rows;
    for (const QString &id : m_params.store->sectionIds(&err))
        if (id.startsWith(QLatin1String("fence-"))) {
            const int n = id.mid(QLatin1String("fence-").size()).toInt();
            if (n < 1)
                continue;
            const auto rec = m_params.store->load(id, &err);
            if (!rec.wellIds.isEmpty())
                rows << qMakePair(n, rec.wellIds);
        }
    std::sort(rows.begin(), rows.end(),
              [](const QPair<int, QStringList> &a,
                 const QPair<int, QStringList> &b) { return a.first < b.first; });
    QVector<QStringList> sections;
    for (const auto &row : rows)
        sections << row.second;
    m_wellIds = sections;
    m_persistedIds = sections;
}

void WellSectionFenceWidget::editSectionWells(int index)
{
    if (index < 0 || index >= m_wellIds.size())
        return;
    WellSectionWellsDialog dlg(m_params.choices, m_wellIds.at(index),
                               m_params.selection
                                   ? m_params.selection->selectedIds()
                                   : QStringList(),
                               this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    m_wellIds[index] = dlg.selectedIds();
    rebuild();
    emit sectionsChanged();
}

void WellSectionFenceWidget::addManualSection()
{
    WellSectionWellsDialog dlg(m_params.choices, QStringList(),
                               m_params.selection
                                   ? m_params.selection->selectedIds()
                                   : QStringList(),
                               this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    if (dlg.selectedIds().isEmpty())
        return;
    m_wellIds << dlg.selectedIds();
    rebuild();
    emit sectionsChanged();
}

void WellSectionFenceWidget::removeSection(int index)
{
    if (index < 0 || index >= m_wellIds.size())
        return;
    m_wellIds.removeAt(index);
    rebuild();
    emit sectionsChanged();
}
