// 层：视图
#include "paleonotify.h"

#include "../../services/errorhub.h"
#include "../uienv_internal.h"
#include "notificationcenter.h"

#include <QCoreApplication>
#include <QCursor>
#include <QDebug>
#include <QToolTip>
#include <QDialog>
#include <QPointer>
#include <QPushButton>
#include <QMessageBox>
#include <QTimer>
#include <QWidget>

using paleo::ui_detail::isOffscreen;

namespace
{
ErrorHub *activeHub()
{
    ErrorHub *hub = ErrorHub::global();
    return NotificationCenter::hasPresenter(hub) ? hub : nullptr;
}

// 通知卡锚定在呈现层宿主（主窗口）上。调用方若身处另一个可见的非对话框
// 顶层窗（如独立的版面设计器主窗口），主窗口上的卡可能被整个盖住——此时
// 保留旧模态呈现（只入账历史）。对话框（多叠在主窗口之上）照常走通知卡。
bool presentsOnHost(ErrorHub *hub, QWidget *parent)
{
    QWidget *host = NotificationCenter::presenterHost(hub);
    if (!parent || !host)
        return true;
    QWidget *win = parent->window();
    return win == host->window() || !win->isVisible() ||
           qobject_cast<QDialog *>(win) != nullptr;
}

QString sourceOf(QWidget *parent, const QString &source)
{
    if (!source.isEmpty())
        return source;
    return parent ? QString::fromLatin1(parent->metaObject()->className())
                  : QStringLiteral("app");
}
} // namespace

namespace PaleoNotify
{
void warning(QWidget *parent, const QString &title, const QString &text,
             const QString &source)
{
    ErrorHub *hub = activeHub();
    const bool onHost = hub && presentsOnHost(hub, parent);
    if (hub)
        hub->raise(ErrorHub::Level::Warning, sourceOf(parent, source), title, text,
                   QString(), false, /*historyOnly=*/!onHost);
    if (!onHost)
    {
        if (isOffscreen())
        {
            qWarning().noquote() << "[PaleoNotify::warning]" << title << ":" << text;
            return;
        }
        QMessageBox::warning(parent, title, text);
    }
}

void information(QWidget *parent, const QString &title, const QString &text,
                 const QString &source)
{
    ErrorHub *hub = activeHub();
    const bool onHost = hub && presentsOnHost(hub, parent);
    if (hub)
        hub->raise(ErrorHub::Level::Info, sourceOf(parent, source), title, text,
                   QString(), false, /*historyOnly=*/!onHost);
    if (!onHost)
    {
        if (isOffscreen())
        {
            qInfo().noquote() << "[PaleoNotify::information]" << title << ":" << text;
            return;
        }
        QMessageBox::information(parent, title, text);
    }
}

void copied(QWidget *parent, const QString &what)
{
    const QString subject =
        what.isEmpty() ? QCoreApplication::translate("PaleoNotify", "内容") : what;
    const QString text =
        QCoreApplication::translate("PaleoNotify", "%1已复制到剪贴板").arg(subject);
    ErrorHub *hub = activeHub();
    if (hub && presentsOnHost(hub, parent))
    {
        hub->raise(ErrorHub::Level::Info, sourceOf(parent, QString()),
                   QCoreApplication::translate("PaleoNotify", "已复制"), text);
        return;
    }
    if (isOffscreen())
    {
        qInfo().noquote() << "[PaleoNotify::copied]" << text;
        return;
    }
    QToolTip::showText(QCursor::pos(), text, parent);
}

void critical(QWidget *parent, const QString &title, const QString &text,
              const QString &source)
{
    ErrorHub *hub = activeHub();
    const bool onHost = hub && presentsOnHost(hub, parent);
    if (hub)
        hub->raise(ErrorHub::Level::Error, sourceOf(parent, source), title, text,
                   QString(), /*severe=*/true, /*historyOnly=*/!onHost);
    if (!onHost)
    {
        if (isOffscreen())
        {
            qCritical().noquote() << "[PaleoNotify::critical]" << title << ":" << text;
            return;
        }
        QMessageBox::critical(parent, title, text);
    }
}

void report(QWidget *parent, const QString &title, const QString &text)
{
    // 报告型提示属模态保留清单：只入账历史（historyOnly，不再上状态栏），
    // 再模态展示全文。
    if (ErrorHub *hub = ErrorHub::global())
        hub->raise(ErrorHub::Level::Info, sourceOf(parent, QString()), title, text,
                   QString(), false, /*historyOnly=*/true);
    if (isOffscreen())
    {
        qInfo().noquote() << "[PaleoNotify::report]" << title << ":" << text;
        return;
    }
    QMessageBox::information(parent, title, text);
}

bool ask(QWidget *parent, const QString &title, const QString &text,
         AskButtons buttons, AskDefault def, AskIcon icon)
{
    using SB = QMessageBox::StandardButton;
    SB accept = SB::Yes;
    SB reject = SB::No;
    switch (buttons)
    {
    case AskButtons::YesNo: break;
    case AskButtons::OkCancel: accept = SB::Ok; reject = SB::Cancel; break;
    case AskButtons::DiscardCancel: accept = SB::Discard; reject = SB::Cancel; break;
    case AskButtons::RetryCancel: accept = SB::Retry; reject = SB::Cancel; break;
    }
    const SB dflt = def == AskDefault::Accept   ? accept
                    : def == AskDefault::Reject ? reject
                                                : SB::NoButton;
    QMessageBox box(icon == AskIcon::Warning ? QMessageBox::Warning : QMessageBox::Question,
                    title, text, accept | reject, parent);
    box.setDefaultButton(dflt);

    if (isOffscreen())
    {
        // 无头守卫（B-05）：若无测试协作驱动器接管，短时限自动按默认非阻塞值关闭，绝不悬挂
        QPointer<QMessageBox> guard(&box);
        QTimer::singleShot(50, &box, [guard, dflt, accept] {
            if (guard && guard->isVisible())
            {
                if (auto *btn = guard->button(dflt != QMessageBox::NoButton ? dflt : accept))
                    btn->click();
                else
                    guard->reject();
            }
        });
    }

    const int res = box.exec();
    return res == accept;
}

SaveChoice askSaveDiscard(QWidget *parent, AskIcon icon, const QString &title,
                          const QString &text, const QString &saveText,
                          const QString &discardText, const QString &cancelText)
{
    QMessageBox box(icon == AskIcon::Warning ? QMessageBox::Warning : QMessageBox::Question,
                    title, text, QMessageBox::NoButton, parent);
    // Empty saveText = no Save button (read-only callers, PW-008).
    QPushButton *save = saveText.isEmpty() ? nullptr
                                           : box.addButton(saveText, QMessageBox::AcceptRole);
    QPushButton *discard = box.addButton(discardText, QMessageBox::DestructiveRole);
    QPushButton *cancel = box.addButton(cancelText, QMessageBox::RejectRole);
    box.setDefaultButton(save ? save : cancel);  // 两处原调用均以「保存」为默认键

    if (isOffscreen())
    {
        // 无头守卫（B-05）：若无测试协作驱动器接管，短时限自动按保存默认值关闭，绝不悬挂
        QPointer<QMessageBox> guard(&box);
        QTimer::singleShot(50, &box, [guard, save] {
            if (guard && guard->isVisible())
            {
                if (save)
                    save->click();
                else
                    guard->reject();
            }
        });
    }

    box.exec();
    if (save && box.clickedButton() == save)
        return SaveChoice::Save;
    if (box.clickedButton() == discard)
        return SaveChoice::Discard;
    return SaveChoice::Cancel;
}
} // namespace PaleoNotify
