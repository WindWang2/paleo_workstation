// 层：视图
#include "aitoolresultview.h"

#include "../paleotheme.h"
#include "../pages/derivationgraph.h"
#include "../../services/derivationgraph.h"

#include <QAbstractItemView>
#include <QClipboard>
#include <QEvent>
#include <QFrame>
#include <QGuiApplication>
#include <QHash>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>
#include <QPushButton>
#include <QSet>
#include <QTableWidgetItem>
#include <QTextBrowser>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

// ui/ai — AI 工具结果结构化视图实现（方向 93）。
//
// 纪律：色/字体/spacing 一律 PaleoTheme::tokens()（本文件不写色值字面量，
// check_ui_invariants 门禁）；全部用户可见文案走 tr()（check_i18n 门禁）；
// 组件零写路径——表格只读、血缘图只读，导航/复制是外发意图。

namespace {
QString kindLabel(const QString &type) {
  if (type == QLatin1String("well"))
    return QObject::tr("井");
  if (type == QLatin1String("planned"))
    return QObject::tr("计划井");
  return type; // 目录类型 token 原样（不编造中文映射）
}

paleo::derivation::Kind kindFromKey(const QString &key) {
  if (key == QLatin1String("raw"))
    return paleo::derivation::Kind::Raw;
  if (key == QLatin1String("derived"))
    return paleo::derivation::Kind::Derived;
  return paleo::derivation::Kind::External;
}

QString cellText(const QJsonObject &entry, const char *key) {
  const QJsonValue value = entry.value(QLatin1String(key));
  if (value.isDouble())
    return QString::number(value.toDouble());
  return value.toString();
}
} // namespace

AiToolResultViewKind aiToolResultViewKind(const QString &tool, bool ok,
                                          const QJsonObject &payload) {
  if (!ok)
    return AiToolResultViewKind::Fallback; // 失败态：错误原文 + JSON 折叠
  if (tool == QLatin1String("paleo.asset_lineage")) {
    const QJsonArray nodes = payload.value(QStringLiteral("nodes")).toArray();
    if (nodes.isEmpty() || nodes.size() > kAiLineageMiniNodeCap)
      return AiToolResultViewKind::Fallback; // 空闭包/超限：快照图不可读
    return AiToolResultViewKind::LineageGraph;
  }
  if (tool == QLatin1String("paleo.query_project")) {
    const QString topic = payload.value(QStringLiteral("topic")).toString();
    if (topic == QLatin1String("wells") || topic == QLatin1String("horizons") ||
        topic == QLatin1String("assets"))
      return AiToolResultViewKind::Table;
    if (topic == QLatin1String("summary") ||
        topic == QLatin1String("well_details"))
      return AiToolResultViewKind::KeyValue;
  }
  return AiToolResultViewKind::Fallback; // 未知工具/topic：现状折叠
}

AiToolResultView::AiToolResultView(const QString &tool, bool ok,
                                   const QString &resultJson, QWidget *parent)
  : QWidget(parent) {
  const QJsonDocument document =
    QJsonDocument::fromJson(resultJson.toUtf8());
  const QJsonObject payload = document.object();
  m_kind = aiToolResultViewKind(tool, ok, payload);
  switch (m_kind) {
  case AiToolResultViewKind::Table:
    buildTable(payload, payload.value(QStringLiteral("topic")).toString());
    break;
  case AiToolResultViewKind::KeyValue:
    if (payload.value(QStringLiteral("topic")).toString() ==
        QLatin1String("well_details"))
      buildKeyValueWithLinks(payload);
    else
      buildKeyValue(payload);
    break;
  case AiToolResultViewKind::LineageGraph:
    buildLineageGraph(payload);
    break;
  case AiToolResultViewKind::Fallback:
    buildFallback(payload);
    break;
  }
}

