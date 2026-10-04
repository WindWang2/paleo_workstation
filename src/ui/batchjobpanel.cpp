// 层：视图
#include "batchjobpanel.h"

#include <QHideEvent>
#include <QHeaderView>
#include <QLabel>
#include <QShowEvent>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "paleotheme.h"
#include "workflow/batchjobqueue.h"

using PaleoBatchQueue::BatchQueue;
using PaleoBatchQueue::ItemState;

namespace
{
/// 状态 → 展示色。用 PaleoTheme token（色值单一真源），不在本文件写 hex。
QColor colorForState(ItemState state)
{
    using PaleoTheme::kColorError;
    using PaleoTheme::kColorSuccess;
    using PaleoTheme::kColorTextMuted;
    using PaleoTheme::kColorWarning;
    switch (state)
    {
        case ItemState::Succeeded:
            return kColorSuccess;
        case ItemState::Skipped:
            return kColorTextMuted;
        case ItemState::Failed:
            return kColorError;
        case ItemState::Cancelled:
            return kColorWarning;
        default:
            return kColorTextMuted;
    }
}

/// 状态 → 优先列前缀。人读一眼分得清「跳过 ≠ 失败」（跳满是幂等命中）。
QString prefixForState(ItemState state)
{
    switch (state)
    {
        case ItemState::Pending:
            return QObject::tr("待跑");
        case ItemState::Queued:
            return QObject::tr("排队");
        case ItemState::Running:
            return QObject::tr("计算");
        case ItemState::Succeeded:
            return QObject::tr("完成");
        case ItemState::Failed:
            return QObject::tr("失败");
        case ItemState::Skipped:
            return QObject::tr("跳过");
        case ItemState::Cancelled:
            return QObject::tr("取消");
    }
    return QObject::tr("未知");
}

/// 耗时格式化（人读展示，非性能断言）。
QString formatElapsed(qint64 ms)
{
    if (ms <= 0)
        return QStringLiteral("--");
    if (ms < 1000)
        return QObject::tr("%1 ms").arg(ms);
    if (ms < 60000)
        return QObject::tr("%1 s").arg(ms / 1000);
    const qint64 m = ms / 60000;
    const qint64 s = (ms % 60000) / 1000;
    return QObject::tr("%1 分 %2 秒").arg(m).arg(s);
}
} // namespace

BatchJobPanel::BatchJobPanel(BatchQueue *queue, QWidget *parent)
    : QWidget(parent), m_queue(queue)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm);
    layout->setSpacing(PaleoTheme::tokens().spacingSm);

    m_summary = new QLabel(this);
    m_summary->setStyleSheet(PaleoTheme::mutedCaptionStyleSheet());
    layout->addWidget(m_summary);

    m_list = new QTreeWidget(this);
    m_list->setColumnCount(5);
    m_list->setHeaderLabels({tr("层位"), tr("方法"), tr("状态"), tr("进度"),
                             tr("耗时 / 原因")});
    m_list->setRootIsDecorated(false);
    m_list->setAlternatingRowColors(true);
    m_list->header()->setSectionResizeMode(4, QHeaderView::Stretch);
    layout->addWidget(m_list);

    // 只观察，不发指令：三个信号是本面板的全部输入面。
    if (m_queue)
    {
        connect(m_queue, &BatchQueue::itemChanged, this,
                &BatchJobPanel::onItemChanged);
        connect(m_queue, &BatchQueue::batchProgress, this,
                &BatchJobPanel::onProgress);
        connect(m_queue, &BatchQueue::batchFinished, this,
                &BatchJobPanel::onFinished);
    }

    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(500);
    connect(m_pollTimer, &QTimer::timeout, this, &BatchJobPanel::refresh);

    refresh();
}

void BatchJobPanel::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    m_pollTimer->start(); // 可见才轮询
}

void BatchJobPanel::hideEvent(QHideEvent *event)
{
    m_pollTimer->stop();
    QWidget::hideEvent(event);
}

QTreeWidgetItem *BatchJobPanel::rowFor(const QString &itemId)
{
    if (m_rows.contains(itemId))
        return m_rows.value(itemId);
    auto *row = new QTreeWidgetItem(m_list);
    m_rows.insert(itemId, row);
    return row;
}

void BatchJobPanel::updateRow(QTreeWidgetItem *row, const QString &itemId)
{
    if (!m_queue || !row)
        return;
    for (const PaleoBatchQueue::BatchItem &it : m_queue->items())
    {
        if (it.itemId != itemId)
            continue;
        row->setText(0, it.horizon);
        row->setText(1, it.methodId);
        row->setText(2, prefixForState(it.state) + QLatin1String(" · ") +
                            PaleoBatchQueue::itemStateText(it.state));
        row->setText(3, QStringLiteral("%1%").arg(it.percent));
        // 末列：有原因先给原因（失败排查第一入口），否则给耗时。
        row->setText(4, it.error.isEmpty() ? formatElapsed(it.elapsedMs)
                                            : it.error);
        // 用 itemWidget 上色代价高；状态列的前景色足够区分，且不引 hex。
        row->setForeground(2, colorForState(it.state));
        return;
    }
}

void BatchJobPanel::updateSummary()
{
    if (!m_queue)
    {
        m_summary->setText(tr("批次队列未就绪"));
        return;
    }
    const PaleoBatchQueue::BatchReport r = m_queue->report();
    if (r.total == 0)
    {
        m_summary->setText(tr("当前无批次"));
        return;
    }
    m_summary->setText(r.summary());
}

void BatchJobPanel::onItemChanged(const QString &itemId)
{
    updateRow(rowFor(itemId), itemId);
    updateSummary();
}

void BatchJobPanel::onProgress(int done, int total)
{
    Q_UNUSED(done);
    Q_UNUSED(total);
    updateSummary();
}

void BatchJobPanel::onFinished()
{
    // 批末全量对账一次：终态/耗时此刻才齐。
    refresh();
}

void BatchJobPanel::refresh()
{
    if (!m_queue)
    {
        m_summary->setText(tr("批次队列未就绪"));
        return;
    }
    const QVector<PaleoBatchQueue::BatchItem> items = m_queue->items();
    if (items.isEmpty())
    {
        m_summary->setText(tr("当前无批次"));
        m_list->clear();
        m_rows.clear();
        return;
    }
    for (const PaleoBatchQueue::BatchItem &it : items)
        updateRow(rowFor(it.itemId), it.itemId);
    updateSummary();
}
