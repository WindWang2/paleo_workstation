// 层：视图
#include "aiplanreviewwidget.h"
#include "../paleotheme.h"
#include "../../ai/chat/planpreviewtool.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <cmath>

// ui/ai — H5 计划审阅面板实现。呈现纪律同 G5 详情面：状态词进文案、错误码
// 原样保留、预检通过≠输入真实性/执行权限（三者分开陈述）；模型提议值与
// 宿主核验事实不互相冒充。

namespace {
constexpr int kMaxPointRows = 256; // 有界点表（UI 侧与解码预算同量级）

QTableWidget *compactTable(const QStringList &headers, QWidget *parent) {
  auto *table = new QTableWidget(parent);
  table->setColumnCount(headers.size());
  table->setHorizontalHeaderLabels(headers);
  table->setSelectionBehavior(QAbstractItemView::SelectRows);
  table->setSelectionMode(QAbstractItemView::SingleSelection);
  table->setWordWrap(false);
  table->verticalHeader()->hide();
  table->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
  table->horizontalHeader()->setStretchLastSection(true);
  table->setFont(PaleoTheme::bodyFont(PaleoTheme::kLabelPt));
  return table;
}
QString valueText(const QJsonValue &value) {
  if (value.isDouble()) {
    const double number = value.toDouble();
    return QString::number(number, 'g', 12);
  }
  if (value.isString())
    return value.toString();
  if (value.isBool())
    return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
  return QString::fromUtf8(QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact));
}
} // namespace

