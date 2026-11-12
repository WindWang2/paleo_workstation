// 层：视图
#include "mappingtaskguide.h"

#include "mappingworkbenchpage.h"
#include "../paleotheme.h"
#include "../../workflow/mappingworkbench.h"

#include <QAbstractItemModel>
#include <QComboBox>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QStyle>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {

QPushButton *stepButton(QWidget *parent, const QString &id, const QString &title, const QString &tip) {
  auto *button = new QPushButton(title, parent);
  button->setObjectName(QStringLiteral("mappingTaskStep.") + id);
  button->setToolTip(tip);
  button->setFlat(true);
  button->setCursor(Qt::PointingHandCursor);
  button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  PaleoTheme::applyThemedStyleSheet(button, [] {
    const auto &t = PaleoTheme::tokens();
    return PaleoTheme::metricStyleSheet(QStringLiteral(
               "QPushButton { color: %1; background: %2; border: 1px solid %3; "
               "border-radius: {rounded.sm}px; padding: {spacing.xs}px {spacing.sm}px; "
               "font-size: {typography.label}pt; font-weight: 600; }"
               "QPushButton[taskDone=\"true\"] { color: %6; background: %7; border-color: %7; }"
               "QPushButton[taskBlocked=\"true\"] { color: %8; background: %9; border-color: %9; }"
               "QPushButton[taskCurrent=\"true\"] { color: %4; background: %5; border-color: %5; }"))
        .arg(t.textMuted.name(), t.surface.name(), t.border.name(), t.onPrimary.name(),
             t.primary.name(), t.successText.name(), t.successBg.name(), t.warningText.name(),
             t.warningBg.name());
  });
  return button;
}

void polish(QWidget *widget) {
  widget->style()->unpolish(widget);
  widget->style()->polish(widget);
  widget->update();
}

QStringList checkedInputIds(QListWidget *inputs) {
  QStringList ids;
  if (!inputs)
    return ids;
  for (int i = 0; i < inputs->count(); ++i)
    if (inputs->item(i)->checkState() == Qt::Checked)
      ids << inputs->item(i)->data(Qt::UserRole).toString();
  return ids;
}

bool evidenceDrift(QWidget *page, const QVariantMap &evidence) {
  if (evidence.isEmpty())
    return false;
  auto *text = page->findChild<QPlainTextEdit *>(QStringLiteral("editBasisText"));
  auto *reason = page->findChild<QLineEdit *>(QStringLiteral("editBasisReason"));
  QStringList versions;
  if (auto *sources = page->findChild<QListWidget *>(QStringLiteral("editBasisSources"))) {
    for (int i = 0; i < sources->count(); ++i)
      versions << sources->item(i)->data(Qt::UserRole).toString();
  }
  return !text || !reason || text->toPlainText().trimmed() != evidence.value("text").toString() ||
         reason->text().trimmed() != evidence.value("reason").toString() ||
         versions != evidence.value("source_versions").toStringList();
}

bool rowInHorizon(const QVariantMap &row, const QString &horizon) {
  const auto hz = row.value("horizon").toString();
  return hz.isEmpty() || hz == horizon;
}

int horizonProductCount(QTreeWidget *tree, const QString &horizon) {
  if (!tree)
    return 0;
  int count = 0;
  for (int i = 0; i < tree->topLevelItemCount(); ++i) {
    const auto row = tree->topLevelItem(i)->data(0, Qt::UserRole).toMap();
    if (!rowInHorizon(row, horizon))
      continue;
    const auto id = row.value("id").toString();
    if (id.startsWith(QLatin1String("product.")) || id.startsWith(QLatin1String("draft.")))
      ++count;
  }
  return count;
}

} // namespace

