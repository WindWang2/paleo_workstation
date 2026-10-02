// 层：视图
#include "crossplotpanel.h"
#include "ui/paleotheme.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
namespace paleo::crossplot {
CrossplotCanvas::CrossplotCanvas(QWidget *parent) : QWidget(parent) {
  setObjectName(QStringLiteral("crossplotCanvas"));
  setMinimumSize(320, 200);
  setMouseTracking(true);
  setFocusPolicy(Qt::StrongFocus);
  setAccessibleName(tr("散点交会图，拖动套索，Shift 拖动框选，单击定位"));
}
QRectF CrossplotCanvas::plotRect() const {
  return QRectF(56, 24, std::max(1, width() - 80), std::max(1, height() - 64));
}
QPointF CrossplotCanvas::normalized(QPointF p) const {
  const auto r = plotRect();
  return {(p.x() - r.left()) / r.width(), (r.bottom() - p.y()) / r.height()};
}
void CrossplotCanvas::densityImage() {
  m_density = {};
  if (m_frame.density.isEmpty())
    return;
  m_density =
      QImage(m_frame.densitySide, m_frame.densitySide, QImage::Format_ARGB32);
  m_density.fill(Qt::transparent);
  const auto ink = PaleoTheme::tokens().textMuted;
  for (int y = 0; y < m_frame.densitySide; ++y)
    for (int x = 0; x < m_frame.densitySide; ++x) {
      const int count = m_frame.density[y * m_frame.densitySide + x];
      if (!count)
        continue;
      QColor color = ink;
      if (!m_frame.densityClass.isEmpty() &&
          m_frame.densityClass[y * m_frame.densitySide + x] >= 0) {
        const auto c =
            classColor(m_frame.densityClass[y * m_frame.densitySide + x]);
        color = QColor(c.red, c.green, c.blue);
      }
      color.setAlpha(
          int(40 + 215 * std::log1p(count) / std::log1p(m_frame.densityMax)));
      m_density.setPixelColor(x, m_frame.densitySide - 1 - y, color);
    }
}
void CrossplotCanvas::setFrame(const PlotFrame &f) {
  m_frame = f;
  m_selected.clear();
  m_gesture.clear();
  densityImage();
  update();
}
void CrossplotCanvas::setSelection(const QVector<int> &indices) {
  m_selected = indices;
  update();
}
void CrossplotCanvas::changeEvent(QEvent *e) {
  if (e->type() == QEvent::ApplicationPaletteChange) {
    densityImage();
    update();
  }
  QWidget::changeEvent(e);
}
void CrossplotCanvas::paintEvent(QPaintEvent *) {
  QPainter p(this);
  const auto &t = PaleoTheme::tokens();
  p.fillRect(rect(), t.surface);
  const auto r = plotRect();
  p.setPen(t.border);
  p.drawRect(r);
  p.setFont(PaleoTheme::bodyFont());
  p.setPen(t.text);
  p.drawText(QRectF(r.left(), r.bottom() + 20, r.width(), 20), Qt::AlignCenter,
             m_frame.xTitle);
  p.save();
  p.translate(16, r.center().y());
  p.rotate(-90);
  p.drawText(QRectF(-r.height() / 2, -10, r.height(), 20), Qt::AlignCenter,
             m_frame.yTitle);
  p.restore();
  p.setFont(PaleoTheme::monoFont());
  p.setPen(t.textMuted);
  if (!m_frame.is3d) {
    p.drawText(QPointF(r.left(), r.bottom() + 16),
               QString::number(m_frame.xMin, 'g', 5));
    p.drawText(QPointF(r.right() - 70, r.bottom() + 16),
               QString::number(m_frame.xMax, 'g', 5));
    p.drawText(QPointF(r.left() + 4, r.top() + 14),
               QString::number(m_frame.yMax, 'g', 5));
  } else
    p.drawText(QPointF(r.left() + 4, r.top() + 14),
               tr("三维投影 · Z：%1").arg(m_frame.zTitle));
  p.save();
  p.setClipRect(r);
  auto screen = [&](double x, double y) {
    return QPointF(r.left() + x * r.width(), r.bottom() - y * r.height());
  };
  if (!m_density.isNull())
    p.drawImage(r, m_density);
  else
    for (const auto &v : m_frame.points) {
      if (v.label >= 0) {
        auto c = classColor(v.label);
        p.setPen(QColor(c.red, c.green, c.blue));
      } else
        p.setPen(t.textMuted);
      p.drawEllipse(screen(v.x, v.y), 2, 2);
    }
  p.setPen(QPen(t.focusRing, 2));
  for (int i : m_selected)
    if (i >= 0 && i < m_frame.points.size()) {
      const auto &v = m_frame.points[i];
      p.drawPoint(screen(v.x, v.y));
    }
  if (!m_gesture.isEmpty()) {
    p.setPen(QPen(t.focusRing, 1, Qt::DashLine));
    p.setBrush(Qt::NoBrush);
    p.drawPolygon(QPolygonF(m_gesture));
  }
  p.restore();
  if (m_frame.points.isEmpty()) {
    p.setPen(t.textMuted);
    p.drawText(r, Qt::AlignCenter, tr("选择两个或更多通道，然后读取样本"));
  }
}
void CrossplotCanvas::mousePressEvent(QMouseEvent *e) {
  if (e->button() != Qt::LeftButton || !plotRect().contains(e->position()))
    return;
  m_dragging = true;
  m_box = e->modifiers().testFlag(Qt::ShiftModifier);
  m_start = e->position();
  m_gesture = {m_start};
}
void CrossplotCanvas::mouseMoveEvent(QMouseEvent *e) {
  if (!m_dragging)
    return;
  const auto end = e->position();
  if (m_box)
    m_gesture = {m_start, {end.x(), m_start.y()}, end, {m_start.x(), end.y()}};
  else if ((end - m_gesture.last()).manhattanLength() > 3)
    m_gesture << end;
  update();
}
void CrossplotCanvas::mouseReleaseEvent(QMouseEvent *e) {
  if (!m_dragging || e->button() != Qt::LeftButton)
    return;
  m_dragging = false;
  if ((e->position() - m_start).manhattanLength() < 4)
    emit pointRequested(normalized(e->position()));
  else if (m_gesture.size() >= 3) {
    QVector<QPointF> vertices;
    for (auto p : m_gesture)
      vertices << normalized(p);
    emit lassoRequested(vertices);
  }
  m_gesture.clear();
  update();
}

CrossplotPanel::CrossplotPanel(QWidget *parent) : QWidget(parent) {
  setObjectName(QStringLiteral("crossplotPanel"));
  setFont(PaleoTheme::bodyFont());
  auto *outer = new QHBoxLayout(this);
  outer->setContentsMargins(8, 8, 8, 8);
  outer->setSpacing(8);
  auto *controls = new QWidget(this);
  auto *form = new QVBoxLayout(controls);
  form->setContentsMargins(0, 0, 0, 0);
  form->setSpacing(8);
  controls->setMaximumWidth(320);
  m_sources = new QListWidget(controls);
  m_sources->setObjectName(QStringLiteral("crossplotSources"));
  m_sources->setSelectionMode(QAbstractItemView::ExtendedSelection);
  m_sources->setAccessibleName(tr("交会数据通道"));
  m_sources->setMinimumHeight(80);
  form->addWidget(m_sources);
  m_load = new QPushButton(tr("读取选中通道"), controls);
  m_load->setObjectName(QStringLiteral("crossplotLoad"));
  form->addWidget(m_load);
  auto *axesRow = new QFormLayout;
  axesRow->setSpacing(8);
  m_x = new QComboBox(controls);
  m_y = new QComboBox(controls);
  m_z = new QComboBox(controls);
  for (auto pair : {qMakePair(m_x, "crossplotX"), qMakePair(m_y, "crossplotY"),
                    qMakePair(m_z, "crossplotZ")}) {
    pair.first->setObjectName(QString::fromLatin1(pair.second));
    pair.first->setMinimumContentsLength(4);
    pair.first->setSizeAdjustPolicy(
        QComboBox::AdjustToMinimumContentsLengthWithIcon);
  }
  axesRow->addRow(tr("X 轴"), m_x);
  axesRow->addRow(tr("Y 轴"), m_y);
  axesRow->addRow(tr("Z 轴"), m_z);
  m_x->setAccessibleName(tr("X 轴"));
  m_y->setAccessibleName(tr("Y 轴"));
  m_z->setAccessibleName(tr("Z 轴"));
  form->addLayout(axesRow);
  auto *rotation = new QHBoxLayout;
  m_yaw = new QDoubleSpinBox(controls);
  m_pitch = new QDoubleSpinBox(controls);
  m_yaw->setRange(-180, 180);
  m_pitch->setRange(-89, 89);
  m_yaw->setValue(30);
  m_pitch->setValue(20);
  m_yaw->setPrefix(tr("方位 "));
  m_pitch->setPrefix(tr("俯仰 "));
  rotation->addWidget(m_yaw);
  rotation->addWidget(m_pitch);
  form->addLayout(rotation);
  m_method = new QComboBox(controls);
  m_method->setObjectName(QStringLiteral("crossplotMethod"));
  m_method->addItems({tr("k-means++"), tr("高斯混合（对角 EM）"),
                      tr("手选凸包规则"), tr("手选多维盒规则")});
  form->addWidget(m_method);
  auto *params = new QHBoxLayout;
  m_k = new QSpinBox(controls);
  m_k->setObjectName(QStringLiteral("crossplotK"));
  m_k->setRange(1, 255);
  m_k->setValue(8);
  m_k->setPrefix(tr("类别数 "));
  m_class = new QSpinBox(controls);
  m_class->setRange(0, 254);
  m_class->setPrefix(tr("手选类别 "));
  params->addWidget(m_k);
  params->addWidget(m_class);
  form->addLayout(params);
  m_standardize = new QCheckBox(tr("各通道标准化（均值 / 标准差）"), controls);
  m_standardize->setChecked(true);
  form->addWidget(m_standardize);
  m_run = new QPushButton(tr("运行分类"), controls);
  m_run->setObjectName(QStringLiteral("crossplotRun"));
  m_write = new QPushButton(tr("写回工程 / 编图"), controls);
  m_write->setObjectName(QStringLiteral("crossplotWrite"));
  m_cancel = new QPushButton(tr("取消"), controls);
  form->addWidget(m_run);
  form->addWidget(m_write);
  form->addWidget(m_cancel);
  m_progress = new QProgressBar(controls);
  m_progress->setRange(0, 100);
  form->addWidget(m_progress);
  form->addStretch();
  auto *scroll = new QScrollArea(this);
  scroll->setWidget(controls);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setFixedWidth(320);
  outer->addWidget(scroll);
  auto *display = new QVBoxLayout;
  m_canvas = new CrossplotCanvas(this);
  display->addWidget(m_canvas, 1);
  m_stats = new QLabel(tr("选区：N=0"), this);
  m_stats->setFont(PaleoTheme::monoFont());
  m_stats->setWordWrap(true);
  m_stats->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  display->addWidget(m_stats);
  m_classes = new QLabel(this);
  m_classes->setWordWrap(true);
  m_classes->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  display->addWidget(m_classes);
  m_message =
      new QLabel(tr("簇编号不代表地质相；写回后可在智能编图中矢量化。"), this);
  m_message->setWordWrap(true);
  m_message->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  display->addWidget(m_message);
  outer->addLayout(display, 1);
  connect(m_load, &QPushButton::clicked, this, [this] {
    QStringList ids;
    for (auto *item : m_sources->selectedItems())
      ids << item->data(Qt::UserRole).toString();
    emit samplesRequested(ids);
  });
  auto changed = [this] {
    m_lasso.clear();
    emit axesRequested(axes());
  };
  for (auto *combo : {m_x, m_y, m_z})
    connect(combo, qOverload<int>(&QComboBox::currentIndexChanged), this,
            changed);
  for (auto *spin : {m_yaw, m_pitch})
    connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            changed);
  connect(m_canvas, &CrossplotCanvas::lassoRequested, this,
          [this](const QVector<QPointF> &v) {
            m_lasso = v;
            emit lassoRequested(v);
          });
  connect(m_canvas, &CrossplotCanvas::pointRequested, this,
          &CrossplotPanel::pointRequested);
  connect(m_run, &QPushButton::clicked, this, [this] {
    ClassificationOptions o;
    o.method = Classifier(m_method->currentIndex());
    o.k = m_k->value();
    o.manualClass = m_class->value();
    o.standardize = m_standardize->isChecked();
    o.axes = axes();
    o.selection = m_lasso;
    emit classifyRequested(o);
  });
  connect(m_write, &QPushButton::clicked, this,
          &CrossplotPanel::writeRequested);
  connect(m_cancel, &QPushButton::clicked, this,
          &CrossplotPanel::cancelRequested);
  setBusy(false);
}
Axes CrossplotPanel::axes() const {
  return {m_x->currentIndex(), m_y->currentIndex(), m_z->currentIndex() - 1,
          m_yaw->value(), m_pitch->value()};
}
void CrossplotPanel::setSources(const QVector<SourceChoice> &sources) {
  m_sources->clear();
  for (const auto &src : sources) {
    auto *item = new QListWidgetItem(src.title, m_sources);
    item->setData(Qt::UserRole, src.id);
    item->setToolTip(src.kind);
  }
}
void CrossplotPanel::setDimensions(const QStringList &names) {
  QSignalBlocker x(m_x), y(m_y), z(m_z);
  m_x->clear();
  m_y->clear();
  m_z->clear();
  m_z->addItem(tr("Z：二维"));
  m_x->addItems(names);
  m_y->addItems(names);
  m_z->addItems(names);
  m_x->setCurrentIndex(0);
  m_y->setCurrentIndex(names.size() > 1 ? 1 : 0);
  m_haveSamples = names.size() >= 2;
  m_lasso.clear();
  m_classified = false;
  setBusy(m_busy);
}
void CrossplotPanel::setFrame(const PlotFrame &f) {
  m_lasso.clear();
  m_canvas->setFrame(f);
}
void CrossplotPanel::setSelection(const Selection &s,
                                  const QStringList &names) {
  m_canvas->setSelection(s.indices);
  QStringList values;
  for (int i = 0; i < s.means.size(); ++i)
    values << tr("%1 均值=%2").arg(names.value(i)).arg(s.means[i], 0, 'g', 6);
  m_stats->setText(tr("选区：N=%1，占比=%2% · %3")
                       .arg(s.indices.size())
                       .arg(s.fraction * 100, 0, 'f', 2)
                       .arg(values.join(QStringLiteral("；"))));
}
void CrossplotPanel::setBusy(bool busy) {
  if (busy && !m_busy)
    m_progress->setValue(0);
  m_busy = busy;
  for (auto *w : QList<QWidget *>{m_sources, m_x, m_y, m_z, m_yaw, m_pitch,
                                  m_method, m_k, m_class, m_standardize})
    w->setEnabled(!busy);
  m_load->setEnabled(!busy);
  m_run->setEnabled(!busy && m_haveSamples);
  m_write->setEnabled(!busy && m_classified);
  m_cancel->setEnabled(busy);
  m_progress->setVisible(busy);
  m_run->setToolTip(m_haveSamples ? QString() : tr("请先读取至少两个通道"));
  m_write->setToolTip(m_classified ? QString() : tr("请先完成分类"));
  m_cancel->setToolTip(busy ? QString() : tr("当前没有运行中的任务"));
}
void CrossplotPanel::setProgress(int percent) { m_progress->setValue(percent); }
void CrossplotPanel::setMessage(const QString &text) {
  m_message->setText(text);
}
void CrossplotPanel::setClassified(bool available,
                                   const QVector<qint64> &counts) {
  m_classified = available;
  QStringList items;
  for (int i = 0; i < counts.size(); ++i) {
    const auto color = classColor(i);
    items << tr("<span style='color:%1'>■</span> 类别 %2：%3")
                 .arg(QColor(color.red, color.green, color.blue).name())
                 .arg(i)
                 .arg(counts[i]);
  }
  m_classes->setTextFormat(Qt::RichText);
  m_classes->setText(items.join(QStringLiteral(" · ")));
  setBusy(m_busy);
}
} // namespace paleo::crossplot