AiPlanReviewWidget::AiPlanReviewWidget(QWidget *parent) : QWidget(parent) {
  setObjectName(QStringLiteral("aiPlanReview"));
  auto *layout = new QVBoxLayout(this);
  const auto &tokens = PaleoTheme::tokens();
  layout->setContentsMargins(tokens.spacingXs, tokens.spacingXs, tokens.spacingXs,
                             tokens.spacingXs);
  layout->setSpacing(tokens.spacingXs);

  auto *header = new QHBoxLayout;
  m_title = new QLabel(this);
  m_title->setObjectName(QStringLiteral("aiPlanReviewTitle"));
  m_title->setTextFormat(Qt::PlainText);
  m_title->setFont(PaleoTheme::bodyFont(PaleoTheme::kLabelPt));
  header->addWidget(m_title, 1);
  m_close = new QPushButton(tr("关闭"), this);
  m_close->setObjectName(QStringLiteral("aiPlanReviewClose"));
  header->addWidget(m_close);
  layout->addLayout(header);
  connect(m_close, &QPushButton::clicked, this, [this] {
    hide();
    emit closed();
  });

  m_status = PaleoTheme::capsuleLabel(tr("未打开"), PaleoTheme::CapsuleKind::Neutral, this);
  m_status->setObjectName(QStringLiteral("aiPlanReviewStatus"));
  layout->addWidget(m_status);

  m_steps = new QListWidget(this);
  m_steps->setObjectName(QStringLiteral("aiPlanReviewSteps"));
  m_steps->setFont(PaleoTheme::bodyFont(PaleoTheme::kLabelPt));
  m_steps->setMaximumHeight(m_steps->fontMetrics().lineSpacing() * 5 + 8);
  layout->addWidget(m_steps);
  connect(m_steps, &QListWidget::currentRowChanged, this, [this](int) {
    if (!m_rebuilding)
      rebuildNodeDetail();
  });

  m_nodeFacts = new QLabel(this);
  m_nodeFacts->setObjectName(QStringLiteral("aiPlanReviewNodeFacts"));
  m_nodeFacts->setTextFormat(Qt::PlainText);
  m_nodeFacts->setWordWrap(true);
  m_nodeFacts->setFont(PaleoTheme::bodyFont(PaleoTheme::kLabelPt));
  layout->addWidget(m_nodeFacts);

  m_params = compactTable({tr("参数"), tr("提案值"), tr("当前值"), tr("来源")}, this);
  m_params->setObjectName(QStringLiteral("aiPlanReviewParams"));
  m_params->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed |
                            QAbstractItemView::AnyKeyPressed);
  m_params->setMaximumHeight(m_params->horizontalHeader()->sizeHint().height() +
                             6 * m_params->fontMetrics().lineSpacing() * 3 / 2 +
                             2 * m_params->frameWidth());
  layout->addWidget(m_params);
  connect(m_params, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *) {
    if (!m_rebuilding)
      applyEdits();
  });

  auto *pointsHeader = new QHBoxLayout;
  auto *pointsLabel = new QLabel(tr("IDW 点集（x/y/z 数值，有界编辑）"), this);
  pointsLabel->setFont(PaleoTheme::bodyFont(PaleoTheme::kLabelPt));
  pointsHeader->addWidget(pointsLabel, 1);
  m_addPoint = new QPushButton(tr("加一行"), this);
  m_removePoint = new QPushButton(tr("删选中行"), this);
  pointsHeader->addWidget(m_addPoint);
  pointsHeader->addWidget(m_removePoint);
  layout->addLayout(pointsHeader);
  m_points = compactTable({tr("x"), tr("y"), tr("z"), tr("附加字段(well_id)")}, this);
  m_points->setObjectName(QStringLiteral("aiPlanReviewPoints"));
  m_points->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed |
                            QAbstractItemView::AnyKeyPressed);
  m_points->setMaximumHeight(m_points->horizontalHeader()->sizeHint().height() +
                             6 * m_points->fontMetrics().lineSpacing() * 3 / 2 +
                             2 * m_points->frameWidth());
  layout->addWidget(m_points);
  connect(m_points, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *) {
    if (!m_rebuilding)
      applyEdits();
  });
  connect(m_addPoint, &QPushButton::clicked, this, [this] {
    if (m_points->rowCount() >= kMaxPointRows)
      return;
    m_rebuilding = true;
    const int row = m_points->rowCount();
    m_points->insertRow(row);
    m_points->setItem(row, 0, new QTableWidgetItem(QStringLiteral("0")));
    m_points->setItem(row, 1, new QTableWidgetItem(QStringLiteral("0")));
    m_points->setItem(row, 2, new QTableWidgetItem(QStringLiteral("0")));
    m_points->setItem(row, 3, new QTableWidgetItem);
    m_rebuilding = false;
    applyEdits();
  });
  connect(m_removePoint, &QPushButton::clicked, this, [this] {
    const int row = m_points->currentRow();
    if (row < 0)
      return;
    m_points->removeRow(row);
    applyEdits();
  });

  auto *subintervalRow = new QHBoxLayout;
  auto *subintervalLabel = new QLabel(tr("subinterval_id（必填）"), this);
  subintervalLabel->setFont(PaleoTheme::bodyFont(PaleoTheme::kLabelPt));
  m_subinterval = new QLineEdit(this);
  m_subinterval->setObjectName(QStringLiteral("aiPlanReviewSubinterval"));
  m_subinterval->setMaxLength(64);
  m_subinterval->setFont(PaleoTheme::bodyFont(PaleoTheme::kLabelPt));
  subintervalRow->addWidget(subintervalLabel);
  subintervalRow->addWidget(m_subinterval, 1);
  layout->addLayout(subintervalRow);
  connect(m_subinterval, &QLineEdit::editingFinished, this, [this] {
    if (!m_rebuilding)
      applyEdits();
  });

  m_inputNotes = new QLabel(this);
  m_inputNotes->setObjectName(QStringLiteral("aiPlanReviewInputNotes"));
  m_inputNotes->setTextFormat(Qt::PlainText);
  m_inputNotes->setWordWrap(true);
  PaleoTheme::applyThemedStyleSheet(
    m_inputNotes, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
  layout->addWidget(m_inputNotes);

  auto *edgesTitle = new QLabel(tr("连接与终端成果"), this);
  edgesTitle->setFont(PaleoTheme::bodyFont(PaleoTheme::kLabelPt));
  layout->addWidget(edgesTitle);
  m_edges = compactTable({tr("来源节点.端口"), tr("目标节点.端口"), tr("输入键")}, this);
  m_edges->setObjectName(QStringLiteral("aiPlanReviewEdges"));
  m_edges->setMaximumHeight(m_edges->horizontalHeader()->sizeHint().height() +
                            3 * m_edges->fontMetrics().lineSpacing() * 3 / 2 +
                            2 * m_edges->frameWidth());
  layout->addWidget(m_edges);
  m_terminal = new QLabel(this);
  m_terminal->setObjectName(QStringLiteral("aiPlanReviewTerminal"));
  m_terminal->setTextFormat(Qt::PlainText);
  m_terminal->setFont(PaleoTheme::bodyFont(PaleoTheme::kLabelPt));
  layout->addWidget(m_terminal);

  auto *issuesTitle = new QLabel(tr("预检问题（首个可定位问题；修正后需重新预检）"), this);
  issuesTitle->setFont(PaleoTheme::bodyFont(PaleoTheme::kLabelPt));
  layout->addWidget(issuesTitle);
  m_issues = new QListWidget(this);
  m_issues->setObjectName(QStringLiteral("aiPlanReviewIssues"));
  m_issues->setFont(PaleoTheme::bodyFont(PaleoTheme::kLabelPt));
  m_issues->setMaximumHeight(m_issues->fontMetrics().lineSpacing() * 3 + 8);
  layout->addWidget(m_issues);

  m_operationNote = new QLabel(this);
  m_operationNote->setObjectName(QStringLiteral("aiPlanReviewOperationNote"));
  m_operationNote->setTextFormat(Qt::PlainText);
  m_operationNote->setWordWrap(true);
  m_operationNote->setFont(PaleoTheme::bodyFont(PaleoTheme::kLabelPt));
  layout->addWidget(m_operationNote);

  auto *actions = new QHBoxLayout;
  actions->setSpacing(tokens.spacingXs);
  m_preflight = new QPushButton(tr("重新预检"), this);
  m_preflight->setObjectName(QStringLiteral("aiPlanReviewPreflight"));
  m_compute = new QPushButton(tr("计算私有成果"), this);
  m_compute->setObjectName(QStringLiteral("aiPlanReviewCompute"));
  m_original = new QPushButton(tr("查看原操作"), this);
  m_original->setObjectName(QStringLiteral("aiPlanReviewOriginal"));
  m_operationDetail = new QPushButton(tr("打开操作详情"), this);
  m_operationDetail->setObjectName(QStringLiteral("aiPlanReviewOperationDetail"));
  actions->addWidget(m_preflight);
  actions->addWidget(m_compute);
  actions->addWidget(m_original);
  actions->addWidget(m_operationDetail);
  actions->addStretch(1);
  layout->addLayout(actions);
  connect(m_preflight, &QPushButton::clicked, this, [this] {
    if (!m_presenter || m_draftId.isEmpty())
      return;
    QString reason;
    if (!m_presenter->requestPreflight(m_draftId, &reason) && !reason.isEmpty())
      refreshStatusLine(tr("预检未开始：%1").arg(reason));
  });
  connect(m_compute, &QPushButton::clicked, this, [this] {
    if (!m_presenter || m_draftId.isEmpty())
      return;
    QString reason;
    // 显式用户动作：空 reason + false 只发生在异常路径；拒绝原因常驻状态行。
    if (!m_presenter->requestCompute(m_draftId, &reason) && !reason.isEmpty())
      refreshStatusLine(reason);
  });
  connect(m_original, &QPushButton::clicked, this, [this] {
    if (!m_presenter || m_draftId.isEmpty())
      return;
    QString reason;
    if (!m_presenter->requestOriginalOperation(m_draftId, &reason) && !reason.isEmpty())
      refreshStatusLine(reason);
  });
  connect(m_operationDetail, &QPushButton::clicked, this, [this] {
    const auto view = m_presenter ? m_presenter->draft(m_draftId) : paleo::chat::AiPlanPresentation::DraftView{};
    if (!view.operationId.isEmpty())
      emit operationDetailRequested(view.operationId);
  });

  hide();
}

