// 层：视图
#include "wellpredictionpanel.h"
#include "../windowsizing_internal.h"
#include "../paleotheme.h"
#include "../../domain/faciescatalog.h"
#include "../wellcomposite/wellcompositecanvas.h"
#include <QComboBox>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <cmath>
using namespace WellComposite;

WellPredictionPanel::WellPredictionPanel(QWidget *parent) : QWidget(parent) {
  setObjectName("wellPredictionPanel");
  setAutoFillBackground(true);
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm);
  layout->setSpacing(PaleoTheme::tokens().spacingSm);
  auto *bar = new QHBoxLayout;
  layout->addLayout(bar);
  bar->addWidget(new QLabel(tr("井"), this));
  m_well = new QComboBox(this);
  m_well->setObjectName("predictionWell");
  bar->addWidget(m_well, 1);
  m_undo = new QPushButton(tr("撤销修订"), this);
  bar->addWidget(m_undo);
  m_undo->setObjectName("undoWellPrediction");
  m_save = new QPushButton(tr("保存修订版本"), this);
  bar->addWidget(m_save);
  m_save->setObjectName("saveWellPrediction");
  auto *graph = new QPushButton(tr("资料关联 / 成果追溯"), this);
  graph->setObjectName("wellPredictionGraph"); bar->addWidget(graph);
  connect(graph, &QPushButton::clicked, this, [this] {
    if (!m_layer.isEmpty()) emit graphRequested(m_layer);
  });
  m_status = new QLabel(this);
  m_status->setWordWrap(true);
  PaleoTheme::applyThemedStyleSheet(m_status, [] {
    return PaleoTheme::mutedCaptionStyleSheet();
  });
  layout->addWidget(m_status);
  auto *split = new QSplitter(this);
  layout->addWidget(split, 1);
  m_list = new QTableWidget(0, 3, split);
  m_list->setObjectName("wellFaciesList");
  m_list->setHorizontalHeaderLabels({tr("井"), tr("测井相"), tr("状态")});
  m_list->setAccessibleName(tr("全部井的测井相，下拉直接修改，自动保存到修订副本"));
  m_list->setToolTip(tr("下拉直接修改该井当前地层的测井相；首次修改自动创建修订副本，改动随时自动保存"));
  m_list->setSelectionBehavior(QAbstractItemView::SelectRows);
  m_list->setSelectionMode(QAbstractItemView::SingleSelection);
  m_list->setEditTriggers(QAbstractItemView::NoEditTriggers);
  m_list->verticalHeader()->hide();
  m_list->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
  m_list->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
  m_list->setMinimumWidth(m_list->fontMetrics().averageCharWidth() * 16); // R6 #844：原 240px
  connect(m_list, &QTableWidget::currentCellChanged, this, [this](int row) {
    if (row >= 0 && row < m_well->count() && row != m_well->currentIndex())
      m_well->setCurrentIndex(row);
  });
  m_canvas = new WellCompositeCanvas(split);
  m_canvas->setMinimumSize(240, 160); // R6 #844：原 360×220，窄 dock 放不下
  auto *right = new QWidget(split);
  right->setAutoFillBackground(true);
  auto *rl = new QVBoxLayout(right);
  rl->setContentsMargins(PaleoTheme::tokens().spacingSm, 0, 0, 0);
  m_intervals = new QTableWidget(0, 3, right);
  m_intervals->setObjectName("predictionIntervals");
  m_intervals->setHorizontalHeaderLabels(
      {tr("顶深 m"), tr("底深 m"), tr("预测相")});
  m_intervals->setSelectionBehavior(QAbstractItemView::SelectRows);
  m_intervals->setSelectionMode(QAbstractItemView::SingleSelection);
  m_intervals->setEditTriggers(QAbstractItemView::NoEditTriggers);
  m_intervals->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
  rl->addWidget(m_intervals);
  auto *edit = new QHBoxLayout;
  rl->addLayout(edit);
  m_facies = new QComboBox(right);
  m_facies->setObjectName("wellFaciesChoice");
  edit->addWidget(m_facies, 1);
  m_apply = new QPushButton(tr("应用预测相"), right);
  edit->addWidget(m_apply);
  m_apply->setObjectName("applyWellFacies");
  split->setStretchFactor(0, 2);
  split->setStretchFactor(1, 3);
  split->setStretchFactor(2, 2);
  connect(m_well, &QComboBox::currentIndexChanged, this,
          [this] { refreshWell(); });
  connect(m_save, &QPushButton::clicked, this,
          &WellPredictionPanel::saveRequested);
  connect(m_undo, &QPushButton::clicked, this,
          &WellPredictionPanel::undoRequested);
  connect(m_apply, &QPushButton::clicked, this, [this] {
    if (m_well->currentIndex() >= 0 && m_intervals->currentRow() >= 0)
      emit reviseRequested(m_well->currentData().toString(),
                           m_intervals->currentRow(),
                           m_facies->currentData().toInt());
  });
  connect(m_intervals, &QTableWidget::currentCellChanged, this,
          [this](int row) {
            if (row >= 0 && m_well->currentIndex() >= 0) {
              auto intervals = m_wells[m_well->currentIndex()]
                                   .toMap()
                                   .value("intervals")
                                   .toList();
              if (row < intervals.size())
                m_facies->setCurrentIndex(m_facies->findData(
                    intervals[row].toMap().value("code").toInt()));
            }
            // finding-134：code 不在 schema → m_facies 落无选中（findData=-1）；
            // 应用钮追加 currentIndex 门控，不再以 currentData()=0 发出空修订意图。
            m_apply->setEnabled(row >= 0 && m_facies->count() > 0 &&
                                m_facies->currentIndex() >= 0);
          });
  connect(
      m_canvas, &WellCompositeCanvas::depthClicked, this, [this](double depth) {
        if (m_well->currentIndex() < 0)
          return;
        auto rows =
            m_wells[m_well->currentIndex()].toMap().value("intervals").toList();
        for (int i = 0; i < rows.size(); ++i) {
          auto r = rows[i].toMap();
          if (depth >= r.value("top").toDouble() &&
              depth < r.value("bottom").toDouble()) {
            m_intervals->selectRow(i);
            break;
          }
        }
      });
  auto *fit = new QPushButton(tr("适应当前地层"), this);
  bar->addWidget(fit);
  connect(fit, &QPushButton::clicked, m_canvas,
          &WellCompositeCanvas::resetZoom);
  m_canvas->setScaleRatio(tr("自适应"));
  clear();
}
void WellPredictionPanel::showEvent(QShowEvent *event) {
  QWidget::showEvent(event);
  QTimer::singleShot(0, m_canvas, &WellCompositeCanvas::resetZoom);
}
void WellPredictionPanel::setUndoAvailable(bool available) {
  m_undo->setEnabled(available);
  m_undo->setToolTip(available ? tr("撤销上一次修订") : tr("没有可撤销的修订"));
}
void WellPredictionPanel::clear() { setResult({}, {}, {}); }
void WellPredictionPanel::setResult(const QString &id,
                                    const QVariantList &wells,
                                    const QVariantList &schema, bool mock) {
  const auto selected = m_well->currentData();
  const int interval = m_intervals->currentRow();
  m_layer = id;
  m_mock = mock;
  m_wells = wells;
  m_schema = schema;
  {
    QSignalBlocker block(m_well);
    m_well->clear();
    for (const auto &v : wells) {
      auto w = v.toMap();
      const bool skipped = w.value("prediction_status").toString() == QStringLiteral("未预测");
      m_well->addItem(w.value("name").toString() + (skipped ? tr("（未预测）") : QString()), w.value("id"));
      m_well->setItemData(m_well->count() - 1, w.value("prediction_reason"), Qt::ToolTipRole);
    }
    int index = m_well->findData(selected);
    m_well->setCurrentIndex(index >= 0 ? index : 0);
  }
  m_facies->clear();
  for (const auto &v : schema) {
    auto f = v.toMap();
    m_facies->addItem(
        QIcon(FaciesCatalog::resourcePath(f.value("icon", f.value("texture")).toString())),
        f.value("name").toString(), f.value("code").toInt());
  }
  m_save->setEnabled(id.startsWith("draft."));
  m_save->setToolTip(id.startsWith("draft.")
                         ? tr("保存修订版本并登记图件文件")
                         : tr("首次修订自动保留预测原件并切换到修订副本"));
  setUndoAvailable(false);
  rebuildList();
  refreshWell();
  if (interval >= 0 && interval < m_intervals->rowCount())
    m_intervals->selectRow(interval);
}
void WellPredictionPanel::rebuildList() {
  QSignalBlocker block(m_list);
  m_list->setRowCount(m_wells.size());
  for (int i = 0; i < m_wells.size(); ++i) {
    const auto w = m_wells[i].toMap();
    const bool skipped = w.value("prediction_status").toString() == QStringLiteral("未预测");
    m_list->setItem(i, 0, new QTableWidgetItem(w.value("name").toString()));
    auto *combo = new QComboBox(m_list);
    combo->setObjectName(QStringLiteral("wellFaciesCombo_%1").arg(i));
    combo->setAccessibleName(tr("%1 的测井相").arg(w.value("name").toString()));
    if (skipped)
      combo->addItem(tr("未预测"), QVariant());
    for (const auto &v : m_schema) {
      const auto f = v.toMap();
      combo->addItem(
          QIcon(FaciesCatalog::resourcePath(f.value("icon", f.value("texture")).toString())),
          f.value("name").toString(), f.value("code").toInt());
    }
    const int current = skipped ? 0 : combo->findData(w.value("facies_code").toInt());
    if (!skipped && current < 0)
    {
      // finding-134：facies_code 不在当前 schema 时补一个 data 无效的「未归类」
      // 占位项（同 FaciesCatalog::find 口径）——旧实现回退到首个分类名（静默错值），
      // 与井段表「未归类」不一致；占位项 data 无效，activated 回调据此不发修订。
      combo->addItem(tr("未归类（%1）").arg(w.value("facies_code").toString()), QVariant());
    }
    combo->setCurrentIndex(current >= 0 ? current : combo->count() - 1);
    combo->setEnabled(!m_schema.isEmpty());
    const QString wellId = w.value("id").toString();
    connect(combo, &QComboBox::activated, this, [this, combo, wellId](int index) {
      const QVariant code = combo->itemData(index);
      if (!code.isValid() || code.isNull())
        return;
      emit reviseRequested(wellId, 0, code.toInt());
    });
    m_list->setCellWidget(i, 1, combo);
    // 状态永远配文字（DESIGN.md：语义不单靠颜色）。
    auto *state = new QTableWidgetItem(
        skipped ? tr("未预测")
        : m_layer.startsWith("draft.") ? tr("修订副本")
                                                                       : tr("预测"));
    state->setToolTip(w.value("prediction_reason").toString());
    m_list->setItem(i, 2, state);
  }
  if (m_well->currentIndex() >= 0 && m_well->currentIndex() < m_list->rowCount())
    m_list->selectRow(m_well->currentIndex());
}
void WellPredictionPanel::setLog(const LasDoc &log) {
  m_log = log;
  rebuildTracks();
  if (!log.ok && !log.error.isEmpty())
    m_status->setText(tr("测井曲线暂不可用：%1").arg(log.error));
}
void WellPredictionPanel::refreshWell() {
  m_intervals->setRowCount(0);
  m_apply->setEnabled(false);
  m_apply->setToolTip(
      tr("修订当前井在该地层的预测相；首次修订保留原件并创建副本"));
  m_log = {};
  if (m_well->currentIndex() < 0) {
    m_canvas->clearTracks();
    m_status->setText(tr("选择测井预测结果以查看当前地层的预测相。"));
    return;
  }
  if (m_list->currentRow() != m_well->currentIndex()) {
    QSignalBlocker block(m_list);
    m_list->selectRow(m_well->currentIndex());
  }
  const auto well = m_wells[m_well->currentIndex()].toMap();
  const auto rows = well.value("intervals").toList();
  m_status->setText(tr("%1 · %2 · 预测相：%3。每井每地层一条结果。")
      .arg(well.value("name").toString(), well.value("formation").toString(), well.value("facies_label").toString()));
  if (well.value("prediction_status").toString() == QStringLiteral("未预测"))
    m_status->setText(tr("%1：未预测（预测相为 NULL）。原因：%2")
        .arg(well.value("name").toString(), well.value("prediction_reason").toString()));
  m_intervals->setRowCount(rows.size());
  for (int i = 0; i < rows.size(); ++i) {
    auto r = rows[i].toMap();
    m_intervals->setItem(i, 0,
                         new QTableWidgetItem(r.value("top").isNull() ? tr("未提供") : QString::number(r.value("top").toDouble(), 'f', 2)));
    m_intervals->setItem(i, 1,
                         new QTableWidgetItem(r.value("bottom").isNull() ? tr("未提供") : QString::number(r.value("bottom").toDouble(), 'f', 2)));
    auto f = FaciesCatalog::find(m_schema, r.value("code"));
    m_intervals->setItem(
        i, 2,
        new QTableWidgetItem(
            QIcon(FaciesCatalog::resourcePath(f.value("icon", f.value("texture")).toString())),
            r.value("label", f.value("name")).toString()));
  }
  rebuildTracks();
  m_intervals->selectRow(0);
  emit featureSelected(well.value("feature_id").toLongLong());
  emit logRequested(well.value("log_version_id").toString());
}
void WellPredictionPanel::rebuildTracks() {
  m_canvas->clearTracks();
  if (m_well->currentIndex() < 0)
    return;
  m_canvas->addTrack(std::make_shared<DepthScaleTrack>());
  if (m_log.ok && m_log.curves.size() > 1) {
    auto track = std::make_shared<CurveTrack>();
    for (int i = 1; i < qMin(4, m_log.curves.size()); ++i) {
      const auto &c = m_log.curves[i];
      CurveData curve;
      curve.name = c.name;
      curve.unit = c.unit;
      double low = INFINITY, high = -INFINITY;
      const auto scope = m_wells[m_well->currentIndex()].toMap();
      double top = 0, base = 0;
      FaciesCatalog::formationRange(scope.value("formation_top"), scope.value("formation_base"), &top, &base);
      for (int j = 0; j < qMin(c.values.size(), m_log.curves.first().values.size()); ++j) {
        const double depth = m_log.curves.first().values[j];
        if (!std::isfinite(top) || !std::isfinite(base) || depth < top || depth >= base) continue;
        curve.depths << depth;
        curve.values << c.values[j];
        if (std::isfinite(c.values[j])) {
          low = std::min(low, c.values[j]);
          high = std::max(high, c.values[j]);
        }
      }
      if (std::isfinite(low)) {
        curve.minScale = low;
        curve.maxScale = high > low ? high : low + 1;
        track->addCurve(curve);
      }
    }
    m_canvas->addTrack(track);
  }
  const auto well = m_wells[m_well->currentIndex()].toMap();
  auto track = std::make_shared<TextTrack>(tr("预测相"), 180);
  track->setKeepTextVisible(true);
  const auto topValue = well.value("formation_top"), baseValue = well.value("formation_base");
  const double top = topValue.toDouble(), base = baseValue.toDouble();
  if (!topValue.isNull() && !baseValue.isNull() && std::isfinite(top) && std::isfinite(base) && base > top) {
    TextInterval item;
    item.topDepth = float(top); item.bottomDepth = float(base);
    item.text = well.value("facies_label").toString();
    auto f = FaciesCatalog::appearance(FaciesCatalog::find(m_schema, well.value("facies_code")));
    item.bgColor = f.contains("color") ? QColor(f.value("color").toString())
                                        : PaleoTheme::tokens().surfaceAlt;
    track->setIntervals({item});
    m_canvas->setDepthRange(top, base);
  }
  m_canvas->addTrack(track);
  m_canvas->resetZoom();
}
