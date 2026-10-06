// 层：视图
#include "errorhistorypanel.h"

#include "../paleotheme.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLineEdit>
#include <QPushButton>
#include <QSet>
#include <QTableWidget>
#include <QVBoxLayout>

namespace
{
enum Column { ColTime, ColLevel, ColSource, ColCount, ColTitle, ColText, ColN };

QString levelLabel(ErrorHub::Level level)
{
    switch (level)
    {
    case ErrorHub::Level::Error: return ErrorHistoryPanel::tr("错误");
    case ErrorHub::Level::Warning: return ErrorHistoryPanel::tr("警告");
    case ErrorHub::Level::Info: return ErrorHistoryPanel::tr("信息");
    }
    return QString();
}

QString oneLine(QString s)
{
    s.replace(QLatin1Char('\t'), QLatin1Char(' '));
    s.replace(QLatin1Char('\n'), QLatin1Char(' '));
    return s;
}
} // namespace

ErrorHistoryPanel::ErrorHistoryPanel(ErrorHub *hub, QWidget *parent)
    : QWidget(parent), m_hub(hub)
{
    setObjectName(QStringLiteral("errorHistoryPanel"));
    const auto &t = PaleoTheme::tokens();
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(t.spacingSm, t.spacingSm, t.spacingSm, t.spacingSm);
    lay->setSpacing(t.spacingSm);

    auto *bar = new QHBoxLayout;
    m_level = new QComboBox(this);
    m_level->setObjectName(QStringLiteral("errorHistoryLevel"));
    m_level->addItems({tr("全部级别"), tr("错误"), tr("警告"), tr("信息")});
    m_level->setAccessibleName(tr("级别过滤"));
    m_filter = new QLineEdit(this);
    m_filter->setObjectName(QStringLiteral("errorHistoryFilter"));
    m_filter->setPlaceholderText(tr("过滤来源/标题/内容"));
    m_filter->setClearButtonEnabled(true);
    m_copy = new QPushButton(tr("复制"), this);
    m_copy->setObjectName(QStringLiteral("errorHistoryCopy"));
    m_copy->setToolTip(tr("复制选中行（无选中则复制全部可见行）"));
    m_clear = new QPushButton(tr("清空"), this);
    m_clear->setObjectName(QStringLiteral("errorHistoryClear"));
    m_clear->setToolTip(tr("清空错误历史"));
    bar->addWidget(m_level);
    bar->addWidget(m_filter, 1);
    bar->addWidget(m_copy);
    bar->addWidget(m_clear);
    lay->addLayout(bar);

    m_table = new QTableWidget(0, ColN, this);
    m_table->setObjectName(QStringLiteral("errorHistoryTable"));
    m_table->setHorizontalHeaderLabels(
        {tr("时间"), tr("级别"), tr("来源"), tr("次数"), tr("标题"), tr("内容")});
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->verticalHeader()->hide();
    m_table->horizontalHeader()->setStretchLastSection(true);
    lay->addWidget(m_table, 1);

    m_refresh.setSingleShot(true);
    m_refresh.setInterval(150);
    connect(&m_refresh, &QTimer::timeout, this, &ErrorHistoryPanel::refreshNow);
    connect(m_level, &QComboBox::currentIndexChanged, this, &ErrorHistoryPanel::refreshNow);
    connect(m_filter, &QLineEdit::textChanged, this, &ErrorHistoryPanel::refreshNow);
    connect(m_copy, &QPushButton::clicked, this, &ErrorHistoryPanel::copyToClipboard);
    connect(m_clear, &QPushButton::clicked, this, [this] {
        if (m_hub)
            m_hub->clear();
    });
    if (hub)
    {
        connect(hub, &ErrorHub::errorRaised, this,
                [this](const ErrorHub::Entry &, bool) { scheduleRefresh(); });
        connect(hub, &ErrorHub::historyCleared, this, &ErrorHistoryPanel::refreshNow);
    }
    refreshNow();
}

void ErrorHistoryPanel::setLevelFilter(int index)
{
    m_level->setCurrentIndex(index);
}

void ErrorHistoryPanel::setTextFilter(const QString &text)
{
    m_filter->setText(text);
}

int ErrorHistoryPanel::rowCount() const
{
    return m_table->rowCount();
}

void ErrorHistoryPanel::scheduleRefresh()
{
    m_dirty = true;
    if (isVisible() && !m_refresh.isActive())
        m_refresh.start();
}

void ErrorHistoryPanel::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (m_dirty)
        refreshNow();
}

void ErrorHistoryPanel::refreshNow()
{
    m_refresh.stop();
    m_dirty = false;
    ErrorHub::Filter f;
    switch (m_level->currentIndex())
    {
    case 1: f.levelMask = 1 << int(ErrorHub::Level::Error); break;
    case 2: f.levelMask = 1 << int(ErrorHub::Level::Warning); break;
    case 3: f.levelMask = 1 << int(ErrorHub::Level::Info); break;
    default: break;
    }
    f.contains = m_filter->text().trimmed();
    const QVector<ErrorHub::Entry> rows = m_hub ? m_hub->entries(f) : QVector<ErrorHub::Entry>{};
    m_table->setUpdatesEnabled(false);
    m_table->clearContents();
    m_table->setRowCount(int(rows.size()));
    // 新在上：倒序填表。
    for (int i = 0; i < rows.size(); ++i)
    {
        const ErrorHub::Entry &e = rows.at(rows.size() - 1 - i);
        const QString time = QDateTime::fromMSecsSinceEpoch(e.lastMs)
                                 .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
        const QString cells[ColN] = {time, levelLabel(e.level), e.source,
                                     QString::number(e.count), e.title, e.text};
        for (int c = 0; c < ColN; ++c)
        {
            auto *item = new QTableWidgetItem(cells[c]);
            if (c == ColText)
                item->setToolTip(e.text);
            m_table->setItem(i, c, item);
        }
    }
    m_table->setUpdatesEnabled(true);
    m_copy->setEnabled(!rows.isEmpty());
    m_clear->setEnabled(m_hub && m_hub->size() > 0);
}

QString ErrorHistoryPanel::copyText() const
{
    QSet<int> selected;
    for (const QModelIndex &idx : m_table->selectionModel()->selectedRows())
        selected.insert(idx.row());
    QStringList lines;
    for (int r = 0; r < m_table->rowCount(); ++r)
    {
        if (!selected.isEmpty() && !selected.contains(r))
            continue;
        QStringList cells;
        for (int c = 0; c < ColN; ++c)
            cells << oneLine(m_table->item(r, c) ? m_table->item(r, c)->text() : QString());
        lines << cells.join(QLatin1Char('\t'));
    }
    return lines.join(QLatin1Char('\n'));
}

void ErrorHistoryPanel::copyToClipboard()
{
    if (QClipboard *cb = QApplication::clipboard())
        cb->setText(copyText());
}