void AiPlanReviewWidget::attachPresenter(paleo::chat::AiPlanPresentation *presenter) {
  if (m_presenter == presenter)
    return;
  if (m_presenter)
    m_presenter->disconnect(this);
  m_presenter = presenter;
  if (m_presenter)
    handlePresenterSignals();
  refreshActionStates();
}

void AiPlanReviewWidget::handlePresenterSignals() {
  connect(m_presenter, &paleo::chat::AiPlanPresentation::draftChanged, this,
          &AiPlanReviewWidget::onDraftChanged);
  connect(m_presenter, &paleo::chat::AiPlanPresentation::preflightFinished, this,
          &AiPlanReviewWidget::onPreflightFinished);
  connect(m_presenter, &paleo::chat::AiPlanPresentation::computeFinished, this,
          &AiPlanReviewWidget::onComputeFinished);
  connect(m_presenter, &paleo::chat::AiPlanPresentation::originalOperationResolved, this,
          &AiPlanReviewWidget::onOriginalResolved);
  connect(m_presenter, &paleo::chat::AiPlanPresentation::busyChanged, this,
          &AiPlanReviewWidget::onBusyChanged);
  connect(m_presenter, &paleo::chat::AiPlanPresentation::contextChanged, this,
          [this] { refreshActionStates(); refreshStatusLine(); });
}

