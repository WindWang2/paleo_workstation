// 层：视图
#include "validatepage.h"
#include "pageshared.h"
#include "../paleotheme.h"
#include "../../services/previewdoc.h"
#include "../../workflow/workflows.h"
#include "../../domain/types.h"
#include <QCoreApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

using namespace paleo::pagesinternal;

namespace
{
  QString severityText(ValidationIssue::Severity s)
  {
    switch (s)
    {
      case ValidationIssue::Info:    return QObject::tr("信息");
      case ValidationIssue::Error:   return QObject::tr("错误");
      case ValidationIssue::Warning: // fall through
      default:                       return QObject::tr("警告");
    }
  }

  // T27 胶囊化：级别/状态文字走 DESIGN.md status-tag（浅底深字胶囊），
  // 不再用 setForeground 彩色裸文字（对比度 2.3–3.3:1 不达标）。
  // 信息是中性事实（不占语义色）；错误/警告走各自语义 token。
  PaleoTheme::CapsuleKind severityCapsule(ValidationIssue::Severity s)
  {
    switch (s)
    {
      case ValidationIssue::Error: return PaleoTheme::CapsuleKind::Error;
      case ValidationIssue::Warning: return PaleoTheme::CapsuleKind::Warning;
      case ValidationIssue::Info: // fall through
      default: return PaleoTheme::CapsuleKind::Neutral;
    }
  }

  PaleoTheme::CapsuleKind residualCapsule(const QString &status)
  {
    if (status == QLatin1String("pass"))
      return PaleoTheme::CapsuleKind::Success;
    if (status == QLatin1String("exceed") || status == QLatin1String("warn"))
      return PaleoTheme::CapsuleKind::Warning;
    return PaleoTheme::CapsuleKind::Neutral; // 未计算
  }

} // namespace

