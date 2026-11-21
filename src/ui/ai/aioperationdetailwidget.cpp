// 层：视图
#include "aioperationdetailwidget.h"
#include "../notifications/paleonotify.h"
#include "../paleotheme.h"
#include "../windowsizing_internal.h"
#include "../../workflow/aiconversationpresentation.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

// ui/ai — G5 操作详情区实现。呈现纪律：状态词进文案；错误码原样保留可
// 排查；不因查询成功宣称业务完成；不承诺校验未读取的全文。

namespace {
constexpr int kEventPageSize = 32;   // 与服务端 events 默认页大小一致
constexpr int kPortPageBytes = 8192; // 单页读取字节（≤ 服务 24KiB 上限）
constexpr int kPortPreviewCap = 64 * 1024; // 端口内容呈现累计上限（有界纪律）

QString elidedMiddle(const QString &text, int cap) {
  if (text.size() <= cap)
    return text;
  return text.left(cap / 2) + QStringLiteral("…") + text.right(cap - cap / 2 - 1);
}
QString knownOrUnknown(const QString &value) {
  return value.isEmpty() ? QObject::tr("未知") : value;
}
QTableWidget *compactTable(const QStringList &headers, QWidget *parent) {
  auto *table = new QTableWidget(parent);
  table->setColumnCount(headers.size());
  table->setHorizontalHeaderLabels(headers);
  table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table->setSelectionBehavior(QAbstractItemView::SelectRows);
  table->setSelectionMode(QAbstractItemView::SingleSelection);
  table->setWordWrap(false);
  table->verticalHeader()->hide();
  table->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
  table->horizontalHeader()->setStretchLastSection(true);
  table->setFont(PaleoTheme::bodyFont(PaleoTheme::kLabelPt));
  table->setMaximumHeight(table->horizontalHeader()->sizeHint().height() +
                          6 * table->fontMetrics().lineSpacing() * 3 / 2 +
                          2 * table->frameWidth());
  return table;
}
} // namespace