QTableWidget *AiToolResultView::makeCompactTable(int columns,
                                                 const QStringList &headers) {
  auto *table = new QTableWidget(this);
  table->setColumnCount(columns);
  table->setHorizontalHeaderLabels(headers);
  table->setEditTriggers(QAbstractItemView::NoEditTriggers); // 只读红线
  table->setSelectionBehavior(QAbstractItemView::SelectRows);
  table->setSelectionMode(QAbstractItemView::SingleSelection);
  table->setAlternatingRowColors(true);
  table->setWordWrap(false);
  table->verticalHeader()->hide();
  table->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
  table->horizontalHeader()->setStretchLastSection(true);
  table->horizontalHeader()->setSectionResizeMode(
    0, QHeaderView::ResizeToContents);
  table->setFont(PaleoTheme::bodyFont(PaleoTheme::kLabelPt));
  table->setSizeAdjustPolicy(QAbstractItemView::AdjustToContents);
  // dock 卡内可读性（DESIGN.md「紧凑表格」）：高度封顶 ≈ 六行 + 表头，
  // 更长走内部滚动，不把卡片区顶穿。
  table->setMaximumHeight(192);
  return table;
}

void AiToolResultView::buildTable(const QJsonObject &payload,
                                  const QString &topic) {
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(PaleoTheme::tokens().spacingXs);

  // 列/行导航语义按 topic 定：wells 行是井实体（entityNavigate），
  // horizons/assets 行是资产（assetNavigate）——id 如实取自出参字段。
  const bool wellRows = topic == QLatin1String("wells");
  QStringList headers;
  if (wellRows)
    headers << tr("井名") << tr("类型") << tr("井深(m)");
  else if (topic == QLatin1String("horizons"))
    headers << tr("层位") << tr("版本") << tr("阶段");
  else
    headers << tr("名称") << tr("类型") << tr("版本") << tr("阶段");
  QTableWidget *table = makeCompactTable(headers.size(), headers);
  table->setObjectName(QStringLiteral("aiResultTable"));

  const QJsonArray rows = payload.value(wellRows ? QStringLiteral("wells")
                                                 : topic == QLatin1String("horizons")
                                                     ? QStringLiteral("horizons")
                                                     : QStringLiteral("assets"))
                            .toArray();
  table->setRowCount(int(rows.size()));
  for (int i = 0; i < rows.size(); ++i) {
    const QJsonObject entry = rows.at(i).toObject();
    const QString id = entry.value(wellRows ? QStringLiteral("id")
                                            : topic == QLatin1String("horizons")
                                                ? QStringLiteral("asset_id")
                                                : QStringLiteral("id"))
                         .toString();
    QStringList cells;
    if (wellRows) {
      cells << cellText(entry, "name") << kindLabel(cellText(entry, "type"))
            << cellText(entry, "td");
    } else if (topic == QLatin1String("horizons")) {
      // 版本列 = 版本号（insertVersionCitation 只产 version_number/stage）。
      cells << cellText(entry, "name") << cellText(entry, "version_number")
            << cellText(entry, "stage");
    } else {
      cells << cellText(entry, "name") << cellText(entry, "type")
            << cellText(entry, "version_number") << cellText(entry, "stage");
    }
    for (int c = 0; c < cells.size(); ++c) {
      auto *item = new QTableWidgetItem(cells.at(c));
      item->setData(Qt::UserRole, id); // 行导航锚（空 = 出参无引用，不跳）
      item->setToolTip(cells.at(c));
      table->setItem(i, c, item);
    }
  }
  layout->addWidget(table, 1);

  auto *footnote = new QLabel(this);
  footnote->setFont(PaleoTheme::bodyFont(PaleoTheme::kLabelPt));
  footnote->setWordWrap(true);
  PaleoTheme::applyThemedStyleSheet(
    footnote, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
  QStringList notes;
  const QString truncated =
    payload.value(QStringLiteral("truncated")).toString();
  if (!truncated.isEmpty())
    notes << truncated;
  notes << tr("共 %1 条").arg(payload.value(QStringLiteral("count")).toInt());
  footnote->setText(notes.join(QStringLiteral(" · ")));
  layout->addWidget(footnote);

  auto *actions = new QHBoxLayout;
  actions->setSpacing(PaleoTheme::tokens().spacingXs);
  actions->addStretch(1);
  auto *copy = new QPushButton(tr("复制表格"), this);
  copy->setObjectName(QStringLiteral("aiResultCopyTable"));
  actions->addWidget(copy);
  layout->addLayout(actions);

  // 复制 = TSV（表头 + 行）：粘贴进表格软件即用；井深等数值保持原文。
  connect(copy, &QPushButton::clicked, this, [this, table, headers] {
    QStringList lines;
    lines << headers.join(QLatin1Char('\t'));
    for (int r = 0; r < table->rowCount(); ++r) {
      QStringList cells;
      for (int c = 0; c < table->columnCount(); ++c)
        cells << (table->item(r, c) ? table->item(r, c)->text() : QString());
      lines << cells.join(QLatin1Char('\t'));
    }
    if (QClipboard *clipboard = QGuiApplication::clipboard())
      clipboard->setText(lines.join(QLatin1Char('\n')));
  });
  // 行点击 = 导航意图（不是数据修改）：空 id 不发（出参没有引用就诚实无跳转）。
  connect(table, &QTableWidget::cellClicked, this,
          [this, table, wellRows](int row, int) {
            const QTableWidgetItem *item = table->item(row, 0);
            if (!item)
              return;
            const QString id = item->data(Qt::UserRole).toString();
            if (id.isEmpty())
              return;
            if (wellRows)
              emit entityNavigateRequested(id);
            else
              emit assetNavigateRequested(id);
          });
}

void AiToolResultView::buildKeyValue(const QJsonObject &payload) {
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(PaleoTheme::tokens().spacingXs);
  // 计数键值对：两列只读表（无表头）——实体/资产计数 + catalog 修订号。
  QTableWidget *table = makeCompactTable(2, {QString(), QString()});
  table->setObjectName(QStringLiteral("aiResultKeyValue"));
  table->horizontalHeader()->hide();
  auto addRow = [table](const QString &key, const QString &value) {
    const int r = table->rowCount();
    table->insertRow(r);
    auto *k = new QTableWidgetItem(key);
    PaleoTheme::setItemTextColor(k, PaleoTheme::ItemTextColor::Muted);
    table->setItem(r, 0, k);
    table->setItem(r, 1, new QTableWidgetItem(value));
  };
  const QJsonObject entities =
    payload.value(QStringLiteral("entities")).toObject();
  for (auto it = entities.begin(); it != entities.end(); ++it)
    addRow(tr("实体 · %1").arg(it.key()), QString::number(it.value().toInt()));
  const QJsonObject assetsByType =
    payload.value(QStringLiteral("assets_by_type")).toObject();
  for (auto it = assetsByType.begin(); it != assetsByType.end(); ++it)
    addRow(tr("资产 · %1").arg(it.key()), QString::number(it.value().toInt()));
  addRow(tr("catalog 修订"), QString::number(
                               payload.value(QStringLiteral("revision")).toInt()));
  layout->addWidget(table);
}

void AiToolResultView::buildKeyValueWithLinks(const QJsonObject &payload) {
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(PaleoTheme::tokens().spacingXs);
  // 井头键值对（两列只读表，无表头）+ 关联资产表（行点击 → 资产定位）。
  QTableWidget *head = makeCompactTable(2, {QString(), QString()});
  head->setObjectName(QStringLiteral("aiResultKeyValue"));
  head->horizontalHeader()->hide();
  auto addRow = [head](const QString &key, const QString &value) {
    const int r = head->rowCount();
    head->insertRow(r);
    auto *k = new QTableWidgetItem(key);
    PaleoTheme::setItemTextColor(k, PaleoTheme::ItemTextColor::Muted);
    head->setItem(r, 0, k);
    head->setItem(r, 1, new QTableWidgetItem(value));
  };
  const QJsonArray links = payload.value(QStringLiteral("links")).toArray();
  addRow(tr("井名"), payload.value(QStringLiteral("well")).toString());
  addRow(tr("井 ID"), payload.value(QStringLiteral("well_id")).toString());
  addRow(tr("关联资产"), QString::number(links.size()));
  const QString truncated =
    payload.value(QStringLiteral("truncated")).toString();
  if (!truncated.isEmpty())
    addRow(tr("截断"), truncated);
  layout->addWidget(head);

  QTableWidget *table =
    makeCompactTable(5, {tr("角色"), tr("资产"), tr("主用"), tr("版本"), tr("阶段")});
  table->setObjectName(QStringLiteral("aiResultTable"));
  table->setRowCount(int(links.size()));
  for (int i = 0; i < links.size(); ++i) {
    const QJsonObject entry = links.at(i).toObject();
    const QStringList cells = {
      cellText(entry, "role"), cellText(entry, "asset"),
      entry.value(QStringLiteral("primary")).toBool() ? tr("是") : QString(),
      cellText(entry, "version_number"), cellText(entry, "stage")};
    for (int c = 0; c < cells.size(); ++c) {
      auto *item = new QTableWidgetItem(cells.at(c));
      item->setData(Qt::UserRole, cellText(entry, "asset_id"));
      item->setToolTip(cells.at(c));
      table->setItem(i, c, item);
    }
  }
  layout->addWidget(table, 1);
  connect(table, &QTableWidget::cellClicked, this,
          [this, table](int row, int) {
            const QTableWidgetItem *item = table->item(row, 0);
            if (!item)
              return;
            const QString id = item->data(Qt::UserRole).toString();
            if (!id.isEmpty())
              emit assetNavigateRequested(id);
          });
}

void AiToolResultView::buildLineageGraph(const QJsonObject &payload) {
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(PaleoTheme::tokens().spacingXs);

  paleo::derivation::Graph graph;
  graph.available = payload.value(QStringLiteral("available")).toBool(true);
  const QJsonArray nodeArray = payload.value(QStringLiteral("nodes")).toArray();
  QHash<QString, QString> assetByVersion; // 节点点击导航用
  for (const QJsonValue &value : nodeArray) {
    const QJsonObject entry = value.toObject();
    paleo::derivation::Node node;
    node.versionId = cellText(entry, "version_id");
    node.assetId = cellText(entry, "asset_id");
    node.versionName = cellText(entry, "version_name");
    node.assetName = cellText(entry, "asset");
    node.stage = cellText(entry, "stage");
    node.kind = kindFromKey(cellText(entry, "kind"));
    node.stale = entry.value(QStringLiteral("stale")).toBool();
    node.column = entry.value(QStringLiteral("column")).toInt();
    graph.nodes.append(node);
    assetByVersion.insert(node.versionId, node.assetId);
  }
  for (const QJsonValue &value : payload.value(QStringLiteral("edges")).toArray()) {
    const QJsonObject entry = value.toObject();
    graph.edges.append({cellText(entry, "parent"), cellText(entry, "child")});
  }
  const QString seedVersion =
    payload.value(QStringLiteral("version_id")).toString();
  const QString seedAsset =
    payload.value(QStringLiteral("asset_id")).toString();
  const QJsonArray closureIds =
    payload.value(QStringLiteral("selection"))
      .toObject()
      .value(QStringLiteral("version_ids"))
      .toArray();
  QSet<QString> closure;
  for (const QJsonValue &id : closureIds)
    closure.insert(id.toString());

  auto *graphView = new DerivationGraph(this);
  graphView->setObjectName(QStringLiteral("aiResultLineageGraph"));
  // 卡内快照高度（渲染核缺省 260 起）：两行节点 + 边的可读快照，更大
  // 交互（缩放/过滤）走「在血缘页打开」。
  graphView->setFixedHeight(240);
  graphView->loadGraph(graph);
  graphView->highlight(seedVersion, closure);
  // 结果可能早在 dock 隐藏时到达：立即 fit 一次（布局完备的场合直接对），
  // 首秀再补一次（见 eventFilter）；此后保留用户缩放。
  QTimer::singleShot(0, graphView, &DerivationGraph::fitGraph);
  m_fitPendingGraph = graphView;
  graphView->installEventFilter(this);
  layout->addWidget(graphView, 1);

  auto *footnote = new QLabel(this);
  footnote->setFont(PaleoTheme::bodyFont(PaleoTheme::kLabelPt));
  footnote->setWordWrap(true);
  PaleoTheme::applyThemedStyleSheet(
    footnote, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
  footnote->setText(
    tr("闭包 %1 版本 · 图上 %2 节点 · 已折叠上游 %3 / 下游 %4")
      .arg(payload.value(QStringLiteral("selection"))
             .toObject()
             .value(QStringLiteral("count"))
             .toInt())
      .arg(graph.nodes.size())
      .arg(payload.value(QStringLiteral("collapsed_upstream")).toInt())
      .arg(payload.value(QStringLiteral("collapsed_downstream")).toInt()));
  layout->addWidget(footnote);

  auto *actions = new QHBoxLayout;
  actions->setSpacing(PaleoTheme::tokens().spacingXs);
  actions->addStretch(1);
  auto *open = new QPushButton(tr("在血缘页打开"), this);
  open->setObjectName(QStringLiteral("aiResultOpenLineage"));
  actions->addWidget(open);
  layout->addLayout(actions);

  // 跳数据页血缘区（全功能交互）：种子资产 + 版本上下文（导航不是修改）。
  connect(open, &QPushButton::clicked, this,
          [this, seedAsset, seedVersion] {
            emit lineageNavigateRequested(seedAsset, seedVersion);
          });
  // 小图节点点击同样走版本定位（与数据页 nodeClicked 同语义）。
  connect(graphView, &DerivationGraph::nodeClicked, this,
          [this, assetByVersion](const QString &versionId) {
            emit lineageNavigateRequested(assetByVersion.value(versionId),
                                          versionId);
          });
}

bool AiToolResultView::eventFilter(QObject *watched, QEvent *event) {
  if (watched == m_fitPendingGraph &&
      (event->type() == QEvent::Show || event->type() == QEvent::Expose)) {
    // 首秀补一次适配后卸载过滤器：之后 fitInView 不再自动发生（用户缩放
    // 属于用户，视图不抢）。
    auto *graph = m_fitPendingGraph;
    m_fitPendingGraph = nullptr;
    graph->removeEventFilter(this);
    graph->fitGraph();
  }
  return QWidget::eventFilter(watched, event);
}

void AiToolResultView::buildFallback(const QJsonObject &payload) {
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(PaleoTheme::tokens().spacingXs);

  // 血缘超限降级时注记如实（节点数在 payload 里，可复核）。
  const QJsonArray nodes = payload.value(QStringLiteral("nodes")).toArray();
  if (payload.value(QStringLiteral("tool")).toString() ==
        QLatin1String("paleo.asset_lineage") &&
      nodes.size() > kAiLineageMiniNodeCap) {
    auto *note = new QLabel(
      tr("闭包 %1 节点超过小图上限 %2，已按 JSON 折叠——「在血缘页打开」"
         "前的完整事实都在下方出参里")
        .arg(nodes.size())
        .arg(kAiLineageMiniNodeCap),
      this);
    note->setFont(PaleoTheme::bodyFont(PaleoTheme::kLabelPt));
    note->setWordWrap(true);
    PaleoTheme::applyThemedStyleSheet(
      note, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
    layout->addWidget(note);
  }

  // 兜底 = 现状 JSON 折叠块：默认收起，展开给 pretty 全文（mono，只读）。
  auto *toggle = new QToolButton(this);
  toggle->setObjectName(QStringLiteral("aiResultJsonToggle"));
  toggle->setText(tr("完整 JSON"));
  toggle->setCheckable(true);
  toggle->setToolButtonStyle(Qt::ToolButtonTextOnly);
  toggle->setArrowType(Qt::RightArrow);
  layout->addWidget(toggle);

  auto *json = new QTextBrowser(this);
  json->setObjectName(QStringLiteral("aiResultJsonBody"));
  json->setFont(PaleoTheme::monoFont());
  json->setOpenExternalLinks(false);
  // 显示上限（呈现层防御）：工具出参本身有界（列表 50/关联 100/血缘 80
  // 节点），这里再挡一道未来工具的意外大出参——截尾如实标注原始长度。
  constexpr int kFallbackCharCap = 8000;
  QString text =
    QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Indented));
  if (text.size() > kFallbackCharCap)
    text = text.left(kFallbackCharCap) +
           tr("\n…（显示前 %1 字符，原始 %2 字符；全文已回传模型）")
             .arg(kFallbackCharCap)
             .arg(text.size());
  json->setText(text);
  json->setMaximumHeight(180);
  json->hide();
  layout->addWidget(json);
  connect(toggle, &QToolButton::toggled, this, [toggle, json](bool on) {
    json->setVisible(on);
    toggle->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
  });
}