ValidatePage::ValidatePage(ValidationWorkflow *wf, QWidget *parent)
  : QWidget(parent)
{
  setProperty(kWfProp, QVariant::fromValue(static_cast<QObject *>(wf)));

  auto *lay = panelLayout(this);
  auto *run = new QPushButton(tr("运行验证"), this);
  run->setObjectName(QStringLiteral("runButton"));
  lay->addWidget(run);
  connect(run, &QPushButton::clicked, this, [this, run] {
    // 同步验证无进度回调：运行期间禁用 + 忙碌文案（禁用带 reason，§35），
    // 先让忙碌态上屏再跑（populate 是同步路径）。
    run->setEnabled(false);
    run->setText(tr("正在验证…"));
    run->setToolTip(tr("验证正在运行——完成后自动恢复"));
    QCoreApplication::processEvents();
    populate();
    run->setText(tr("运行验证"));
    run->setToolTip(QString());
    run->setEnabled(true);
  });

  // autoplan §5C：D61 残差表 —— 每口井一行（井名/残差或原因/阈值），
  // 状态字+颜色（通过 #43A047 / 超过阈值 #F29900 / 未计算 #5D6E80）。
  // 计数行在表头；还没跑时面板写「还没有计算 <标定层位> 残差」。
  auto *resSummary =
      new QLabel(tr("还没有计算 %1 残差").arg(PreviewDocService::targetHorizon()), this);
  resSummary->setObjectName(QStringLiteral("residualSummaryLabel"));
  lay->addWidget(resSummary);
  auto *resCap =
      caption(tr("%1 时间残差").arg(PreviewDocService::targetHorizon()), this);
  resCap->setObjectName(QStringLiteral("residualCaption"));
  lay->addWidget(resCap);
  auto *resTable = new QTableWidget(0, 3, this);
  resTable->setObjectName(QStringLiteral("residualTable"));
  resTable->setAccessibleName(tr("%1 残差表").arg(PreviewDocService::targetHorizon()));
  resTable->setHorizontalHeaderLabels({tr("井名"), tr("残差或原因"), tr("阈值")});
  resTable->verticalHeader()->setVisible(false);
  resTable->horizontalHeader()->setStretchLastSection(true);
  lay->addWidget(resTable, 1);
  // T24：残差行双击与问题行同一条 locateRequested——列 0 上挂
  // layerId/WKT/payload（populate 写入），三视图按同一载荷联动。
  connect(resTable, &QTableWidget::itemDoubleClicked, this, [this, resTable](QTableWidgetItem *it) {
    if (!it)
      return;
    auto *first = resTable->item(it->row(), 0);
    if (first)
      emit locateRequested(first->data(Qt::UserRole).toString(),
                           first->data(Qt::UserRole + 1).toString(),
                           first->data(Qt::UserRole + 2).toMap());
  });

  auto *table = new QTableWidget(0, 4, this);
  table->setObjectName(QStringLiteral("issueTable"));
  table->setAccessibleName(tr("验证问题列表"));
  table->setHorizontalHeaderLabels({tr("级别"), tr("代码"), tr("信息"), tr("图层")});
  table->verticalHeader()->setVisible(false);
  table->horizontalHeader()->setStretchLastSection(true);
  lay->addWidget(table, 1);
  // 初始空态：不留白板——指引下一步（populate 重建行时会清掉它）。
  {
    table->insertRow(0);
    auto *it = new QTableWidgetItem(
        tr("还没有运行验证 — 点上方「运行验证」生成问题清单"));
    it->setFlags(Qt::NoItemFlags);
    it->setForeground(PaleoTheme::tokens().textMuted);
    it->setTextAlignment(Qt::AlignCenter);
    table->setItem(0, 0, it);
    table->setSpan(0, 0, 1, table->columnCount());
  }
  connect(table, &QTableWidget::itemDoubleClicked, this, [this, table](QTableWidgetItem *it) {
    if (!it)
      return;
    auto *first = table->item(it->row(), 0); // issue data lives on column 0
    if (first)
      emit locateRequested(first->data(Qt::UserRole).toString(),
                           first->data(Qt::UserRole + 1).toString(),
                           first->data(Qt::UserRole + 2).toMap());
  });

  // 「在数据页看这条剖面」（预览壳重排）：问题行/残差行的载荷里带 inline
  // 测线号才可用；点击把整份 payload 原样发给 shell（切数据页+聚焦该测线）。
  // 两张表共用一颗钮——armed 载荷记在按钮属性上，最近一次选中的表生效。
  auto *openSection = new QPushButton(tr("在数据页看这条剖面"), this);
  openSection->setObjectName(QStringLiteral("openSeismicSectionButton"));
  openSection->setAccessibleName(tr("在数据页看这条剖面"));
  openSection->setEnabled(false);
  openSection->setToolTip(tr("先在问题表或残差表中选一条含剖面位置的行")); // §35 禁用带原因
  lay->addWidget(openSection);
  const auto hasSection = [](const QVariantMap &p) {
    return p.value(QStringLiteral("inline"), -1).toInt() >= 0;
  };
  const auto armSectionFrom = [this, openSection, hasSection](QTableWidget *src) {
    int row = src ? src->currentRow() : -1;
    if (row < 0 && src)
    {
      const QList<QTableWidgetItem *> sel = src->selectedItems();
      if (!sel.isEmpty())
        row = sel.front()->row();
    }
    auto *first = (src && row >= 0) ? src->item(row, 0) : nullptr;
    const QVariantMap p = first ? first->data(Qt::UserRole + 2).toMap() : QVariantMap();
    openSection->setProperty("armedPayload", p);
    const bool armed = hasSection(p);
    openSection->setEnabled(armed);
    // 两表共用一钮：armed 时文案带上目标行标识，不再隐式指向「最近选中」。
    if (armed)
    {
      const int inl = p.value(QStringLiteral("inline"), -1).toInt();
      const QString wellName = p.value(QStringLiteral("well_name")).toString();
      openSection->setText(wellName.isEmpty()
                               ? tr("在数据页看剖面（Inline %1）").arg(inl)
                               : tr("在数据页看剖面（井 %1 · Inline %2）")
                                     .arg(wellName)
                                     .arg(inl));
      openSection->setToolTip(QString());
    }
    else
    {
      openSection->setText(tr("在数据页看这条剖面"));
      openSection->setToolTip(tr("先在问题表或残差表中选一条含剖面位置的行")); // §35 禁用带原因
    }
  };
  connect(table, &QTableWidget::itemSelectionChanged, openSection,
          [armSectionFrom, table]() { armSectionFrom(table); });
  connect(resTable, &QTableWidget::itemSelectionChanged, openSection,
          [armSectionFrom, resTable]() { armSectionFrom(resTable); });
  connect(openSection, &QPushButton::clicked, this,
          [this, openSection, hasSection]() {
            const QVariantMap p = openSection->property("armedPayload").toMap();
            if (hasSection(p))
              emit seismicSectionRequested(p);
          });

  auto *status = new QLabel(this);
  status->setObjectName(QStringLiteral("statusLabel"));
  lay->addWidget(status);
  if (wf)
    connect(wf, &ValidationWorkflow::validationDone, status,
            [status](int n) { status->setText(tr("发现 %1 个问题").arg(n)); });
}

