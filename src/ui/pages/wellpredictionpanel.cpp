// 层：视图
#include "wellpredictionpanel.h"
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
  m_status = new QLabel(this);
  m_status->setWordWrap(true);
  PaleoTheme::applyThemedStyleSheet(m_status, [] {
    return PaleoTheme::mutedCaptionStyleSheet();
  });
  layout->addWidget(m_status);
  auto *split = new QSplitter(this);
  layout->addWidget(split, 1);
  m_canvas = new WellCompositeCanvas(split);
  m_canvas->setMinimumSize(360, 220);
  auto *right = new QWidget(split);
  right->setAutoFillBackground(true);
  auto *rl = new QVBoxLayout(right);
  rl->setContentsMargins(PaleoTheme::tokens().spacingSm, 0, 0, 0);
  m_intervals = new QTableWidget(0, 3, right);
  m_intervals->setObjectName("predictionIntervals");
  m_intervals->setHorizontalHeaderLabels(
      {tr("顶深 m"), tr("底深 m"), tr("相类别")});
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
  m_apply = new QPushButton(tr("应用到选中井段"), right);
  edit->addWidget(m_apply);
  m_apply->setObjectName("applyWellFacies");
  split->setStretchFactor(0, 3);
  split->setStretchFactor(1, 2);
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
            m_apply->setEnabled(row >= 0 && m_facies->count() > 0);
            if (row >= 0 && m_well->currentIndex() >= 0) {
              auto intervals = m_wells[m_well->currentIndex()]
                                   .toMap()
                                   .value("intervals")
                                   .toList();
              if (row < intervals.size())
                m_facies->setCurrentIndex(m_facies->findData(
                    intervals[row].toMap().value("code").toInt()));
            }
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
  auto *fit = new QPushButton(tr("适应井段"), this);
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
                                    const QVariantList &schema) {
  const auto selected = m_well->currentData();
  const int interval = m_intervals->currentRow();
  m_layer = id;
  m_wells = wells;
  m_schema = schema;
  {
    QSignalBlocker block(m_well);
    m_well->clear();
    for (const auto &v : wells) {
      auto w = v.toMap();
      m_well->addItem(w.value("name").toString(), w.value("id"));
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
  refreshWell();
  if (interval >= 0 && interval < m_intervals->rowCount())
    m_intervals->selectRow(interval);
}
void WellPredictionPanel::setLog(const LasDoc &log) {
  m_log = log;
  rebuildTracks();
}
void WellPredictionPanel::refreshWell() {
  m_intervals->setRowCount(0);
  m_apply->setEnabled(false);
  m_apply->setToolTip(
      tr("选择井段与相类别；首次修订自动保留预测原件并切换到修订副本"));
  m_log = {};
  if (m_well->currentIndex() < 0) {
    m_canvas->clearTracks();
    m_status->setText(tr("选择测井预测结果以查看井道与相井段。"));
    return;
  }
  const auto well = m_wells[m_well->currentIndex()].toMap();
  const auto rows = well.value("intervals").toList();
  m_status->setText(tr("预测为 "
                       "Mock；地图相点按井段累计厚度最大的类别显示并在画布标"
                       "注类别。%1点击井道或表格选择井段，选择相类别后直接应"
                       "用修订；首次修订自动保留预测原件。")
                        .arg(well.value("depth_mock").toBool()
                                 ? tr("当前深度为模拟范围 0–120 m。 ")
                                 : QString()));
  m_intervals->setRowCount(rows.size());
  for (int i = 0; i < rows.size(); ++i) {
    auto r = rows[i].toMap();
    m_intervals->setItem(i, 0,
                         new QTableWidgetItem(QString::number(
                             r.value("top").toDouble(), 'f', 2)));
    m_intervals->setItem(i, 1,
                         new QTableWidgetItem(QString::number(
                             r.value("bottom").toDouble(), 'f', 2)));
    auto f = FaciesCatalog::find(m_schema, r.value("code"));
    m_intervals->setItem(
        i, 2,
        new QTableWidgetItem(
            QIcon(FaciesCatalog::resourcePath(f.value("icon", f.value("texture")).toString())),
            f.value("name").toString()));
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
      for (int j = 0; j < c.values.size(); ++j) {
        curve.depths << m_log.curves.first().values[j];
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
  // 单一预测相道（井道图式样 相|亚|微）：显示当前生效井段；修订直接在预测相
  // 上进行，首次修订由主窗自动保留预测原件并切换到修订副本。
  auto track = std::make_shared<FaciesCompoundTrack>(tr("预测相"), 240);
  QVector<FaciesInterval> items;
  for (const auto &v : well.value("intervals").toList()) {
    auto r = v.toMap();
    auto f = FaciesCatalog::find(m_schema, r.value("code"));
    FaciesInterval item;
    item.topDepth = r.value("top").toDouble();
    item.bottomDepth = r.value("bottom").toDouble();
    item.majorFacies = f.value("facies", f.value("name")).toString();
    item.subFacies = f.value("subfacies").toString();
    item.microFacies = f.value("microfacies").toString();
    if (item.microFacies.isEmpty())
      item.microFacies = f.value("name").toString();
    item.patternType = f.value("texture").toString();
    item.majorColor = item.subColor = item.microColor =
        QColor(f.value("color").toString());
    items << item;
  }
  track->setIntervals(items);
  m_canvas->addTrack(track);
  auto rows = well.value("intervals").toList();
  if (!rows.isEmpty())
    m_canvas->setDepthRange(rows.first().toMap().value("top").toDouble(),
                            rows.last().toMap().value("bottom").toDouble());
}
