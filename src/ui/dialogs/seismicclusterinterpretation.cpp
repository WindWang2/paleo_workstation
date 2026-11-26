// 层：视图
#include "seismicclusterinterpretation.h"
#include "../../workflow/mappingworkbench.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {
class PatchPager : public QWidget {
public:
  PatchPager(MappingWorkbench *workbench, const QString &layer, QWidget *parent)
      : QWidget(parent), m_workbench(workbench), m_layer(layer) {
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    patch = new QComboBox(this);
    patch->setObjectName(QStringLiteral("clusterManualPatch"));
    m_back = new QPushButton(QObject::tr("上一页"), this);
    m_forward = new QPushButton(QObject::tr("下一页"), this);
    layout->addWidget(patch, 1);
    layout->addWidget(m_back);
    layout->addWidget(m_forward);
    connect(m_back, &QPushButton::clicked, this, [this] {
      m_starts.removeLast();
      load();
    });
    connect(m_forward, &QPushButton::clicked, this, [this] {
      m_starts << m_next;
      load();
    });
  }
  void open(qint64 fid) {
    m_starts = {0};
    if (fid > 0) m_starts << fid;
    load();
    if (fid > 0 && patch->currentData().toMap().value("feature_id").toLongLong() != fid) {
      m_starts = {0}; load();
    }
  }
  QComboBox *patch;
  QString failure;
  std::function<void()> pageChanged;
private:
  void load() {
    failure.clear();
    constexpr int pageSize = 128;
    const auto rows = m_workbench->clusterPatchChoices(m_layer, &failure, m_starts.last(), pageSize + 1);
    const bool more = rows.size() > pageSize;
    m_next = more ? rows.at(pageSize).toMap().value("feature_id").toLongLong() : 0;
    {
      const QSignalBlocker quiet(patch);
      patch->clear();
      for (qsizetype i = 0; i < rows.size() && i < pageSize; ++i) {
        const auto row = rows.at(i).toMap();
        const auto caption = !row.value("patch_id").isNull() && row.value("cluster_id").toInt() > 0
            ? QObject::tr("簇 %1 · 图斑 %2").arg(row.value("cluster_id").toInt()).arg(row.value("patch_id").toLongLong())
            : QObject::tr("合并图斑");
        patch->addItem(QObject::tr("%1 · 要素 %2").arg(caption).arg(row.value("feature_id").toLongLong()), row);
      }
      patch->setCurrentIndex(patch->count() ? 0 : -1);
    }
    m_back->setEnabled(m_starts.size() > 1);
    m_forward->setEnabled(more);
    if (pageChanged) pageChanged();
  }
  MappingWorkbench *m_workbench;
  QString m_layer;
  QList<qint64> m_starts;
  qint64 m_next = 0;
  QPushButton *m_back, *m_forward;
};

QString exclusionLabel(const QString &reason) {
  static const QMap<QString, QString> labels{
    {"outside_grid", QObject::tr("网格外")}, {"nodata", QObject::tr("地震缺测")},
    {"no_coverage", QObject::tr("层段无预测覆盖")}, {"thickness_tie", QObject::tr("优势相厚度相同")},
    {"unresolved_md_range", QObject::tr("井段顶底深度无法核对")},
    {"missing_prediction_or_point", QObject::tr("缺少预测或井点")},
    {"unpredicted", QObject::tr("未预测")}, {"whole_well", QObject::tr("仅有全井预测")}
  };
  return labels.value(reason, reason);
}
QString faciesPath(const QVariantMap &row) {
  QStringList path;
  for (const auto *key : {"facies", "subfacies", "microfacies"}) {
    const auto name = row.value(key).toString();
    if (!name.isEmpty()) path << name;
  }
  return path.isEmpty() ? row.value("name").toString() : path.join(QStringLiteral(" / "));
}

