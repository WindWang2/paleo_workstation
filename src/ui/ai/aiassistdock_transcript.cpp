// 层：视图
#include "aiassistdock.h"

#include "../../ai/chat/markdown.h"
#include <QDesktopServices>
#include <QElapsedTimer>
#include <QEvent>
#include <QScrollBar>
#include <QTextBrowser>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocumentFragment>
#include <QTimer>
#include <QUrl>

namespace {
QString transcriptStyles() {
  const auto &t = PaleoTheme::tokens();
  return PaleoTheme::metricStyleSheet(QStringLiteral(
    "p {margin:0 0 {spacing.xs}px 0} "
    "h1 {font-size:{typography.display}pt} h2 {font-size:{typography.title}pt} "
    "h3,h4,h5,h6 {font-size:{typography.body}pt} "
    "h1,h2,h3,h4,h5,h6 {margin:{spacing.sm}px 0 {spacing.xs}px 0} "
    "blockquote {margin:{spacing.xs}px 0 {spacing.xs}px {spacing.md}px; color:%1} "
    "pre,code {font-family:'%2'; font-size:{typography.mono}pt; background-color:%3} "
    "pre {margin:{spacing.sm}px 0} "
    "table {border-collapse:collapse; border:1px solid %4} "
    "th,td {border:1px solid %4; padding:{spacing.xs}px {spacing.sm}px} "
    "th {background-color:%3} a {color:%5; text-decoration:underline}"))
    .arg(t.textMuted.name(), PaleoTheme::monoFont().family(), t.surfaceAlt.name(),
         t.border.name(), t.primaryText.name());
}

void beginEntry(QTextCursor &cursor, const QString &label, bool error = false) {
  const auto &t = PaleoTheme::tokens();
  QTextBlockFormat heading;
  heading.setTopMargin(cursor.atStart() ? 0 : t.spacingMd);
  heading.setBottomMargin(t.spacingXs);
  QTextCharFormat caption;
  caption.setFont(PaleoTheme::bodyFont(t.labelPt));
  caption.setFontWeight(QFont::DemiBold);
  caption.setForeground(error ? t.errorText : t.textMuted);
  if (cursor.atStart())
    cursor.setBlockFormat(heading);
  else
    cursor.insertBlock(heading, caption);
  cursor.insertText(label, caption);
  cursor.insertBlock(QTextBlockFormat(), QTextCharFormat());
}

bool followsLatest(QTextBrowser *browser) {
  const auto *scroll = browser->verticalScrollBar();
  return !browser->textCursor().hasSelection() &&
         scroll->value() >= scroll->maximum() - scroll->singleStep();
}

void insertBody(QTextCursor &cursor, const QString &text, bool markdown) {
  if (!markdown) {
    cursor.insertText(text, QTextCharFormat());
    return;
  }
  QTextDocument rendered;
  rendered.setDefaultFont(cursor.document()->defaultFont());
  rendered.setDefaultStyleSheet(cursor.document()->defaultStyleSheet());
  rendered.setHtml(AiMarkdown::toHtml(text));
  // Qt merges a fragment's leading paragraph into the receiving block. Seed its
  // semantic format before insertion; list and frame IDs belong to each document.
  auto first = rendered.firstBlock().blockFormat();
  first.clearProperty(QTextFormat::ObjectIndex);
  cursor.setBlockFormat(first);
  cursor.insertFragment(QTextDocumentFragment(&rendered));
}

void restoreSelection(QTextBrowser *view, int anchor, int position, const QString &text,
                      int bodyStart) {
  auto *doc = view->document();
  QTextCursor range(doc);
  range.setPosition(qMin(anchor, doc->characterCount() - 1));
  range.setPosition(qMin(position, doc->characterCount() - 1), QTextCursor::KeepAnchor);
  if (range.selectedText() != text) {
    // An unfinished delimiter may have become markup before the selected words.
    range = doc->find(text, bodyStart, QTextDocument::FindCaseSensitively);
    if (range.isNull())
      return;
  }
  view->setTextCursor(range);
}
} // namespace

QTextBrowser *AiAssistDock::createTranscript() {
  m_previewTimer = new QTimer(this);
  m_previewTimer->setSingleShot(true);
  m_previewTimer->setInterval(100);
  connect(m_previewTimer, &QTimer::timeout, this, [this] {
    const auto selection = m_transcript->textCursor();
    if (!selection.hasSelection() || selection.selectionEnd() <= m_assistantStart)
      renderAssistantDraft();
  });
  auto *browser = new QTextBrowser(this);
  browser->setAccessibleName(tr("对话记录"));
  browser->document()->setDefaultStyleSheet(transcriptStyles());
  browser->document()->setDocumentMargin(PaleoTheme::tokens().spacingSm);
  browser->setOpenLinks(false);
  connect(browser, &QTextBrowser::anchorClicked, this, [](const QUrl &url) {
    if (url.isValid() && !url.host().isEmpty() &&
        (url.scheme() == QLatin1String("https") || url.scheme() == QLatin1String("http")))
      QDesktopServices::openUrl(url);
  });
  return browser;
}

void AiAssistDock::clearTranscript() {
  m_previewTimer->stop();
  m_transcriptEntries.clear();
  m_assistantStart = -1;
  m_assistantDraft.clear();
  m_transcript->clear();
}