MappingTaskGuide::MappingTaskGuide(const QString &mode, MappingWorkbench *workbench, QWidget *page,
                                   QWidget *parent)
    : QWidget(parent), m_mode(mode), m_workbench(workbench), m_page(page) {
  setObjectName(QStringLiteral("mappingTaskGuide"));
  setAccessibleName(tr("任务向导"));
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm,
                              PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm);
  layout->setSpacing(PaleoTheme::tokens().spacingXs);
  PaleoTheme::applyThemedStyleSheet(this, [] {
    const auto &t = PaleoTheme::tokens();
    return PaleoTheme::metricStyleSheet(QStringLiteral(
               "QWidget#mappingTaskGuide { background: %1; border: 1px solid %2; "
               "border-radius: {rounded.sm}px; }"))
        .arg(t.surfaceAlt.name(), t.border.name());
  });
  auto *steps = new QWidget(this);
  steps->setObjectName(QStringLiteral("mappingTaskStepper"));
  steps->setAccessibleName(m_mode == QLatin1String("compose") ? tr("编图步骤") : tr("预测步骤"));
  auto *row = new QHBoxLayout(steps);
  row->setContentsMargins(0, 0, 0, 0);
  row->setSpacing(PaleoTheme::tokens().spacingXs);
  layout->addWidget(steps);
  m_hint = new QLabel(this);
  m_hint->setObjectName(QStringLiteral("mappingTaskHint"));
  m_hint->setWordWrap(true);
  m_hint->setTextFormat(Qt::PlainText);
  m_hint->setFont(PaleoTheme::bodyFont(PaleoTheme::kBodyPt));
  PaleoTheme::applyThemedStyleSheet(m_hint, [] {
    return QStringLiteral("QLabel { color: %1; }").arg(PaleoTheme::tokens().text.name());
  });
  layout->addWidget(m_hint);
  m_next = new QPushButton(this);
  m_next->setObjectName(QStringLiteral("mappingTaskNext"));
  layout->addWidget(m_next);
  connect(m_next, &QPushButton::clicked, this, [this] {
    focusAnchor(m_next->property("target").toString());
  });
  buildSteps();
}

void MappingTaskGuide::buildSteps() {
  auto *steps = findChild<QWidget *>(QStringLiteral("mappingTaskStepper"));
  auto *row = steps ? qobject_cast<QHBoxLayout *>(steps->layout()) : nullptr;
  if (!row)
    return;
  const auto add = [this, row](const QString &id, const QString &title, const QString &anchor,
                               const QString &tip) {
    if (row->count() > 0) {
      auto *arrow = new QLabel(QStringLiteral("→"), row->parentWidget());
      arrow->setFont(PaleoTheme::bodyFont(PaleoTheme::kLabelPt));
      PaleoTheme::applyThemedStyleSheet(arrow, [] {
        return QStringLiteral("QLabel { color: %1; }").arg(PaleoTheme::tokens().textMuted.name());
      });
      row->addWidget(arrow);
    }
    auto *button = stepButton(this, id, title, tip);
    button->setProperty("anchor", anchor);
    row->addWidget(button);
    connect(button, &QPushButton::clicked, this, [this, anchor] { focusAnchor(anchor); });
  };
  if (m_mode == QLatin1String("compose")) {
    add(QStringLiteral("basemap"), tr("1 基础图"), QStringLiteral("mappingBaseMapSelector"),
        tr("选择已有图件，或新建空白相图"));
    add(QStringLiteral("evidence"), tr("2 依据"), QStringLiteral("editBasisText"),
        tr("填写解释说明和修改目的后启用。分割相面、覆盖绘制和合并都要先完成这一步"));
    add(QStringLiteral("edit"), tr("3 相编辑"), QStringLiteral("workbenchResults"),
        tr("先在画布选中相面，再分割、覆盖绘制或合并"));
    add(QStringLiteral("deliver"), tr("4 保存"), QStringLiteral("workbench.checkTopology"),
        tr("检查三级相面拓扑，保存新版本后再导出"));
  } else {
    add(QStringLiteral("prepare"), tr("1 准备"), QStringLiteral("predictionKind"),
        tr("选择预测类型，并勾选井或地震体"));
    add(QStringLiteral("run"), tr("2 运行"), QStringLiteral("workbench.predict"),
        tr("按当前层位运行预测"));
    add(QStringLiteral("analyze"), tr("3 分析"), QStringLiteral("workbenchResults"),
        tr("把图件显示到画布，对照井道或一致性"));
  }
}