void AiPlanReviewWidget::openProposal(const QJsonObject &payload, const QString &toolCallId) {
  if (!m_presenter) {
    showRefusal(QStringLiteral("plan_host_or_session_unavailable"),
                tr("计划审阅服务未接入（宿主/会话不可用）"));
    return;
  }
  QString reason;
  const QString draftId = m_presenter->openProposal(payload, toolCallId, &reason);
  if (draftId.isEmpty()) {
    showRefusal(reason, tr("提案无法打开审阅（缺完整图或契约不符）"));
    return;
  }
  m_draftId = draftId;
  rebuild();
  show();
  raise();
}

void AiPlanReviewWidget::showRefusal(const QString &code, const QString &detail) {
  m_draftId.clear();
  m_rebuilding = true;
  m_title->setText(tr("计划审阅（只读）"));
  m_steps->clear();
  m_params->setRowCount(0);
  m_points->setRowCount(0);
  m_edges->setRowCount(0);
  m_issues->clear();
  m_nodeFacts->clear();
  m_inputNotes->setText(detail);
  m_terminal->clear();
  m_operationNote->setText(tr("拒绝码：%1").arg(code));
  m_rebuilding = false;
  refreshActionStates();
  auto *fresh = PaleoTheme::capsuleLabel(tr("无法审阅"), PaleoTheme::CapsuleKind::Error, this);
  layout()->replaceWidget(m_status, fresh);
  m_status->deleteLater();
  m_status = fresh;
  m_status->setObjectName(QStringLiteral("aiPlanReviewStatus"));
  show();
}

void AiPlanReviewWidget::rebuild() {
  const auto view = m_presenter ? m_presenter->draft(m_draftId) : paleo::chat::AiPlanPresentation::DraftView{};
  if (view.draftId != m_draftId) {
    showRefusal(QStringLiteral("plan_draft_unknown"), tr("草案已不存在"));
    return;
  }
  m_rebuilding = true;
  const QString selectedNode = m_steps->currentRow() >= 0 && m_steps->currentItem()
      ? m_steps->currentItem()->data(Qt::UserRole).toString() : QString();
  m_title->setText(tr("计划审阅 — %1（修订 %2）")
                       .arg(view.title.isEmpty() ? tr("未命名计划") : view.title)
                       .arg(view.revision));
  m_steps->clear();
  const auto nodes = view.currentGraph.value(QStringLiteral("nodes")).toArray();
  const auto order = view.lastPayload.value(QStringLiteral("order")).toArray();
  QStringList ordered;
  for (const auto &entry : order)
    ordered.append(entry.toString());
  for (const auto &node : nodes) {
    const auto id = node.toObject().value(QStringLiteral("id")).toString();
    if (!ordered.contains(id))
      ordered.append(id);
  }
  for (const auto &id : ordered) {
    for (const auto &node : nodes) {
      const auto object = node.toObject();
      if (object.value(QStringLiteral("id")).toString() != id)
        continue;
      auto *item = new QListWidgetItem(
        tr("%1 · %2 → %3").arg(id, object.value(QStringLiteral("capability")).toString(),
                               object.value(QStringLiteral("output_port")).toString()));
      item->setData(Qt::UserRole, id);
      m_steps->addItem(item);
      break;
    }
  }
  if (!selectedNode.isEmpty()) {
    for (int i = 0; i < m_steps->count(); ++i)
      if (m_steps->item(i)->data(Qt::UserRole).toString() == selectedNode) {
        m_steps->setCurrentRow(i);
        break;
      }
  }
  if (m_steps->currentRow() < 0 && m_steps->count() > 0)
    m_steps->setCurrentRow(0);

  m_edges->setRowCount(0);
  const auto edges = view.currentGraph.value(QStringLiteral("edges")).toArray();
  for (const auto &entry : edges) {
    const auto edge = entry.toObject();
    const int row = m_edges->rowCount();
    m_edges->insertRow(row);
    m_edges->setItem(row, 0, new QTableWidgetItem(QStringLiteral("%1.%2").arg(
      edge.value(QStringLiteral("from")).toString(),
      edge.value(QStringLiteral("from_port")).toString())));
    m_edges->setItem(row, 1, new QTableWidgetItem(QStringLiteral("%1.%2").arg(
      edge.value(QStringLiteral("to")).toString(),
      edge.value(QStringLiteral("to_port")).toString())));
    m_edges->setItem(row, 2, new QTableWidgetItem(
      edge.value(QStringLiteral("to_port")).toString() == QLatin1String("RASTER")
        ? QStringLiteral("raster_path") : QStringLiteral("—")));
  }
  const auto terminal = view.lastPayload.value(QStringLiteral("terminal")).toString();
  m_terminal->setText(terminal.isEmpty()
      ? tr("终端成果：未知（预检未通过时不猜测）")
      : tr("终端成果：%1（私有计算产物，compute-only；不登记正式成果库/地图）").arg(terminal));

  m_issues->clear();
  const auto issues = view.lastPayload.value(QStringLiteral("issues")).toArray();
  if (view.preflightDone && view.preflightValid) {
    m_issues->addItem(tr("预检通过（结构与类型可用）。注意：这不证明输入真实性、"
                         "物理合理性或执行权限——来源核验与效果门在执行时由宿主裁决。"));
  } else if (issues.isEmpty()) {
    m_issues->addItem(view.preflightDone ? tr("预检未通过（验证器未给出定位字段）")
                                         : tr("尚未预检当前修订"));
  }
  for (const auto &entry : issues) {
    const auto issue = entry.toObject();
    m_issues->addItem(tr("问题：%1%2%3")
                          .arg(issue.value(QStringLiteral("code")).toString(),
                                issue.value(QStringLiteral("field")).toString().isEmpty()
                                    ? QString() : tr("（字段 %1）").arg(
                                          issue.value(QStringLiteral("field")).toString()),
                                issue.value(QStringLiteral("message")).toString().isEmpty()
                                    ? QString() : tr(" — %1").arg(
                                          issue.value(QStringLiteral("message")).toString())));
  }

  rebuildNodeDetail();
  m_rebuilding = false;
  refreshStatusLine();
  refreshActionStates();
}

