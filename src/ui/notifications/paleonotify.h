// 层：视图
#pragma once
#include <QString>

class QWidget;

// 方向64：视图层错误/提示统一出口（取代散落的 QMessageBox 静态调用）。
//
// 呈现规则见 DESIGN.md「错误呈现」节：
// - warning()/information()/critical() 在「全局 ErrorHub 已安装且有存活呈现层
//   (NotificationCenter)」时入账 ErrorHub（warning→通知卡、information→状态栏、
//   critical→severe 模态且同键 60s 单弹），**立即返回、不阻塞**；
//   否则（单测、组装根之前）原样回落迁移前的 QMessageBox 静态调用。
// - 调用方身处另一个可见的非对话框顶层窗（独立设计器等，主窗口通知卡可能被
//   盖住）时：入账 historyOnly + 旧模态呈现。
// - 文本逐字透传（title/text 不改写）；source 缺省取 parent 的类名作来源域。
// - report()：模态保留清单内的「需阅读的报告型提示」，始终模态 information。
// - ask*()：模态保留清单内的「用户裁决」确认，始终模态，按钮/默认键与迁移前
//   QMessageBox 调用逐一对应（返回值=是否点了接受键）。
namespace PaleoNotify
{
void warning(QWidget *parent, const QString &title, const QString &text,
             const QString &source = QString());
void information(QWidget *parent, const QString &title, const QString &text,
                 const QString &source = QString());
void critical(QWidget *parent, const QString &title, const QString &text,
              const QString &source = QString());

// 模态保留：报告型提示（多行统计/路径清单等需要用户读完的内容）。
void report(QWidget *parent, const QString &title, const QString &text);

enum class AskButtons
{
    YesNo,         // QMessageBox::Yes | No（question 缺省按钮）
    OkCancel,      // Ok | Cancel
    DiscardCancel, // Discard | Cancel
    RetryCancel,   // Retry | Cancel
};
enum class AskDefault
{
    Platform,      // QMessageBox::NoButton（交给 Qt 选，同 question 缺省参数）
    Accept,        // 接受键（Yes/Ok/Discard/Retry）
    Reject,        // 拒绝键（No/Cancel）
};
enum class AskIcon
{
    Question,      // QMessageBox::question
    Warning,       // QMessageBox::warning
};
// 返回 true = 用户点了接受键（Yes/Ok/Discard/Retry）。
bool ask(QWidget *parent, const QString &title, const QString &text,
         AskButtons buttons = AskButtons::YesNo,
         AskDefault def = AskDefault::Platform, AskIcon icon = AskIcon::Question);

enum class SaveChoice { Save, Discard, Cancel };
// 三键「保存 / 放弃 / 取消」（AcceptRole / DestructiveRole / RejectRole）。
// 按钮文案由调用方 tr() 传入（保留原翻译上下文）。默认键 = 保存；关窗/Esc = Cancel。
SaveChoice askSaveDiscard(QWidget *parent, AskIcon icon, const QString &title,
                          const QString &text, const QString &saveText,
                          const QString &discardText, const QString &cancelText);
} // namespace PaleoNotify