void renderWellPreview(QTreeWidget *tree, const QVariantMap &preview) {
  tree->clear();
  QMap<int, QString> names;
  for (const auto &value : preview.value("facies").toList()) {
    const auto row = value.toMap();
    names.insert(row.value("code").toInt(), faciesPath(row));
  }
  QMap<int, QTreeWidgetItem *> clusters;
  for (const auto &value : preview.value("cluster_facies_mapping").toList()) {
    const auto row = value.toMap();
    const int id = row.value("cluster_id").toInt();
    const QString status = row.value("status").toString();
    QString label = names.value(row.value("facies_code").toInt());
    if (status == "unsupported") label = QObject::tr("无井支持");
    if (status == "conflict") label = QObject::tr("有分歧");
    if (status == "single_well") label += QObject::tr("（单井支持）");
    if (row.value("facies_code").toInt() > 0)
      label += QObject::tr(" · 支持率 %1%").arg(qRound(100 * row.value("support_ratio").toDouble()));
    QStringList counts;
    for (const auto &entry : row.value("votes").toList()) {
      const auto vote = entry.toMap();
      counts << QObject::tr("%1 × %2").arg(names.value(vote.value("facies_code").toInt()))
                    .arg(vote.value("count").toInt());
    }
    clusters[id] = new QTreeWidgetItem(tree, {QObject::tr("簇 %1").arg(id), label, counts.join(QStringLiteral("，"))});
  }
  QTreeWidgetItem *excluded = nullptr;
  for (const auto &value : preview.value("supporting_wells").toList()) {
    const auto row = value.toMap();
    auto *group = clusters.value(row.value("cluster_id").toInt());
    if (!row.value("voted").toBool() || !group) {
      if (!excluded) excluded = new QTreeWidgetItem(tree, {QObject::tr("未计入的井")});
      group = excluded;
    }
    const QString location = row.value("voted").toBool()
        ? QObject::tr("图斑 %1").arg(row.value("patch_id").toLongLong()) : exclusionLabel(row.value("reason").toString());
    auto *item = new QTreeWidgetItem(group, {row.value("well_name", row.value("id")).toString(),
                  names.value(row.value("facies_code").toInt()), location});
    item->setToolTip(0, row.value("version_id").toString());
  }
}
}

QVariantMap SeismicClusterInterpretation::chooseManual(MappingWorkbench *workbench, const QString &id,
    const Highlight &highlight, QWidget *parent, QString *error) {
  const auto version = workbench->versionForLayer(id);
  const auto schema = version.extra.contains("facies") ? version.extra.value("facies").toList()
      : workbench->facies(version.extra.value("horizon").toString());
  if (schema.isEmpty()) { *error = tr("请先为该层位保存微相分类"); return {}; }

  QDialog dialog(parent);
  dialog.setWindowTitle(tr("人工调整微相"));
  auto *form = new QFormLayout(&dialog);
  auto *pager = new PatchPager(workbench, id, &dialog);
  auto *patch = pager->patch;
  auto *whole = new QCheckBox(tr("应用到同簇的所有图斑"), &dialog);
  auto *target = new QComboBox(&dialog);
  target->setObjectName(QStringLiteral("clusterManualFacies"));
  whole->setObjectName(QStringLiteral("clusterManualWhole"));
  auto *details = new QLabel(&dialog);
  details->setWordWrap(true);
  const auto selected = workbench->selectedFeatures(id);
  pager->open(selected.size() == 1 ? selected.first() : 0);
  if (!patch->count()) {
    *error = pager->failure.isEmpty() ? tr("聚类面没有可调整的图斑") : pager->failure;
    return {};
  }
  for (const auto &value : schema) {
    const auto row = value.toMap();
    target->addItem(faciesPath(row), row.value("code"));
  }
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
  target->setPlaceholderText(tr("选择微相"));
  target->setCurrentIndex(-1);
  buttons->button(QDialogButtonBox::Save)->setEnabled(false);
  bool validScope = false;
  QObject::connect(target, &QComboBox::currentIndexChanged, &dialog, [&](int index) {
    buttons->button(QDialogButtonBox::Save)->setEnabled(index >= 0 && validScope);
  });
  auto refresh = [&] {
    const auto row = patch->currentData().toMap();
    const bool hasCluster = row.value("cluster_id").toInt() > 0;
    whole->setEnabled(hasCluster);
    if (!hasCluster) {
      const QSignalBlocker quiet(whole);
      whole->setChecked(false);
    }
    const auto fid = row.value("feature_id").toLongLong();
    QList<qint64> ids{fid};
    QString failure = pager->failure;
    qint64 count = whole->isChecked()
        ? workbench->clusterSelection(id, row.value("cluster_id").toInt(), ids, &failure) : 1;
    validScope = !row.isEmpty() && count > 0 && failure.isEmpty();
    buttons->button(QDialogButtonBox::Save)->setEnabled(validScope && target->currentIndex() >= 0);
    if (!validScope) { highlight({}); details->setText(failure); return; }
    if (count > ids.size() && !ids.isEmpty() && !ids.contains(fid)) {
      ids.last() = fid;
    }
    highlight(ids);
    QString evidence;
    if (!row.value("patch_id").isNull() && !row.value("patch_wells").isNull())
      evidence += tr("原始图斑支持井数为 %1。").arg(row.value("patch_wells").toInt());
    if (hasCluster && !row.value("cluster_wells").isNull())
      evidence += tr("原始簇支持井数为 %1。").arg(row.value("cluster_wells").toInt());
    const auto current = target->findData(row.value("facies_code"));
    details->setText(tr("当前微相为%1。%2将更新 %3 个图斑。")
        .arg(current < 0 ? tr("待定") : target->itemText(current), evidence).arg(count));
    if (count > ids.size()) details->setText(details->text() + tr(" 当前高亮 %1 个图斑。").arg(ids.size()));
  };
  form->addRow(tr("图斑"), pager);
  form->addRow(whole);
  form->addRow(tr("目标微相"), target);
  form->addRow(details);
  form->addRow(buttons);
  QObject::connect(patch, &QComboBox::currentIndexChanged, &dialog, refresh);
  QObject::connect(whole, &QCheckBox::toggled, &dialog, refresh);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  QObject::connect(workbench, &MappingWorkbench::catalogBound, &dialog, &QDialog::reject);
  pager->pageChanged = refresh;
  refresh();
  const bool accepted = dialog.exec() == QDialog::Accepted;
  highlight(selected);
  if (!accepted) return {};
  if (!version.extra.contains("facies") && workbench->facies(version.extra.value("horizon").toString()) != schema) {
    *error = tr("层位微相分类已更改，请重新选择微相");
    return {};
  }
  return {{"patch_id", patch->currentData().toMap().value("patch_id")}, {"feature_id", patch->currentData().toMap().value("feature_id")},
          {"whole_cluster", whole->isChecked()},
          {"facies_code", target->currentData()}};
}