void AiPlanReviewWidget::rebuildNodeDetail() {
  const auto view = m_presenter ? m_presenter->draft(m_draftId) : paleo::chat::AiPlanPresentation::DraftView{};
  const auto *item = m_steps->currentItem();
  if (!item || view.draftId != m_draftId) {
    m_nodeFacts->clear();
    m_params->setRowCount(0);
    m_points->setRowCount(0);
    return;
  }
  const QString nodeId = item->data(Qt::UserRole).toString();
  QJsonObject node;
  const auto nodes = view.currentGraph.value(QStringLiteral("nodes")).toArray();
  for (const auto &entry : nodes) {
    const auto object = entry.toObject();
    if (object.value(QStringLiteral("id")).toString() == nodeId)
      node = object;
  }
  QJsonObject originNode;
  const auto origins = view.originGraph.value(QStringLiteral("nodes")).toArray();
  for (const auto &entry : origins) {
    const auto object = entry.toObject();
    if (object.value(QStringLiteral("id")).toString() == nodeId)
      originNode = object;
  }
  if (node.isEmpty()) {
    m_nodeFacts->setText(tr("节点 %1 不在当前修订中").arg(nodeId));
    m_params->setRowCount(0);
    m_points->setRowCount(0);
    return;
  }
  const auto effective = view.lastPayload.value(QStringLiteral("effective_parameters"))
                             .toObject().value(nodeId).toObject();
  m_nodeFacts->setText(tr("节点 %1 · %2（契约 v%3）· 输出端口 %4 · 意图 %5")
                           .arg(nodeId,
                                node.value(QStringLiteral("capability")).toString(),
                                QString::number(node.value(QStringLiteral("contract_version")).toInt()),
                                node.value(QStringLiteral("output_port")).toString(),
                                node.value(QStringLiteral("intent")).toString()));

  m_params->setRowCount(0);
  QStringList keys;
  for (auto it = effective.constBegin(); it != effective.constEnd(); ++it)
    keys.append(it.key());
  const auto current = node.value(QStringLiteral("parameters")).toObject();
  const auto proposed = originNode.value(QStringLiteral("parameters")).toObject();
  for (auto it = current.constBegin(); it != current.constEnd(); ++it)
    if (!keys.contains(it.key()))
      keys.append(it.key());
  keys.sort();
  for (const auto &key : keys) {
    const int row = m_params->rowCount();
    m_params->insertRow(row);
    m_params->setItem(row, 0, new QTableWidgetItem(key));
    auto *proposalItem = new QTableWidgetItem(valueText(proposed.value(key)));
    proposalItem->setFlags(proposalItem->flags() & ~Qt::ItemIsEditable);
    m_params->setItem(row, 1, proposalItem);
    m_params->setItem(row, 2, new QTableWidgetItem(valueText(current.value(key))));
    auto *sourceItem = new QTableWidgetItem(paramSourceText(nodeId, key));
    sourceItem->setFlags(sourceItem->flags() & ~Qt::ItemIsEditable);
    m_params->setItem(row, 3, sourceItem);
  }

  // 输入区：IDW 节点给点集/子单元的有界表格；其余只读呈现。
  const bool isIdw = node.value(QStringLiteral("capability")).toString() ==
                     QLatin1String("constraint.idw");
  m_points->setVisible(isIdw);
  m_addPoint->setVisible(isIdw);
  m_removePoint->setVisible(isIdw);
  m_subinterval->setVisible(isIdw);
  m_points->setRowCount(0);
  const auto inputs = node.value(QStringLiteral("inputs")).toObject();
  if (isIdw) {
    m_subinterval->setText(inputs.value(QStringLiteral("subinterval_id")).toString());
    const auto points = inputs.value(QStringLiteral("points")).toArray();
    for (const auto &entry : points) {
      const auto point = entry.toObject();
      const int row = m_points->rowCount();
      m_points->insertRow(row);
      for (int column = 0; column < 3; ++column) {
        const QString key = column == 0 ? QStringLiteral("x")
            : column == 1 ? QStringLiteral("y") : QStringLiteral("z");
        m_points->setItem(row, column, new QTableWidgetItem(valueText(point.value(key))));
      }
      m_points->setItem(row, 3, new QTableWidgetItem(
        point.value(QStringLiteral("well_id")).isUndefined()
          ? QString() : valueText(point.value(QStringLiteral("well_id")))));
    }
    QStringList notes;
    const auto refs = QStringList{QStringLiteral("points_ref"), QStringLiteral("constraints_ref")};
    for (const auto &key : refs)
      if (inputs.contains(key))
        notes.append(tr("%1 = %2（提案值·来源未核验：字符串引用不构成宿主核验的"
                        "Catalogue 版本证明）").arg(key, valueText(inputs.value(key))));
    notes.append(tr("CRS/单位：宿主未提供核验事实，如实标注未知；grid_wkt %1")
                     .arg(inputs.contains(QStringLiteral("grid_wkt"))
                              ? tr("已由提案给出（未核验）") : tr("未提供")));
    m_inputNotes->setText(notes.join(QLatin1Char('\n')));
  } else {
    m_inputNotes->setText(tr("栅格输入由图内上游边供给（raster_path）；本入口"
                             "不支持独立文件源，不提供任意文件打开回退。"));
  }
}

