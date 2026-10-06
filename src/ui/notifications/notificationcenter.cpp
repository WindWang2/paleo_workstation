// 层：视图
#include "notificationcenter.h"

#include "../paleotheme.h"
#include "toastcard.h"

#include <QEvent>
#include <QHash>
#include <QMessageBox>
#include <QStatusBar>
#include <QWidget>
#include <algorithm>

namespace
{
// hub → 存活呈现层计数（主线程访问）。
QHash<const ErrorHub *, int> &presenters()
{
    static QHash<const ErrorHub *, int> map;
    return map;
}
} // namespace

NotificationCenter::NotificationCenter(QWidget *host, ErrorHub *hub)
    : QObject(host), m_host(host), m_hub(hub)
{
    m_cards.reserve(kMaxCards);
    for (int i = 0; i < kMaxCards; ++i)
    {
        auto *card = new ToastCard(host);
        ++m_allocations;
        card->onDismissed = [this] { relayout(); };
        m_cards.append(card);
    }
    if (host)
        host->installEventFilter(this);
    if (hub)
    {
        ++presenters()[hub];
        connect(hub, &ErrorHub::errorRaised, this, &NotificationCenter::onRaised);
    }
}

NotificationCenter::~NotificationCenter()
{
    if (m_hub)
    {
        auto it = presenters().find(m_hub.data());
        if (it != presenters().end() && --it.value() <= 0)
            presenters().erase(it);
    }
    for (ToastCard *card : std::as_const(m_cards))
        if (card)
            card->onDismissed = nullptr;
}

bool NotificationCenter::hasPresenter(const ErrorHub *hub)
{
    return hub && presenters().value(hub, 0) > 0;
}

void NotificationCenter::setStatusBar(QStatusBar *bar)
{
    m_status = bar;
}

void NotificationCenter::setDismissMsForTest(int infoMs, int warningMs, int errorMs)
{
    m_infoMs = infoMs;
    m_warningMs = warningMs;
    m_errorMs = errorMs;
}

int NotificationCenter::dismissMs(ErrorHub::Level level) const
{
    switch (level)
    {
    case ErrorHub::Level::Info: return m_infoMs;
    case ErrorHub::Level::Warning: return m_warningMs;
    case ErrorHub::Level::Error: return m_errorMs;
    }
    return m_warningMs;
}

ToastCard *NotificationCenter::cardForKey(const QString &key) const
{
    for (ToastCard *card : m_cards)
        if (card->isActive() && card->key() == key)
            return card;
    return nullptr;
}

ToastCard *NotificationCenter::takeCard()
{
    ToastCard *oldest = nullptr;
    for (ToastCard *card : std::as_const(m_cards))
    {
        if (!card->isActive())
            return card;
        if (!oldest || card->seq() < oldest->seq())
            oldest = card;
    }
    // 池满：提前收起最旧一张复用（不排队、不溢出）。
    oldest->onDismissed = nullptr;
    oldest->dismiss();
    oldest->onDismissed = [this] { relayout(); };
    return oldest;
}

void NotificationCenter::onRaised(const ErrorHub::Entry &entry, bool firstInWindow)
{
    if (!m_host)
        return;
    if (entry.level == ErrorHub::Level::Info)
    {
        // info 不去重：状态栏是非打扰通道，保持逐次出现的时机。
        if (m_status)
        {
            QString msg = entry.text;
            msg.replace(QLatin1Char('\n'), QLatin1Char(' '));
            m_status->showMessage(msg, m_infoMs);
            ++m_statusShown;
        }
        return;
    }
    if (entry.severe)
    {
        if (!firstInWindow)
            return;  // 同键 60s 单弹
        auto *box = new QMessageBox(QMessageBox::Critical, entry.title, entry.text,
                                    QMessageBox::Ok, m_host);
        box->setAttribute(Qt::WA_DeleteOnClose);
        box->setObjectName(QStringLiteral("paleoSevereErrorBox"));
        box->open();
        ++m_modalShown;
        return;
    }
    const int ms = dismissMs(entry.level);
    if (ToastCard *same = cardForKey(entry.dedupKey))
    {
        same->bump(entry.count, ms);
        return;
    }
    if (!firstInWindow)
        return;  // 窗内已呈现过且卡已收起：只记历史计数，不重弹
    ToastCard *card = takeCard();
    card->present(entry, ms, ++m_seq);
    relayout();
}

void NotificationCenter::relayout()
{
    if (!m_host)
        return;
    const auto &t = PaleoTheme::tokens();
    // 新卡在下，旧卡依次向上；按 seq 降序摆放（池恒 4 张，栈上排序无堆分配）。
    ToastCard *order[kMaxCards];
    int n = 0;
    for (ToastCard *card : std::as_const(m_cards))
        if (card->isActive())
            order[n++] = card;
    std::sort(order, order + n,
              [](const ToastCard *a, const ToastCard *b) { return a->seq() > b->seq(); });
    int bottom = m_host->height() - t.spacingMd;
    const int x = std::max(0, m_host->width() - t.spacingMd - kCardWidth);
    for (int i = 0; i < n; ++i)
    {
        ToastCard *card = order[i];
        const int h = card->height();
        card->move(x, std::max(0, bottom - h));
        card->raise();
        bottom -= h + t.spacingSm;
    }
}

bool NotificationCenter::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_host && event->type() == QEvent::Resize)
        relayout();
    return QObject::eventFilter(watched, event);
}

int NotificationCenter::visibleCardCount() const
{
    int n = 0;
    for (ToastCard *card : m_cards)
        n += card->isActive() && card->isVisible() ? 1 : 0;
    return n;
}

QStringList NotificationCenter::visibleCardTexts() const
{
    QVector<ToastCard *> act;
    for (ToastCard *card : m_cards)
        if (card->isActive())
            act.append(card);
    std::sort(act.begin(), act.end(),
              [](const ToastCard *a, const ToastCard *b) { return a->seq() < b->seq(); });
    QStringList out;
    for (ToastCard *card : act)
        out << card->text();
    return out;
}