void ValidatePage::populate()
{
  auto *table = child<QTableWidget>(this, "issueTable");
  if (!table)
    return;
  table->setRowCount(0);
  auto *wf = qobject_cast<ValidationWorkflow *>(
      property(kWfProp).value<QObject *>());
  if (!wf)
    return;
  const QList<ValidationIssue> issues = wf->validate();
  for (const ValidationIssue &v : issues)
  {
    const int row = table->rowCount();
    table->insertRow(row);
    auto *sev = new QTableWidgetItem(); // 级别文字进胶囊控件（T27）
    sev->setData(Qt::UserRole, v.layerId);       // locate intent reads these
    sev->setData(Qt::UserRole + 1, v.wktLocation);
    // 三视图联动载荷：wellId/horizon/inline/time_ms（非残差问题不含井字段）。
    QVariantMap payload = v.details;
    if (!v.wellId.isEmpty())
      payload.insert(QStringLiteral("wellId"), v.wellId);
    if (!v.horizon.isEmpty())
      payload.insert(QStringLiteral("horizon"), v.horizon);
    sev->setData(Qt::UserRole + 2, payload);
    sev->setFlags(sev->flags() & ~Qt::ItemIsEditable);
    table->setItem(row, 0, sev);
    table->setCellWidget(row, 0,
                         PaleoTheme::capsuleLabel(severityText(v.severity),
                                                  severityCapsule(v.severity), table));
    auto *code = new QTableWidgetItem(v.code);
    auto *msg = new QTableWidgetItem(v.message);
    auto *layer = new QTableWidgetItem(v.layerId);
    for (auto *it : {code, msg, layer})
      it->setFlags(it->flags() & ~Qt::ItemIsEditable);
    table->setItem(row, 1, code);
    table->setItem(row, 2, msg);
    table->setItem(row, 3, layer);
  }

  // ---- D61 逐井残差表（autoplan §5C）--------------------------------------
  auto *resTable = child<QTableWidget>(this, "residualTable");
  auto *resSummary = child<QLabel>(this, "residualSummaryLabel");
  if (!resTable)
    return;
  resTable->setRowCount(0);
  const QVariantList rows = wf->lastResidualRows();
  if (rows.isEmpty())
  {
    // 空残差表两种含义要分清（T25）：D61 栅格缺失/打不开时工作流会
    // 发 RASTER_MISSING 问题——摘要行复述它的原因，不冒充「没跑过」。
    QString rasterReason;
    for (const ValidationIssue &v : issues)
      if (v.code == QLatin1String("RASTER_MISSING"))
        rasterReason = v.message;
    if (resSummary)
      resSummary->setText(rasterReason.isEmpty()
                              ? tr("还没有计算 %1 残差")
                                    .arg(PreviewDocService::targetHorizon())
                              : rasterReason);
    return;
  }
  fillResidualTable(resTable, rows);
  if (resSummary)
  {
    int nExceed = 0;
    for (const QVariant &v : rows)
      if (v.toMap().value(QStringLiteral("status")).toString() == QLatin1String("exceed"))
        ++nExceed;
    const double thr = rows.first().toMap()
                           .value(QStringLiteral("threshold_ms"), 10.0)
                           .toDouble();
    resSummary->setText(tr("%1 口超过 %2 ms").arg(nExceed).arg(thr, 0, 'f', 0));
  }
}