QString AiPlanReviewWidget::paramSourceText(const QString &nodeId, const QString &key) const {
  const auto view = m_presenter ? m_presenter->draft(m_draftId)
                                : paleo::chat::AiPlanPresentation::DraftView{};
  QJsonObject current, proposed, effective;
  for (const auto &entry : view.currentGraph.value(QStringLiteral("nodes")).toArray()) {
    const auto object = entry.toObject();
    if (object.value(QStringLiteral("id")).toString() == nodeId)
      current = object.value(QStringLiteral("parameters")).toObject();
  }
  for (const auto &entry : view.originGraph.value(QStringLiteral("nodes")).toArray()) {
    const auto object = entry.toObject();
    if (object.value(QStringLiteral("id")).toString() == nodeId)
      proposed = object.value(QStringLiteral("parameters")).toObject();
  }
  effective = view.lastPayload.value(QStringLiteral("effective_parameters")).toObject()
                  .value(nodeId).toObject();
  if (!proposed.contains(key) && effective.contains(key))
    return tr("算法默认");
  if (current.value(key) != proposed.value(key))
    return tr("用户修改");
  return tr("提案");
}

QJsonObject AiPlanReviewWidget::buildEditedGraph() const {
  const auto view = m_presenter ? m_presenter->draft(m_draftId)
                                : paleo::chat::AiPlanPresentation::DraftView{};
  QJsonObject graph;
  graph.insert(QStringLiteral("nodes"), view.currentGraph.value(QStringLiteral("nodes")).toArray());
  graph.insert(QStringLiteral("edges"), view.currentGraph.value(QStringLiteral("edges")).toArray());
  const auto *item = m_steps->currentItem();
  if (!item)
    return graph;
  const QString nodeId = item->data(Qt::UserRole).toString();
  auto nodes = graph.value(QStringLiteral("nodes")).toArray();
  for (int i = 0; i < nodes.size(); ++i) {
    auto node = nodes.at(i).toObject();
    if (node.value(QStringLiteral("id")).toString() != nodeId)
      continue;
    // 参数：当前值列 → 按提案/生效值的类型回写（数值有限性在此把关）。
    auto parameters = node.value(QStringLiteral("parameters")).toObject();
    for (int row = 0; row < m_params->rowCount(); ++row) {
      const auto key = m_params->item(row, 0)->text();
      const auto text = m_params->item(row, 2)->text();
      const auto reference = parameters.contains(key)
          ? parameters.value(key)
          : view.lastPayload.value(QStringLiteral("effective_parameters")).toObject()
                .value(nodeId).toObject().value(key);
      if (reference.isDouble()) {
        bool ok = false;
        const double number = text.trimmed().toDouble(&ok);
        if (!ok || !std::isfinite(number))
          return {};
        parameters.insert(key, number);
      } else {
        parameters.insert(key, text);
      }
    }
    node.insert(QStringLiteral("parameters"), parameters);
    // IDW 输入：点表 + subinterval（每行 x/y/z 必须有限数值）。
    if (node.value(QStringLiteral("capability")).toString() == QLatin1String("constraint.idw")) {
      auto inputs = node.value(QStringLiteral("inputs")).toObject();
      QJsonArray points;
      for (int row = 0; row < m_points->rowCount(); ++row) {
        QJsonObject point;
        for (int column = 0; column < 3; ++column) {
          const QString key = column == 0 ? QStringLiteral("x")
              : column == 1 ? QStringLiteral("y") : QStringLiteral("z");
          bool ok = false;
          const double number = m_points->item(row, column)->text().trimmed().toDouble(&ok);
          if (!ok || !std::isfinite(number))
            return {};
          point.insert(key, number);
        }
        const auto extra = m_points->item(row, 3)->text();
        if (!extra.isEmpty())
          point.insert(QStringLiteral("well_id"), extra);
        points.append(point);
      }
      inputs.insert(QStringLiteral("points"), points);
      inputs.insert(QStringLiteral("subinterval_id"), m_subinterval->text().trimmed());
      node.insert(QStringLiteral("inputs"), inputs);
    }
    nodes.replace(i, node);
    break;
  }
  graph.insert(QStringLiteral("nodes"), nodes);
  return graph;
}