bool SeismicClusterInterpretation::previewWells(MappingWorkbench *workbench, const QString &id,
                                               QWidget *parent, QString *error) {
  const auto sources = workbench->clusterWellSources(id, error);
  if (sources.isEmpty()) {
    if (error->isEmpty()) *error = tr("当前层段尚无可用的井预测，可先人工指定微相");
    return false;
  }
  QDialog dialog(parent);
  dialog.setWindowTitle(tr("用井预测赋相"));
  auto *layout = new QVBoxLayout(&dialog);
  layout->addWidget(new QLabel(tr("参与赋值的井预测版本"), &dialog));
  auto *versions = new QListWidget(&dialog);
  for (const auto &value : sources) {
    const auto row = value.toMap();
    auto *item = new QListWidgetItem(row.value("title").toString(), versions);
    item->setData(Qt::UserRole, row.value("version_id"));
    item->setToolTip(row.value("version_id").toString());
    item->setCheckState(Qt::Checked);
  }
  layout->addWidget(versions);
  auto *preview = new QPushButton(tr("预览井票"), &dialog);
  layout->addWidget(preview);
  auto *tree = new QTreeWidget(&dialog);
  tree->setHeaderLabels({tr("簇 / 井"), tr("建议微相"), tr("井票 / 图斑")});
  tree->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
  layout->addWidget(tree);
  auto *status = new QLabel(&dialog);
  status->setWordWrap(true);
  layout->addWidget(status);
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
  auto *save = buttons->button(QDialogButtonBox::Save);
  save->setEnabled(false);
  layout->addWidget(buttons);
  auto submit = [&] {
    QStringList ids;
    for (int i = 0; i < versions->count(); ++i)
      if (versions->item(i)->checkState() == Qt::Checked) ids << versions->item(i)->data(Qt::UserRole).toString();
    if (ids.isEmpty()) { status->setText(tr("请选择至少一个井预测版本")); return; }
    tree->clear();
    save->setEnabled(false);
    preview->setEnabled(false);
    versions->setEnabled(false);
    status->setText(tr("正在核对井位并统计各簇票数…"));
    QString failure;
    if (!workbench->previewClusterWells(id, ids, &failure)) {
      status->setText(failure); preview->setEnabled(true); versions->setEnabled(true);
    }
  };
  QObject::connect(preview, &QPushButton::clicked, &dialog, submit);
  QObject::connect(versions, &QListWidget::itemChanged, &dialog, [&] {
    tree->clear(); save->setEnabled(false); status->setText(tr("井预测版本已更改，请重新预览"));
  });
  QObject::connect(workbench, &MappingWorkbench::clusterWellPreviewReady, &dialog,
      [&](const QString &layer, const QVariantMap &result) {
    if (layer != id) return;
    renderWellPreview(tree, result);
    const int votes = result.value("voting_wells").toInt();
    save->setEnabled(votes > 0); preview->setEnabled(true); versions->setEnabled(true);
    status->setText(tr("共 %1 口井参与投票。展开簇可查看井和图斑。").arg(votes));
  });
  QObject::connect(workbench, &MappingWorkbench::errorOccurred, &dialog, [&](const QString &failure) {
    status->setText(failure); preview->setEnabled(true); versions->setEnabled(true); save->setEnabled(false);
  });
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  QObject::connect(workbench, &MappingWorkbench::catalogBound, &dialog, &QDialog::reject);
  QPointer<MappingWorkbench> guard(workbench);
  submit();
  if (dialog.exec() == QDialog::Accepted) return true;
  if (guard) guard->cancelClusterWellPreview();
  return false;
}