void ValidatePage::fillResidualTable(QTableWidget *resTable, const QVariantList &rows)
{
  if (!resTable || rows.isEmpty())
    return;
  const double thr = rows.first().toMap()
                         .value(QStringLiteral("threshold_ms"), 10.0)
                         .toDouble();
  for (const QVariant &v : rows)
  {
    const QVariantMap m = v.toMap();
    const QString status = m.value(QStringLiteral("status")).toString();
    const int r = resTable->rowCount();
    resTable->insertRow(r);
    QString word;
    if (status == QLatin1String("pass"))
      word = tr("通过");
    else if (status == QLatin1String("exceed"))
      word = tr("超过阈值");
    else if (status == QLatin1String("warn"))
      word = tr("警告");
    else
      word = tr("未计算"); // 中性胶囊：无栅格/未跑，不占语义色
    const double residualMs = m.value(QStringLiteral("residual_ms")).toDouble();
    auto *name = new QTableWidgetItem(m.value(QStringLiteral("well_name")).toString());
    // T24：残差行与问题行共用三视图联动载荷——列 0 挂 layerId/POINT WKT/
    // payload（wellId/horizon/well_x/well_y/inline/time_ms）。采样点是分层
    // X/Y（缺省井口），缺坐标的行不填 (0,0)（threewaylocator 只认成对字段）。
    name->setData(Qt::UserRole, m.value(QStringLiteral("layer_id")).toString());
    const bool hasXY = m.contains(QStringLiteral("x")) && m.contains(QStringLiteral("y"));
    if (hasXY)
      name->setData(Qt::UserRole + 1,
                    QStringLiteral("POINT(%1 %2)")
                        .arg(m.value(QStringLiteral("x")).toDouble())
                        .arg(m.value(QStringLiteral("y")).toDouble()));
    QVariantMap payload;
    payload.insert(QStringLiteral("wellId"), m.value(QStringLiteral("well_id")).toString());
    payload.insert(QStringLiteral("horizon"), m.value(QStringLiteral("horizon")).toString());
    payload.insert(QStringLiteral("well_name"), m.value(QStringLiteral("well_name")).toString());
    payload.insert(QStringLiteral("inline"), m.value(QStringLiteral("inline"), -1).toInt());
    if (hasXY)
    {
      payload.insert(QStringLiteral("well_x"), m.value(QStringLiteral("x")));
      payload.insert(QStringLiteral("well_y"), m.value(QStringLiteral("y")));
    }
    if (m.contains(QStringLiteral("time_ms")))
      payload.insert(QStringLiteral("time_ms"), m.value(QStringLiteral("time_ms")));
    if (m.contains(QStringLiteral("residual_ms")))
      payload.insert(QStringLiteral("residual_ms"), m.value(QStringLiteral("residual_ms")));
    name->setData(Qt::UserRole + 2, payload);
    auto *val = new QTableWidgetItem(); // 文本进胶囊+mono 值控件（T27/T32）
    // 状态胶囊 + 数值 mono 面：残差数字右对齐等宽（DESIGN.md mono token）。
    auto *cell = new QWidget(resTable);
    auto *hl = new QHBoxLayout(cell);
    hl->setContentsMargins(4, 1, 4, 1);
    hl->setSpacing(4);
    hl->addWidget(PaleoTheme::capsuleLabel(word, residualCapsule(status), cell));
    if (m.contains(QStringLiteral("residual_ms")))
    {
      auto *num = new QLabel(tr("%1 ms").arg(residualMs, 0, 'f', 1), cell);
      num->setFont(PaleoTheme::monoFont());
      num->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
      PaleoTheme::applyThemedStyleSheet(num, [] {
        return QStringLiteral("color: %1;")
            .arg(PaleoTheme::tokens().text.name().toUpper());
      });
      hl->addWidget(num);
    }
    else if (!m.value(QStringLiteral("reason")).toString().isEmpty())
    {
      auto *reason = new QLabel(m.value(QStringLiteral("reason")).toString(), cell);
      reason->setWordWrap(true);
      PaleoTheme::applyThemedStyleSheet(
          reason, [] { return PaleoTheme::mutedCaptionStyleSheet(); }); // text-muted 活体
      hl->addWidget(reason, 1);
    }
    hl->addStretch(1);
    auto *thrItem = new QTableWidgetItem(tr("%1 ms").arg(thr, 0, 'f', 0));
    thrItem->setFont(PaleoTheme::monoFont()); // 阈值列也是数字面
    thrItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    for (auto *it : {name, val, thrItem})
      it->setFlags(it->flags() & ~Qt::ItemIsEditable);
    resTable->setItem(r, 0, name);
    resTable->setItem(r, 1, val);
    resTable->setItem(r, 2, thrItem);
    resTable->setCellWidget(r, 1, cell);
  }
}