void AiPlanReviewWidget::applyEdits() {
  if (!m_presenter || m_draftId.isEmpty())
    return;
  const auto graph = buildEditedGraph();
  if (graph.isEmpty()) {
    refreshStatusLine(tr("修改未应用：数值必须为有限数（x/y/z/数值参数）"));
    return;
  }
  QString reason;
  if (!m_presenter->updateDraftGraph(m_draftId, graph, &reason))
    refreshStatusLine(tr("修改未应用：%1").arg(reason));
  // 成功路径由 draftChanged → rebuild 反馈；失败原因常驻状态行。
}

void AiPlanReviewWidget::refreshStatusLine(const QString &extra) {
  const auto view = m_presenter ? m_presenter->draft(m_draftId)
                                : paleo::chat::AiPlanPresentation::DraftView{};
  QString text;
  PaleoTheme::CapsuleKind kind = PaleoTheme::CapsuleKind::Neutral;
  if (view.draftId != m_draftId) {
    text = tr("未打开");
  } else if (!view.preflightDone) {
    text = tr("待预检（当前修订尚未验证）");
  } else if (!view.preflightValid) {
    kind = PaleoTheme::CapsuleKind::Error;
    text = tr("预检未通过");
  } else if (!view.executionEligible) {
    kind = PaleoTheme::CapsuleKind::Warning;
    text = tr("预检通过·执行资格失效（%1）").arg(
      view.ineligibleReason.isEmpty() ? tr("上下文变化") : view.ineligibleReason);
  } else if (m_presenter && m_presenter->busy(m_draftId, QStringLiteral("compute"))) {
    kind = PaleoTheme::CapsuleKind::Warning;
    text = tr("计算中…");
  } else {
    kind = PaleoTheme::CapsuleKind::Success;
    text = tr("预检通过·尚未执行");
  }
  if (!view.acceptedNote.isEmpty())
    text += QLatin1Char(' ') + view.acceptedNote;
  if (!extra.isEmpty()) {
    text += QLatin1Char(' ') + extra;
    if (kind == PaleoTheme::CapsuleKind::Success)
      kind = PaleoTheme::CapsuleKind::Warning;
  }
  auto *fresh = PaleoTheme::capsuleLabel(text, kind, this);
  layout()->replaceWidget(m_status, fresh);
  m_status->deleteLater();
  m_status = fresh;
  m_status->setObjectName(QStringLiteral("aiPlanReviewStatus"));
}