AiOperationDetailWidget::AiOperationDetailWidget(QWidget *parent) : QWidget(parent) {
  setObjectName(QStringLiteral("aiOperationDetail"));
  auto *layout = new QVBoxLayout(this);
  const auto &tokens = PaleoTheme::tokens();
  layout->setContentsMargins(tokens.spacingXs, tokens.spacingXs, tokens.spacingXs,
                             tokens.spacingXs);
  layout->setSpacing(tokens.spacingXs);

  auto *header = new QHBoxLayout;
  m_title = new QLabel(this);
  m_title->setObjectName(QStringLiteral("aiOperationDetailTitle"));
  m_title->setTextFormat(Qt::PlainText);
  m_title->setFont(PaleoTheme::bodyFont(PaleoTheme::kLabelPt));
  header->addWidget(m_title, 1);
  m_close = new QPushButton(tr("关闭"), this);
  m_close->setObjectName(QStringLiteral("aiOperationDetailClose"));
  header->addWidget(m_close);
  layout->addLayout(header);
  connect(m_close, &QPushButton::clicked, this, [this] {
    hide();
    emit closed();
  });

  m_axes = new QLabel(this);
  m_axes->setObjectName(QStringLiteral("aiOperationDetailAxes"));
  m_axes->setTextFormat(Qt::PlainText);
  m_axes->setWordWrap(true);
  m_axes->setFont(PaleoTheme::bodyFont(PaleoTheme::kLabelPt));
  layout->addWidget(m_axes);

  auto *controls = new QHBoxLayout;
  controls->setSpacing(tokens.spacingXs);
  m_refresh = new QPushButton(tr("刷新状态"), this);
  m_refresh->setObjectName(QStringLiteral("aiOperationDetailRefresh"));
  m_cancel = new QPushButton(tr("取消操作"), this);
  m_cancel->setObjectName(QStringLiteral("aiOperationDetailCancel"));
  m_resume = new QPushButton(tr("恢复执行"), this);
  m_resume->setObjectName(QStringLiteral("aiOperationDetailResume"));
  controls->addWidget(m_refresh);
  controls->addWidget(m_cancel);
  controls->addWidget(m_resume);
  controls->addStretch(1);
  layout->addLayout(controls);
  connect(m_refresh, &QPushButton::clicked, this, [this] {
    if (!m_operationId.isEmpty())
      emit refreshRequested(m_operationId);
  });
  connect(m_cancel, &QPushButton::clicked, this, [this] {
    if (!m_operationId.isEmpty())
      emit cancelRequested(m_operationId);
  });
  connect(m_resume, &QPushButton::clicked, this, [this] {
    if (!m_operationId.isEmpty())
      emit resumeRequested(m_operationId);
  });

  m_stateTable = compactTable({tr("事实"), tr("值")}, this);
  m_stateTable->setObjectName(QStringLiteral("aiOperationDetailFacts"));
  m_stateTable->horizontalHeader()->hide();
  layout->addWidget(m_stateTable);

  auto *eventsTitle = new QLabel(tr("事件（按修订游标分页）"), this);
  eventsTitle->setFont(PaleoTheme::bodyFont(PaleoTheme::kLabelPt));
  layout->addWidget(eventsTitle);
  m_eventsTable = compactTable({tr("修订"), tr("事件")}, this);
  m_eventsTable->setObjectName(QStringLiteral("aiOperationDetailEvents"));
  layout->addWidget(m_eventsTable);
  m_eventsNote = new QLabel(this);
  m_eventsNote->setObjectName(QStringLiteral("aiOperationDetailEventsNote"));
  m_eventsNote->setTextFormat(Qt::PlainText);
  m_eventsNote->setWordWrap(true);
  m_eventsNote->setFont(PaleoTheme::bodyFont(PaleoTheme::kLabelPt));
  PaleoTheme::applyThemedStyleSheet(
    m_eventsNote, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
  layout->addWidget(m_eventsNote);
  auto *eventsControls = new QHBoxLayout;
  eventsControls->setSpacing(tokens.spacingXs);
  m_eventsNext = new QPushButton(tr("下一页事件"), this);
  m_eventsNext->setObjectName(QStringLiteral("aiOperationDetailEventsNext"));
  m_eventsRestart = new QPushButton(tr("重新起读"), this);
  m_eventsRestart->setObjectName(QStringLiteral("aiOperationDetailEventsRestart"));
  eventsControls->addWidget(m_eventsNext);
  eventsControls->addWidget(m_eventsRestart);
  eventsControls->addStretch(1);
  layout->addLayout(eventsControls);
  connect(m_eventsNext, &QPushButton::clicked, this, [this] {
    if (m_operationId.isEmpty())
      return;
    bool ok = false;
    const auto after = m_eventsCursor.toULongLong(&ok);
    emit eventsRequested(m_operationId, ok ? after : 0);
  });
  connect(m_eventsRestart, &QPushButton::clicked, this, [this] {
    // 显式重读才复位游标（不静默拼「完整事件史」）。
    m_eventsCursor.clear();
    m_eventsExpired = false;
    if (!m_operationId.isEmpty())
      emit eventsRequested(m_operationId, 0);
  });

  auto *portsTitle = new QLabel(tr("结果端口"), this);
  portsTitle->setFont(PaleoTheme::bodyFont(PaleoTheme::kLabelPt));
  layout->addWidget(portsTitle);
  m_portsTable = compactTable({tr("端口"), tr("类型"), tr("单位"), tr("字节"),
                               tr("SHA-256"), tr("诊断"), tr("编码")},
                              this);
  m_portsTable->setObjectName(QStringLiteral("aiOperationDetailPorts"));
  layout->addWidget(m_portsTable);
  auto *portControls = new QHBoxLayout;
  portControls->setSpacing(tokens.spacingXs);
  m_portRead = new QPushButton(tr("读取内容"), this);
  m_portRead->setObjectName(QStringLiteral("aiOperationDetailPortRead"));
  m_portNext = new QPushButton(tr("继续读取"), this);
  m_portNext->setObjectName(QStringLiteral("aiOperationDetailPortNext"));
  m_portRead->setEnabled(false);
  m_portNext->setEnabled(false);
  portControls->addWidget(m_portRead);
  portControls->addWidget(m_portNext);
  portControls->addStretch(1);
  layout->addLayout(portControls);
  connect(m_portsTable, &QTableWidget::cellClicked, this, [this](int row, int) {
    const auto *item = m_portsTable->item(row, 0);
    if (!item)
      return;
    m_selectedPort = item->data(Qt::UserRole + 1).toString();
    m_portEncoding = item->data(Qt::UserRole + 2).toString();
    m_portRead->setEnabled(!m_selectedPort.isEmpty());
    m_portNext->setEnabled(false);
  });
  connect(m_portRead, &QPushButton::clicked, this, [this] {
    if (m_operationId.isEmpty() || m_selectedPort.isEmpty())
      return;
    resetPortView();
    emit portPageRequested(m_operationId, m_selectedPort, 0, kPortPageBytes);
  });
  connect(m_portNext, &QPushButton::clicked, this, [this] {
    if (m_operationId.isEmpty() || m_selectedPort.isEmpty())
      return;
    emit portPageRequested(m_operationId, m_selectedPort, m_portOffset,
                           kPortPageBytes);
  });
  m_portPreview = new QLabel(this);
  m_portPreview->setObjectName(QStringLiteral("aiOperationDetailPortPreview"));
  m_portPreview->setTextFormat(Qt::PlainText);
  m_portPreview->setWordWrap(true);
  m_portPreview->setFont(PaleoTheme::monoFont());
  PaleoTheme::applyThemedStyleSheet(
    m_portPreview, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
  m_portPreview->setMaximumHeight(paleo::ui_detail::lineRows(m_portPreview, 12));
  layout->addWidget(m_portPreview);
  hide();
}

void AiOperationDetailWidget::closeEvent(QCloseEvent *event) {
  emit closed();
  QWidget::closeEvent(event);
}

void AiOperationDetailWidget::openOperation(const QString &operationId) {
  m_operationId = operationId;
  m_eventsCursor.clear();
  m_eventsExpired = false;
  resetPortView();
  m_title->setText(tr("操作 %1").arg(elidedMiddle(operationId, 28)));
  m_title->setToolTip(operationId);
  m_axes->setText(tr("正在读取状态…"));
  m_stateTable->setRowCount(0);
  m_eventsTable->setRowCount(0);
  m_portsTable->setRowCount(0);
  m_eventsNote->clear();
  m_portPreview->clear();
  show();
  // 显式打开 = 显式检视：刷新 + 事件起读 + 端口表。
  emit refreshRequested(operationId);
  emit eventsRequested(operationId, 0);
  emit resultsRequested(operationId);
}

void AiOperationDetailWidget::resetPortView() {
  m_selectedPort.clear();
  m_portEncoding.clear();
  m_portOffset = 0;
  m_portBytes.clear();
  m_portPreview->clear();
  m_portRead->setEnabled(false);
  m_portNext->setEnabled(false);
}

void AiOperationDetailWidget::rebuildStateRows(const QJsonObject &state) {
  m_stateTable->setRowCount(0);
  auto addRow = [this](const QString &key, const QString &value,
                       const QString &tooltip = {}) {
    const int r = m_stateTable->rowCount();
    m_stateTable->insertRow(r);
    auto *k = new QTableWidgetItem(key);
    PaleoTheme::setItemTextColor(k, PaleoTheme::ItemTextColor::Muted);
    m_stateTable->setItem(r, 0, k);
    auto *v = new QTableWidgetItem(value);
    if (!tooltip.isEmpty())
      v->setToolTip(tooltip);
    m_stateTable->setItem(r, 1, v);
  };
  addRow(tr("operationId"), elidedMiddle(m_operationId, 28), m_operationId);
  addRow(tr("执行"), knownOrUnknown(state.value(QStringLiteral("execution")).toString()));
  addRow(tr("提交"), knownOrUnknown(state.value(QStringLiteral("commitState")).toString()));
  addRow(tr("呈现"), knownOrUnknown(state.value(QStringLiteral("presentation")).toString()));
  addRow(tr("修订"), knownOrUnknown(state.value(QStringLiteral("revision")).toString()));
  const auto compute = state.value(QStringLiteral("computeCount"));
  addRow(tr("计算次数"), compute.isDouble() ? QString::number(compute.toDouble())
                                            : tr("未知"));
  addRow(tr("取消请求"), state.value(QStringLiteral("cancelRequested")).toBool()
                            ? tr("已请求取消（不等于已停止）")
                            : tr("未请求"));
  const auto receipt = state.value(QStringLiteral("commitReceiptRef")).toString();
  if (!receipt.isEmpty())
    addRow(tr("提交收据"), elidedMiddle(receipt, 28), receipt);
  const auto code = state.value(QStringLiteral("code")).toString();
  if (!code.isEmpty())
    addRow(tr("code"), code);
}

void AiOperationDetailWidget::onOperationState(const QString &operationId,
                                               const QJsonObject &state) {
  if (operationId != m_operationId || isHidden())
    return; // 迟到帧/目标已切：按代次丢弃
  const QString axes = tr("执行：%1 · 提交：%2 · 呈现：%3")
                         .arg(knownOrUnknown(state.value(QStringLiteral("execution")).toString()),
                              knownOrUnknown(state.value(QStringLiteral("commitState")).toString()),
                              knownOrUnknown(state.value(QStringLiteral("presentation")).toString()));
  m_axes->setText(axes);
  rebuildStateRows(state);
}

void AiOperationDetailWidget::onOperationEvents(const QString &operationId,
                                                const QJsonObject &events) {
  if (operationId != m_operationId || isHidden())
    return;
  const QString code = events.value(QStringLiteral("code")).toString();
  if (events.value(QStringLiteral("ok")).toBool()) {
    m_eventsTable->setRowCount(0);
    const auto rows = events.value(QStringLiteral("events")).toArray();
    for (const auto &value : rows) {
      const auto entry = value.toObject();
      const int r = m_eventsTable->rowCount();
      m_eventsTable->insertRow(r);
      m_eventsTable->setItem(
        r, 0, new QTableWidgetItem(entry.value(QStringLiteral("revision")).toString()));
      auto *body = new QTableWidgetItem(QString::fromUtf8(
        QJsonDocument(entry).toJson(QJsonDocument::Compact)));
      body->setToolTip(body->text());
      m_eventsTable->setItem(r, 1, body);
    }
    m_eventsCursor = events.value(QStringLiteral("nextRevision")).toString();
    const bool more = events.value(QStringLiteral("hasMore")).toBool();
    m_eventsNext->setEnabled(more);
    QStringList notes;
    if (events.value(QStringLiteral("historyTruncated")).toBool())
      notes << tr("事件史曾截断（最早修订 %1）")
                   .arg(events.value(QStringLiteral("firstRevision")).toString());
    notes << (more ? tr("尚有后续事件（下一页自修订 %1 起）").arg(m_eventsCursor)
                   : tr("请求范围已读完（至修订 %1）").arg(m_eventsCursor));
    m_eventsNote->setText(notes.join(QStringLiteral(" · ")));
    return;
  }
  if (code == QLatin1String("event_history_expired")) {
    m_eventsExpired = true;
    m_eventsNote->setText(
      tr("事件史已过期：最早可读修订 %1，此前事件不再可读；"
         "如需从头检视请点「重新起读」")
        .arg(events.value(QStringLiteral("firstRevision")).toString()));
    m_eventsNext->setEnabled(false);
    return;
  }
  m_eventsNote->setText(tr("事件读取失败：%1（详情见表格下方原始 code）").arg(code));
  m_eventsNext->setEnabled(false);
}

void AiOperationDetailWidget::onOperationResults(const QString &operationId,
                                                 const QJsonObject &results) {
  if (operationId != m_operationId || isHidden())
    return;
  m_portsTable->setRowCount(0);
  const auto ports = results.value(QStringLiteral("ports")).toArray();
  for (const auto &value : ports) {
    const auto port = value.toObject();
    const int r = m_portsTable->rowCount();
    m_portsTable->insertRow(r);
    const QString portId = port.value(QStringLiteral("portId")).toString();
    const QStringList cells{
      portId, knownOrUnknown(port.value(QStringLiteral("kind")).toString()),
      knownOrUnknown(port.value(QStringLiteral("unit")).toString()),
      port.value(QStringLiteral("byteSize")).toString(),
      elidedMiddle(port.value(QStringLiteral("sha256")).toString(), 16),
      port.value(QStringLiteral("diagnostic")).toBool() ? tr("诊断") : QString(),
      knownOrUnknown(port.value(QStringLiteral("encoding")).toString())};
    for (int c = 0; c < cells.size(); ++c) {
      auto *item = new QTableWidgetItem(cells.at(c));
      if (c == 0) {
        item->setData(Qt::UserRole + 1, portId);
        item->setData(Qt::UserRole + 2,
                      port.value(QStringLiteral("encoding")).toString());
      }
      m_portsTable->setItem(r, c, item);
    }
  }
  if (ports.isEmpty()) {
    const QString code = results.value(QStringLiteral("code")).toString();
    m_portPreview->setText(
      code == QLatin1String("result_unavailable")
        ? tr("暂无结果端口（%1）").arg(code)
        : tr("结果端口读取失败：%1").arg(knownOrUnknown(code)));
  }
}

void AiOperationDetailWidget::onPortPage(const QString &operationId,
                                         const QString &portId,
                                         const QJsonObject &page) {
  if (operationId != m_operationId || isHidden() || portId != m_selectedPort)
    return; // 前一端口/操作的字节不闪入当前视图
  if (!page.value(QStringLiteral("ok")).toBool()) {
    m_portPreview->setText(tr("读取失败：%1").arg(
      page.value(QStringLiteral("code")).toString()));
    m_portNext->setEnabled(false);
    return;
  }
  const auto bytes = QByteArray::fromBase64(
    page.value(QStringLiteral("data")).toString().toLatin1());
  if (m_portBytes.size() + bytes.size() > kPortPreviewCap) {
    m_portPreview->setText(tr("已到本视图呈现上限（%1 字节）——"
                              "未读取部分不做校验结论")
                             .arg(kPortPreviewCap));
    m_portNext->setEnabled(false);
    return;
  }
  m_portBytes.append(bytes);
  m_portOffset = page.value(QStringLiteral("nextOffset")).toString().toLongLong();
  m_portNext->setEnabled(page.value(QStringLiteral("hasMore")).toBool());
  // 文本预览仅限已知文本 encoding；其余按 hex 有界呈现（不解释内容语义）。
  const bool text = m_portEncoding.contains(QLatin1String("text"), Qt::CaseInsensitive);
  if (text) {
    const auto decoded = paleo::chat::decodeUtf8Display(m_portBytes);
    const QString tail = page.value(QStringLiteral("hasMore")).toBool()
                           ? tr("尚有后续")
                           : tr("读取完成");
    m_portPreview->setText(
      decoded.text + tr("\n（已取回 %1 字节；%2）").arg(m_portBytes.size()).arg(tail));
  } else {
    QStringList hex;
    const int shown = qMin(m_portBytes.size(), 2048);
    for (int i = 0; i < shown; ++i)
      hex << QStringLiteral("%1").arg(quint8(m_portBytes.at(i)), 2, 16, QLatin1Char('0'));
    m_portPreview->setText(hex.join(QLatin1Char(' ')) +
                           (m_portBytes.size() > shown
                              ? tr(" …（hex 仅呈现前 %1 字节，共 %2 字节）")
                                  .arg(shown)
                                  .arg(m_portBytes.size())
                              : tr("（共 %1 字节）").arg(m_portBytes.size())));
  }
}

void AiOperationDetailWidget::onActionFinished(const QString &operationId,
                                               const QString &action,
                                               const QJsonObject &response) {
  if (operationId != m_operationId || isHidden())
    return;
  // 控制结果按服务事实呈现：cancel_after_commit / commit_uncertain 等可解释，
  // 原始 code 保留（W4.4）。已提交事实不被失败/取消抹掉——状态刷新陈述。
  const QString code = response.value(QStringLiteral("code")).toString();
  QString note;
  if (action == QLatin1String("cancel")) {
    if (code == QLatin1String("cancel_after_commit"))
      note = tr("操作已提交，取消请求不影响已提交成果（cancel_after_commit）");
    else if (code == QLatin1String("commit_uncertain"))
      note = tr("提交不确定（commit_uncertain）：提交状态以服务事实为准");
    else if (code == QLatin1String("cancel_requested") ||
             response.value(QStringLiteral("ok")).toBool())
      note = tr("已请求取消（等待执行面确认，不提前宣称已停止）");
    else
      note = tr("取消请求失败：%1").arg(code);
  } else if (action == QLatin1String("resume")) {
    note = response.value(QStringLiteral("ok")).toBool()
             ? tr("恢复已受理（原 operationId 继续执行）")
             : tr("恢复失败：%1").arg(code);
  }
  m_eventsNote->setText(note);
  emit refreshRequested(m_operationId); // 控制后刷新状态（显式动作的一部分）
}

void AiOperationDetailWidget::onBusyChanged(const QString &operationId,
                                            const QString &action, bool busy) {
  if (operationId != m_operationId)
    return;
  setControlBusy(action, busy);
}

void AiOperationDetailWidget::setControlBusy(const QString &action, bool busy) {
  QPushButton *button = action == QLatin1String("refresh") ? m_refresh
                        : action == QLatin1String("cancel") ? m_cancel
                                                            : m_resume;
  if (button)
    button->setEnabled(!busy);
}
