// 层：视图
#include "paleonotify.h"

#include "../../services/errorhub.h"
#include "notificationcenter.h"

#include <QPushButton>
#include <QMessageBox>
#include <QWidget>

namespace
{
ErrorHub *activeHub()
{
    ErrorHub *hub = ErrorHub::global();
    return NotificationCenter::hasPresenter(hub) ? hub : nullptr;
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
    if (ErrorHub *hub = activeHub())
        hub->raise(ErrorHub::Level::Warning, sourceOf(parent, source), title, text);
    else
        QMessageBox::warning(parent, title, text);
}

void information(QWidget *parent, const QString &title, const QString &text,
                 const QString &source)
{
    if (ErrorHub *hub = activeHub())
        hub->raise(ErrorHub::Level::Info, sourceOf(parent, source), title, text);
    else
        QMessageBox::information(parent, title, text);
}

void critical(QWidget *parent, const QString &title, const QString &text,
              const QString &source)
{
    if (ErrorHub *hub = activeHub())
        hub->raise(ErrorHub::Level::Error, sourceOf(parent, source), title, text,
                   QString(), /*severe=*/true);
    else
        QMessageBox::critical(parent, title, text);
}

void report(QWidget *parent, const QString &title, const QString &text)
{
    // 报告型提示属模态保留清单：仍入账历史（info，不触发呈现分流之外的弹窗——
    // info 级只走状态栏），再模态展示全文。
    if (ErrorHub *hub = ErrorHub::global())
        hub->raise(ErrorHub::Level::Info, sourceOf(parent, QString()), title, text);
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
    const SB answer = icon == AskIcon::Warning
                          ? QMessageBox::warning(parent, title, text, accept | reject, dflt)
                          : QMessageBox::question(parent, title, text, accept | reject, dflt);
    return answer == accept;
}

SaveChoice askSaveDiscard(QWidget *parent, AskIcon icon, const QString &title,
                          const QString &text, const QString &saveText,
                          const QString &discardText, const QString &cancelText)
{
    QMessageBox box(icon == AskIcon::Warning ? QMessageBox::Warning : QMessageBox::Question,
                    title, text, QMessageBox::NoButton, parent);
    QPushButton *save = box.addButton(saveText, QMessageBox::AcceptRole);
    QPushButton *discard = box.addButton(discardText, QMessageBox::DestructiveRole);
    box.addButton(cancelText, QMessageBox::RejectRole);
    box.exec();
    if (box.clickedButton() == save)
        return SaveChoice::Save;
    if (box.clickedButton() == discard)
        return SaveChoice::Discard;
    return SaveChoice::Cancel;
}
} // namespace PaleoNotify