void AiPlanReviewWidget::refreshActionStates() {
  QString reason;
  const bool allowed = m_presenter && !m_draftId.isEmpty() &&
                       m_presenter->computeAllowed(m_draftId, &reason);
  m_compute->setEnabled(allowed);
  m_preflight->setEnabled(m_presenter && !m_draftId.isEmpty() &&
                          !m_presenter->busy(m_draftId, QStringLiteral("preflight")) &&
                          !m_presenter->busy(m_draftId, QStringLiteral("compute")));
  const auto view = m_presenter ? m_presenter->draft(m_draftId)
                                : paleo::chat::AiPlanPresentation::DraftView{};
  m_original->setEnabled(m_presenter && !m_draftId.isEmpty() &&
                         !view.clientRequestId.isEmpty() &&
                         !m_presenter->busy(m_draftId, QStringLiteral("original")));
  m_operationDetail->setEnabled(!view.operationId.isEmpty());
}

void AiPlanReviewWidget::onDraftChanged(const QString &draftId) {
  if (draftId != m_draftId)
    return; // 其他草案的事实不贴到本面板
  rebuild();
}

void AiPlanReviewWidget::onPreflightFinished(const QString &draftId,
                                             const QJsonObject &payload) {
  if (draftId != m_draftId)
    return;
  Q_UNUSED(payload);
  rebuild();
}

void AiPlanReviewWidget::onComputeFinished(const QString &draftId, const QJsonObject &facts) {
  if (draftId != m_draftId)
    return;
  const auto view = m_presenter->draft(m_draftId);
  QStringList lines;
  lines.append(tr("执行：%1 · 提交：%2 · 呈现：%3").arg(
    view.execution.isEmpty() ? tr("未知") : view.execution,
    view.commitState.isEmpty() ? tr("未知") : view.commitState,
    view.presentation.isEmpty() ? tr("未知") : view.presentation));
  if (!view.operationId.isEmpty())
    lines.append(tr("原操作：%1").arg(view.operationId));
  const auto children = facts.value(QStringLiteral("summary")).toObject()
                             .value(QStringLiteral("child_operations")).toArray();
  if (!children.isEmpty())
    lines.append(tr("子步骤事实（后端返回，非独立发布）：%1 个").arg(children.size()));
  if (!facts.value(QStringLiteral("operationId")).toString().isEmpty() &&
      facts.value(QStringLiteral("code")).toString() != view.execution)
    lines.append(tr("服务码：%1").arg(facts.value(QStringLiteral("code")).toString()));
  lines.append(tr("compute-only 私有计算产物：未登记正式成果库、未改地图。"));
  m_operationNote->setText(lines.join(QLatin1Char('\n')));
  refreshStatusLine();
  refreshActionStates();
}

void AiPlanReviewWidget::onOriginalResolved(const QString &draftId, const QJsonObject &record) {
  if (draftId != m_draftId)
    return;
  if (!record.value(QStringLiteral("ok")).toBool()) {
    m_operationNote->setText(tr("原操作不可恢复：%1（不自动发起新计算冒充恢复）")
                                 .arg(record.value(QStringLiteral("code")).toString()));
  } else {
    m_operationNote->setText(tr("原操作 %1 · 执行 %2 · 提交 %3 · computeCount %4").arg(
      record.value(QStringLiteral("operationId")).toString(),
      record.value(QStringLiteral("execution")).toString(),
      record.value(QStringLiteral("commitState")).toString(),
      record.value(QStringLiteral("computeCount")).toVariant().toString()));
  }
  refreshActionStates();
}

void AiPlanReviewWidget::onBusyChanged(const QString &draftId, const QString &action,
                                       bool busy) {
  if (draftId != m_draftId)
    return;
  Q_UNUSED(action);
  Q_UNUSED(busy);
  refreshStatusLine();
  refreshActionStates();
}

void AiPlanReviewWidget::changeEvent(QEvent *event) {
  QWidget::changeEvent(event);
  if (event->type() == QEvent::ApplicationPaletteChange && !m_draftId.isEmpty())
    rebuild(); // 主题切换：重建 token 依赖样式/字体
}