void MappingTaskGuide::refresh() {
  if (!m_page || !m_workbench)
    return;
  if (m_mode == QLatin1String("compose"))
    refreshCompose();
  else
    refreshPredict();
}

void MappingTaskGuide::apply(int stage, const QString &hint, const QString &nextText,
                             const QString &anchor, bool nextEnabled) {
  setProperty("stage", stage);
  m_hint->setText(hint);
  m_next->setText(nextEnabled ? nextText : tr("请先完成上一步"));
  m_next->setEnabled(nextEnabled);
  m_next->setProperty("target", nextEnabled ? anchor : QString());
  m_next->setToolTip(nextEnabled ? tr("滚到这一步的控件") : hint);
}

void MappingTaskGuide::markStep(const QString &id, bool done, bool current, bool blocked,
                                const QString &tip) {
  auto *button = findChild<QPushButton *>(QStringLiteral("mappingTaskStep.") + id);
  if (!button)
    return;
  button->setProperty("taskDone", done && !current);
  button->setProperty("taskCurrent", current);
  button->setProperty("taskBlocked", blocked && !current);
  if (!tip.isEmpty())
    button->setToolTip(tip);
  polish(button);
}

void MappingTaskGuide::focusAnchor(const QString &name) {
  if (name.isEmpty() || !m_page)
    return;
  auto *target = m_page->findChild<QWidget *>(name);
  if (!target)
    return;
  if (auto *scroll = m_page->findChild<QScrollArea *>())
    scroll->ensureWidgetVisible(target, 12, 48);
  if (target->focusPolicy() != Qt::NoFocus)
    target->setFocus(Qt::OtherFocusReason);
}

void MappingTaskGuide::refreshPredict() {
  auto *page = qobject_cast<MappingWorkbenchPage *>(m_page);
  const bool horizon = page && !page->horizon().isEmpty();
  auto *inputs = m_page->findChild<QListWidget *>(QStringLiteral("workbenchInputs"));
  auto *kind = m_page->findChild<QComboBox *>(QStringLiteral("predictionKind"));
  const auto checked = checkedInputIds(inputs);
  const bool busy = m_workbench->busy();
  const int results = horizonProductCount(
      m_page->findChild<QTreeWidget *>(QStringLiteral("workbenchResults")),
      page ? page->horizon() : QString());
  const bool ran = results > 0 && !busy;
  const bool prepared = horizon && (ran || !checked.isEmpty());
  const int stage = !horizon ? 0 : busy || (prepared && !ran) ? 1 : ran ? 2 : 0;
  const QString kindId = kind ? kind->currentData().toString() : QString();
  QString hint;
  QString nextText;
  QString anchor;
  bool enabled = true;
  if (!horizon) {
    hint = tr("请先在画布上方选择层位，再准备井或地震输入。");
    enabled = false;
  } else if (busy) {
    hint = tr("预测进行中，可以取消。完成后图件会出现在分析和画布上。");
    nextText = tr("看进度");
    anchor = QStringLiteral("workbenchPredictionProgress");
  } else if (results > 0 && page && !page->selectedLayer().isEmpty()) {
    hint = tr("已选中图件。显示到画布后，测井结果可打开井道修订，地震相可做一致性对照。");
    nextText = tr("显示图件");
    anchor = QStringLiteral("workbench.show");
  } else if (results > 0) {
    hint = tr("本层已有图件。选中后显示到画布，测井结果可打开井道，地震相可做一致性对照。要重跑，先勾选输入再运行。");
    nextText = tr("去分析");
    anchor = QStringLiteral("workbenchResults");
  } else if (!inputs || inputs->count() == 0) {
    hint = tr("暂无可用输入。请先在数据管理导入并关联地震体或测井，再回来勾选。");
    enabled = false;
  } else if (checked.isEmpty()) {
    hint = kindId == QLatin1String("wells")
               ? tr("勾选本层位的井。每口井得到当前地层的预测微相，分析时可打开井道。")
           : kindId == QLatin1String("seismic_mock")
               ? tr("勾选一个地震体。Mock 会沿层位窗聚类，结果需要地质复核。")
               : tr("勾选一个地震体，按当前层位调用预测服务。");
    nextText = tr("去勾选");
    anchor = QStringLiteral("workbenchInputs");
  } else {
    hint = tr("输入已齐。运行后到「分析」：显示图件、对照井道，或看地震相和测井相是否一致。");
    nextText = tr("去运行");
    anchor = QStringLiteral("workbench.predict");
  }
  markStep(QStringLiteral("prepare"), prepared, stage == 0, false, QString());
  markStep(QStringLiteral("run"), ran, stage == 1, false, QString());
  markStep(QStringLiteral("analyze"), false, stage == 2, false, QString());
  apply(stage, hint, nextText, anchor, enabled);
}

