// 层：视图
#include "toastcard.h"

#include "../paleotheme.h"

#include <QCoreApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

ToastCard::ToastCard(QWidget *parent) : QFrame(parent)
{
    setObjectName(QStringLiteral("paleoToastCard"));
    setAttribute(Qt::WA_StyledBackground, true);
    setFixedWidth(360);  // = NotificationCenter::kCardWidth（DESIGN.md）
    const auto &t = PaleoTheme::tokens();
    // 样式只在构造时生成一次；级别切换只改动态属性 + repolish，不重建串。
    setStyleSheet(QStringLiteral(
        "QFrame#paleoToastCard{background:%1;border:1px solid %2;border-radius:%3px;"
        "border-left:4px solid %2;}"
        "QFrame#paleoToastCard[level=\"warning\"]{border-left:4px solid %4;}"
        "QFrame#paleoToastCard[level=\"error\"]{border-left:4px solid %5;}"
        "QLabel#toastLevel[level=\"warning\"]{color:%6;font-weight:bold;}"
        "QLabel#toastLevel[level=\"error\"]{color:%7;font-weight:bold;}"
        "QLabel#toastTitle{font-weight:bold;color:%8;}"
        "QLabel#toastText{color:%8;}"
        "QLabel#toastCount{background:%9;color:%8;border-radius:%10px;padding:0 6px;}")
                      .arg(t.surface.name(), t.border.name())
                      .arg(t.radiusMd)
                      .arg(t.warning.name(), t.error.name(), t.warningText.name(),
                           t.errorText.name(), t.text.name(), t.surfaceAlt.name())
                      .arg(t.radiusSm));

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(t.spacingSm + 4, t.spacingSm, t.spacingSm, t.spacingSm);
    outer->setSpacing(t.spacingXs);
    auto *head = new QHBoxLayout;
    head->setSpacing(t.spacingSm);
    m_level = new QLabel(this);
    m_level->setObjectName(QStringLiteral("toastLevel"));
    m_title = new QLabel(this);
    m_title->setObjectName(QStringLiteral("toastTitle"));
    m_title->setTextFormat(Qt::PlainText);
    m_count = new QLabel(this);
    m_count->setObjectName(QStringLiteral("toastCount"));
    m_count->hide();
    m_close = new QToolButton(this);
    m_close->setObjectName(QStringLiteral("toastClose"));
    m_close->setText(QString(QChar(0x2715)));
    m_close->setAutoRaise(true);
    m_close->setAccessibleName(QCoreApplication::translate("ToastCard", "关闭通知"));
    head->addWidget(m_level);
    head->addWidget(m_title, 1);
    head->addWidget(m_count);
    head->addWidget(m_close);
    outer->addLayout(head);
    m_text = new QLabel(this);
    m_text->setObjectName(QStringLiteral("toastText"));
    m_text->setTextFormat(Qt::PlainText);
    m_text->setWordWrap(true);
    m_text->setTextInteractionFlags(Qt::TextSelectableByMouse);
    outer->addWidget(m_text);

    m_timer.setSingleShot(true);
    QObject::connect(&m_timer, &QTimer::timeout, this, [this] { dismiss(); });
    QObject::connect(m_close, &QToolButton::clicked, this, [this] { dismiss(); });
    hide();
}

void ToastCard::applyLevel(ErrorHub::Level level)
{
    if (m_levelShown == int(level))
        return;
    m_levelShown = int(level);
    const char *name = level == ErrorHub::Level::Error ? "error" : "warning";
    setProperty("level", QLatin1String(name));
    m_level->setProperty("level", QLatin1String(name));
    m_level->setText(level == ErrorHub::Level::Error ? QCoreApplication::translate("ToastCard", "错误")
                                                      : QCoreApplication::translate("ToastCard", "警告"));
    style()->unpolish(this);
    style()->polish(this);
    m_level->style()->unpolish(m_level);
    m_level->style()->polish(m_level);
}

void ToastCard::present(const ErrorHub::Entry &entry, int ms, quint64 seq)
{
    applyLevel(entry.level);
    m_key = entry.dedupKey;
    m_seq = seq;
    m_title->setText(entry.title);
    m_text->setText(entry.text);
    m_title->setVisible(!entry.title.isEmpty());
    m_count->setVisible(entry.count > 1);
    if (entry.count > 1)
        m_count->setText(QStringLiteral("\u00d7%1").arg(entry.count));
    m_active = true;
    adjustSize();
    show();
    raise();
    m_timer.start(ms);
}

void ToastCard::bump(int count, int ms)
{
    m_count->setText(QStringLiteral("\u00d7%1").arg(count));
    m_count->setVisible(count > 1);
    m_timer.start(ms);
}

void ToastCard::dismiss()
{
    if (!m_active)
        return;
    m_active = false;
    m_timer.stop();
    hide();
    if (onDismissed)
        onDismissed();
}

QString ToastCard::text() const
{
    return m_text->text();
}