void AiAssistDock::appendEntry(const TranscriptEntry &entry) {
  const bool follow = followsLatest(m_transcript);
  QTextCursor cursor(m_transcript->document());
  cursor.movePosition(QTextCursor::End);
  cursor.beginEditBlock();
  beginEntry(cursor, entry.label, entry.error);
  insertBody(cursor, entry.text, entry.markdown);
  cursor.endEditBlock();
  m_transcriptEntries.append(entry);
  if (follow)
    m_transcript->verticalScrollBar()->setValue(m_transcript->verticalScrollBar()->maximum());
  updateActionStates();
}

void AiAssistDock::appendMessage(const ChatMessage &message) {
  if (message.role == ChatRole::Assistant && message.content.isEmpty())
    return;
  finalizeAssistantBlock();
  if (message.localImportReceipt) {
    const auto newline = message.content.indexOf(QLatin1Char('\n'));
    appendEntry(newline < 0 ? TranscriptEntry{tr("文件导入"), message.content}
                            : TranscriptEntry{message.content.left(newline), message.content.mid(newline + 1)});
    return;
  }
  switch (message.role) {
  case ChatRole::User:      appendEntry({tr("你"), message.content}); break;
  case ChatRole::Assistant: appendEntry({tr("助手"), message.content, true}); break;
  case ChatRole::System:    appendEntry({tr("系统"), message.content}); break;
  case ChatRole::Tool:
    appendEntry({tr("工具"), tr("结果已回传模型（%1 字符）").arg(message.content.size())});
    break;
  }
}

void AiAssistDock::appendDelta(const QString &text) {
  if (text.isEmpty())
    return;
  m_assistantDraft += text;
  if (m_assistantStart < 0) {
    if (text.trimmed().isEmpty())
      return;
    const bool follow = followsLatest(m_transcript);
    QTextCursor cursor(m_transcript->document());
    cursor.movePosition(QTextCursor::End);
    beginEntry(cursor, tr("助手"));
    m_assistantStart = cursor.position();
    m_previewLength = -1;
    m_previewTimer->setInterval(100);
    renderAssistantDraft();
    if (follow)
      m_transcript->verticalScrollBar()->setValue(m_transcript->verticalScrollBar()->maximum());
    return;
  }
  if (!m_previewTimer->isActive())
    m_previewTimer->start();
}

void AiAssistDock::renderAssistantDraft() {
  if (m_assistantStart < 0 || m_previewLength == m_assistantDraft.size())
    return;
  QElapsedTimer cost;
  cost.start();
  const bool follow = followsLatest(m_transcript);
  const auto selection = m_transcript->textCursor();
  const int anchor = selection.anchor(), position = selection.position();
  const QString selected = selection.hasSelection() && selection.selectionEnd() > m_assistantStart
                         ? selection.selectedText() : QString();
  auto *scroll = m_transcript->verticalScrollBar();
  const int offset = scroll->value();
  QTextCursor cursor(m_transcript->document());
  cursor.setPosition(m_assistantStart);
  cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
  cursor.beginEditBlock();
  cursor.removeSelectedText();
  insertBody(cursor, m_assistantDraft, true);
  cursor.endEditBlock();
  if (!selected.isEmpty()) {
    restoreSelection(m_transcript, anchor, position, selected, m_assistantStart);
    scroll->setValue(offset);
  }
  m_previewLength = m_assistantDraft.size();
  // Coalesce tokens and give long replies more time between layout passes.
  m_previewTimer->setInterval(int(qBound(qint64(100), cost.elapsed() * 8, qint64(1500))));
  if (follow)
    scroll->setValue(scroll->maximum());
}

void AiAssistDock::finalizeAssistantBlock() {
  m_previewTimer->stop();
  if (m_assistantStart < 0) {
    m_assistantDraft.clear();
    return;
  }
  renderAssistantDraft();
  m_transcriptEntries.append({tr("助手"), m_assistantDraft, true});
  m_assistantDraft.clear();
  m_assistantStart = -1;
  updateActionStates();
}

void AiAssistDock::appendError(const QString &message) {
  finalizeAssistantBlock();
  appendEntry({tr("错误"), message, false, true});
}

void AiAssistDock::changeEvent(QEvent *event) {
  QWidget::changeEvent(event);
  if (m_transcript && m_transcriptTheme != PaleoTheme::currentTheme())
    refreshTranscriptTheme();
}

void AiAssistDock::refreshTranscriptTheme() {
  m_transcriptTheme = PaleoTheme::currentTheme();
  auto *document = m_transcript->document();
  document->setDefaultStyleSheet(transcriptStyles());
  const auto saved = m_transcript->textCursor();
  const int anchor = saved.anchor(), position = saved.position();
  const QString selected = saved.selectedText();
  auto *scroll = m_transcript->verticalScrollBar();
  const int offset = scroll->value();
  const bool follow = followsLatest(m_transcript);
  QTextCursor cursor(document);
  cursor.beginEditBlock();
  cursor.select(QTextCursor::Document);
  cursor.removeSelectedText();
  for (const auto &entry : m_transcriptEntries) {
    beginEntry(cursor, entry.label, entry.error);
    insertBody(cursor, entry.text, entry.markdown);
  }
  if (m_assistantStart >= 0) {
    beginEntry(cursor, tr("助手"));
    m_assistantStart = cursor.position();
    insertBody(cursor, m_assistantDraft, true);
    m_previewLength = m_assistantDraft.size();
  }
  cursor.endEditBlock();
  restoreSelection(m_transcript, anchor, position, selected, 0);
  scroll->setValue(follow ? scroll->maximum() : offset);
}