void MappingTaskGuide::refreshCompose() {
  auto *page = qobject_cast<MappingWorkbenchPage *>(m_page);
  const QString horizon = page ? page->horizon() : QString();
  const auto evidence = m_workbench->editEvidence(horizon);
  const bool armed = !evidence.isEmpty();
  const bool drift = evidenceDrift(m_page, evidence);
  bool workCopy = false;
  bool pendingSave = false;
  if (auto *tree = m_page->findChild<QTreeWidget *>(QStringLiteral("workbenchResults"))) {
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
      const auto row = tree->topLevelItem(i)->data(0, Qt::UserRole).toMap();
      if (!rowInHorizon(row, horizon))
        continue;
      const auto id = row.value("id").toString();
      const bool copy = row.value("draft").toBool() && id.startsWith(QLatin1String("draft."));
      workCopy = workCopy || copy;
      if (copy && m_workbench->hasUnpublishedEdits(id))
        pendingSave = true;
    }
  }
  const bool evidenceReady = armed && !drift;
  const int stage = horizon.isEmpty() || !workCopy ? 0 : !evidenceReady ? 1 : pendingSave ? 3 : 2;
  const QString blockedTip =
      tr("分割相面、覆盖绘制和合并都要先启用解释依据，否则这些编辑无法完成");
  QString hint;
  QString nextText;
  QString anchor;
  bool enabled = true;
  if (horizon.isEmpty()) {
    hint = tr("请先在画布上方选择层位。");
    enabled = false;
  } else if (!workCopy && !armed) {
    hint = tr("先在基础图里选择已有图件，或新建空白相图。建好后必须启用解释依据，分割相面才能完成。");
    nextText = tr("去选基础图");
    anchor = QStringLiteral("mappingBaseMapSelector");
  } else if (!workCopy) {
    hint = tr("解释依据已启用。建立相图之后就可以绘制，并用分割切开相面。");
    nextText = tr("去建图");
    anchor = QStringLiteral("baseMapStart");
  } else if (!armed) {
    hint = tr("还不能完成相面编辑。分割、覆盖绘制和合并都要先启用解释依据，否则改动落不成。");
    nextText = tr("去启用依据");
    anchor = QStringLiteral("editBasisText");
  } else if (drift) {
    hint = tr("依据表单已改，还没重新启用。现在分割仍按上一次的依据记录，新说明不会写上。");
    nextText = tr("重新启用");
    anchor = QStringLiteral("workbench.setEditEvidence");
  } else if (pendingSave) {
    hint = tr("相面已改。先检查拓扑，再保存新版本。导出要等保存之后。");
    nextText = tr("去检查");
    anchor = QStringLiteral("workbench.checkTopology");
  } else {
    hint = tr("依据已启用，可以分割相面：先在画布选中相面，再画一条穿过该面的分割线。新面继承原来的三级归属。");
    nextText = tr("去编相面");
    anchor = QStringLiteral("workbenchResults");
  }
  markStep(QStringLiteral("basemap"), workCopy, stage == 0, false, QString());
  markStep(QStringLiteral("evidence"), evidenceReady, stage == 1, false,
           stage == 1 ? hint : tr("启用后，随后的分割和改类会自动记下这些依据"));
  const bool editBlocked = workCopy && !armed;
  markStep(QStringLiteral("edit"), pendingSave, stage == 2, editBlocked,
           editBlocked ? blockedTip
           : armed     ? tr("依据已启用。先选中相面，再画分割线；新面继承原来的三级归属")
                       : tr("先完成基础图和解释依据，再分割相面"));
  markStep(QStringLiteral("deliver"), false, stage == 3, false, QString());
  apply(stage, hint, nextText, anchor, enabled);
}

void MappingWorkbenchPage::installTaskGuide(QVBoxLayout *outer) {
  if (m_heading) {
    QFont title = m_heading->font();
    title.setPointSize(PaleoTheme::tokens().titlePt);
    title.setBold(true);
    m_heading->setFont(title);
    m_heading->setTextFormat(Qt::PlainText);
  }
  if (m_mode != QLatin1String("predict") && m_mode != QLatin1String("compose"))
    return;
  auto *guide = new MappingTaskGuide(m_mode, m_workbench, this, this);
  outer->insertWidget(0, guide);
  outer->setSpacing(PaleoTheme::tokens().spacingXs);
  connect(m_workbench, &MappingWorkbench::editEvidenceChanged, guide, &MappingTaskGuide::refresh);
  connect(m_workbench, &MappingWorkbench::predictionBusyChanged, guide,
          [guide](bool) { guide->refresh(); });
  if (auto *inputs = findChild<QListWidget *>(QStringLiteral("workbenchInputs")))
    connect(inputs, &QListWidget::itemChanged, guide, [guide](QListWidgetItem *) { guide->refresh(); });
  if (auto *kind = findChild<QComboBox *>(QStringLiteral("predictionKind")))
    connect(kind, qOverload<int>(&QComboBox::currentIndexChanged), guide,
            [guide](int) { guide->refresh(); });
  if (auto *results = findChild<QTreeWidget *>(QStringLiteral("workbenchResults")))
    connect(results, &QTreeWidget::currentItemChanged, guide,
            [guide](QTreeWidgetItem *, QTreeWidgetItem *) { guide->refresh(); });
  if (auto *text = findChild<QPlainTextEdit *>(QStringLiteral("editBasisText")))
    connect(text, &QPlainTextEdit::textChanged, guide, &MappingTaskGuide::refresh);
  if (auto *reason = findChild<QLineEdit *>(QStringLiteral("editBasisReason")))
    connect(reason, &QLineEdit::textChanged, guide, [guide](const QString &) { guide->refresh(); });
  if (auto *sources = findChild<QListWidget *>(QStringLiteral("editBasisSources"))) {
    connect(sources->model(), &QAbstractItemModel::rowsInserted, guide,
            [guide](const QModelIndex &, int, int) { guide->refresh(); });
    connect(sources->model(), &QAbstractItemModel::rowsRemoved, guide,
            [guide](const QModelIndex &, int, int) { guide->refresh(); });
    connect(sources->model(), &QAbstractItemModel::dataChanged, guide,
            [guide] { guide->refresh(); });
    connect(sources->model(), &QAbstractItemModel::modelReset, guide,
            &MappingTaskGuide::refresh);
  }
  if (m_mode == QLatin1String("predict")) {
    const auto labels = findChildren<QLabel *>();
    for (auto *label : labels) {
      if (label == m_heading || !label->text().contains(QStringLiteral("结果待复核")))
        continue;
      label->setToolTip(label->text());
      label->setText(tr("按上面的步骤准备输入、运行，再对照图件分析。"));
      break;
    }
  }
}
