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
namespace {
// 与 services/faciestraining.h isSupervisedClassifier 同口径；视图层不依赖
// services，按枚举显式判定（domain 契约：新增分类器只许追加末尾，Som=4、
// Lda/Qda/Knn=5/6/7——crossplotMethod 索引耦合同一顺序）。
bool supervisedClassifier(Classifier m) {
  return m == Classifier::Lda || m == Classifier::Qda || m == Classifier::Knn;
}
} // namespace
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
  outer->setContentsMargins(PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm);
  outer->setSpacing(PaleoTheme::tokens().spacingSm);
  auto *controls = new QWidget(this);
  auto *form = new QVBoxLayout(controls);
  form->setContentsMargins(0, 0, 0, 0);
  form->setSpacing(PaleoTheme::tokens().spacingSm);
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
  axesRow->setSpacing(PaleoTheme::tokens().spacingSm);
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
  // 项序 = Classifier 枚举序（domain 契约：KMeans/Gmm/Hull/Box/Som/Lda/Qda/Knn，
  // 只许追加）——Classifier(currentIndex()) 的索引耦合靠测试护栏保持。
  m_method->addItems({tr("k-means++"), tr("高斯混合（对角 EM）"),
                      tr("手选凸包规则"), tr("手选多维盒规则"),
                      tr("SOM 自组织图"), tr("LDA 线性判别"),
                      tr("QDA 二次判别"), tr("kNN 近邻")});
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
  // 方法相关参数：SOM 原型网格 / kNN 近邻 / 监督 CV 折数按方法显隐；
  // 掩膜阈值全方法可见（伴生低置信掩膜件用，无监督分支同样落盘）。
  auto *methodParams = new QHBoxLayout;
  methodParams->setSpacing(PaleoTheme::tokens().spacingSm);
  m_somWidth = new QSpinBox(controls);
  m_somWidth->setObjectName(QStringLiteral("crossplotSomWidth"));
  m_somWidth->setRange(2, 16);
  m_somWidth->setValue(4);
  m_somWidth->setPrefix(tr("SOM 宽 "));
  m_somHeight = new QSpinBox(controls);
  m_somHeight->setObjectName(QStringLiteral("crossplotSomHeight"));
  m_somHeight->setRange(2, 16);
  m_somHeight->setValue(4);
  m_somHeight->setPrefix(tr("SOM 高 "));
  m_knnK = new QSpinBox(controls);
  m_knnK->setObjectName(QStringLiteral("crossplotKnnK"));
  m_knnK->setRange(1, 50);
  m_knnK->setValue(5);
  m_knnK->setPrefix(tr("近邻 k "));
  // 推理用模型训练时记录的近邻数（classifyWith 不读 o.knnNeighbors），改此项
  // 只影响下次训练——提示如实说明，避免「改了就行」的误期。
  m_knnK->setToolTip(tr("修改后需重新训练方生效（推理使用模型内记录的近邻数）"));
  m_cvFolds = new QSpinBox(controls);
  m_cvFolds->setObjectName(QStringLiteral("crossplotCvFolds"));
  m_cvFolds->setRange(2, 10);
  m_cvFolds->setValue(5);
  m_cvFolds->setPrefix(tr("CV 折数 "));
  m_maskThreshold = new QSpinBox(controls);
  m_maskThreshold->setObjectName(QStringLiteral("crossplotMaskThreshold"));
  m_maskThreshold->setRange(5, 95);
  m_maskThreshold->setSingleStep(5);
  m_maskThreshold->setValue(50);
  m_maskThreshold->setPrefix(tr("掩膜阈值 "));
  m_maskThreshold->setSuffix(tr(" %"));
  for (auto *w : QList<QWidget *>{m_somWidth, m_somHeight, m_knnK, m_cvFolds,
                                 m_maskThreshold})
    methodParams->addWidget(w);
  form->addLayout(methodParams);
  m_standardize = new QCheckBox(tr("各通道标准化（均值 / 标准差）"), controls);
  m_standardize->setChecked(true);
  form->addWidget(m_standardize);
  // 标注区：套索选区 → 自由词类名 → workflow 训练集（视图只发信号）。
  auto *annotate = new QHBoxLayout;
  annotate->setSpacing(PaleoTheme::tokens().spacingSm);
  m_className = new QComboBox(controls);
  m_className->setObjectName(QStringLiteral("crossplotClassName"));
  m_className->setEditable(true);
  m_className->setPlaceholderText(tr("类名（如：砂岩）"));
  m_className->setAccessibleName(tr("标注类名"));
  m_assign = new QPushButton(tr("标注选区"), controls);
  m_assign->setObjectName(QStringLiteral("crossplotAssignLabel"));
  m_assign->setToolTip(tr("把当前套索选区标注为指定类名（重复标注覆盖）"));
  m_clearTraining = new QPushButton(tr("清除标注"), controls);
  m_clearTraining->setObjectName(QStringLiteral("crossplotClearTraining"));
  m_clearTraining->setToolTip(tr("清空全部标注与已训练模型"));
  annotate->addWidget(m_className);
  annotate->addWidget(m_assign);
  annotate->addWidget(m_clearTraining);
  form->addLayout(annotate);
  m_trainingSummary = new QLabel(QString(), controls);
  m_trainingSummary->setObjectName(QStringLiteral("crossplotTrainingSummary"));
  m_trainingSummary->setWordWrap(true);
  m_trainingSummary->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  form->addWidget(m_trainingSummary);
  m_train = new QPushButton(tr("训练分类器"), controls);
  m_train->setObjectName(QStringLiteral("crossplotTrain"));
  form->addWidget(m_train);
  m_run = new QPushButton(tr("运行分类"), controls);
  m_run->setObjectName(QStringLiteral("crossplotRun"));
  m_write = new QPushButton(tr("写回工程 / 编图"), controls);
  m_write->setObjectName(QStringLiteral("crossplotWrite"));
  m_cancel = new QPushButton(tr("取消"), controls);
  m_cancel->setObjectName(QStringLiteral("crossplotCancel"));
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
  // 训练质量区（只读 rich text）：CV 混淆矩阵 + 每类查准/查全 + warnings。
  m_quality = new QLabel(this);
  m_quality->setObjectName(QStringLiteral("crossplotQuality"));
  m_quality->setWordWrap(true);
  m_quality->setTextFormat(Qt::RichText);
  m_quality->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  display->addWidget(m_quality);
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
    emit classifyRequested(options());
  });
  connect(m_assign, &QPushButton::clicked, this, [this] {
    emit assignLabelRequested(m_className->currentText());
  });
  connect(m_clearTraining, &QPushButton::clicked, this,
          &CrossplotPanel::clearTrainingRequested);
  connect(m_train, &QPushButton::clicked, this,
          &CrossplotPanel::trainRequested);
  connect(m_method, qOverload<int>(&QComboBox::currentIndexChanged), this,
          [this] {
            updateMethodParams();
            emit methodChanged();
          });
  // 训练相关参数变更 → 重算训练可用态（validate 下限与这两项范围耦合）。
  for (auto *spin : {m_cvFolds, m_knnK})
    connect(spin, qOverload<int>(&QSpinBox::valueChanged), this,
            &CrossplotPanel::paramsChanged);
  connect(m_write, &QPushButton::clicked, this,
          &CrossplotPanel::writeRequested);
  connect(m_cancel, &QPushButton::clicked, this,
          &CrossplotPanel::cancelRequested);
  // busy 对偶 tooltip 备份：在各控件常态 tooltip 落定后统一采集（空 tooltip
  // 的 m_className 也入列，busy 期统一显「任务进行中」）。
  for (auto *w : QList<QWidget *>{m_className, m_assign, m_clearTraining,
                                  m_somWidth, m_somHeight, m_knnK, m_cvFolds,
                                  m_maskThreshold})
    m_tooltips << qMakePair(w, w->toolTip());
  updateMethodParams();
  setBusy(false);
  // 训练禁用原因单一出处 = controller 首次 refreshTrainingState 下发；面板
  // 构造期不 hardcode 文案（空原因 + 禁用即可）。
}
ClassificationOptions CrossplotPanel::options() const {
  ClassificationOptions o;
  o.method = Classifier(m_method->currentIndex());
  o.k = m_k->value();
  o.manualClass = m_class->value();
  o.standardize = m_standardize->isChecked();
  o.axes = axes();
  o.selection = m_lasso;
  o.somWidth = m_somWidth->value();
  o.somHeight = m_somHeight->value();
  o.knnNeighbors = m_knnK->value();
  o.cvFolds = m_cvFolds->value();
  o.confidenceMaskThreshold = m_maskThreshold->value() / 100.0; // 百分数 → [0,1]
  return o;
}
void CrossplotPanel::updateMethodParams() {
  const auto method = Classifier(m_method->currentIndex());
  const bool som = method == Classifier::Som;
  const bool knn = method == Classifier::Knn;
  const bool supervised = supervisedClassifier(method);
  m_somWidth->setVisible(som);
  m_somHeight->setVisible(som);
  m_knnK->setVisible(knn);
  m_cvFolds->setVisible(supervised);
  // k（簇数）对 SOM/监督族不适用；手选类别仅凸包/多维盒规则有意义，保持
  // 既有始终可见形制不变。
  m_k->setVisible(!som && !supervised);
  setBusy(m_busy); // 监督族运行门禁随方法切换重算（无监督族恒可用）
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
  // 换样本 = 标注与模型整体作废（workflow setSamples → clearTraining 同语义）：
  // 清标注展示与质量区；训练可用态由 controller 经 trainingChanged 重算，
  // 这里只重置与样本绑定的展示面与已训练记忆。摘要清空（「未标注」等工作流
  // 口径文案由 controller setTrainingSummary 单一出处下发）。
  m_trainingSummary->clear();
  m_quality->clear();
  m_modelTrained = false;
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
  // busy 对偶 tooltip 仅在状态翻转时切/还：busy 期重复 setBusy(true)
  // （setTrainingState/setModelTrained/refreshTrainingState 等路径会重入）
  // 不得把对偶文案误还原。
  const bool flipped = busy != m_busy;
  m_busy = busy;
  for (auto *w : QList<QWidget *>{m_sources, m_x, m_y, m_z, m_yaw, m_pitch,
                                  m_method, m_k, m_class, m_standardize,
                                  m_className, m_assign, m_clearTraining,
                                  m_train, m_somWidth, m_somHeight, m_knnK,
                                  m_cvFolds, m_maskThreshold})
    w->setEnabled(!busy);
  // busy 对偶 tooltip：忙 = 「任务进行中」，闲 = 还原常态文案。
  if (flipped)
    for (auto &p : m_tooltips)
      p.first->setToolTip(busy ? tr("任务进行中") : p.second);
  m_load->setEnabled(!busy);
  // 运行按钮门禁：监督族要求「训练态可用且已训练」（两者均由 controller 依
  // workflow 门禁语义经 setTrainingState/setModelTrained 下发，面板只记状态）；
  // 无监督族（k-means/GMM/凸包/多维盒/SOM）不参与者两态。
  const bool supervised =
      supervisedClassifier(Classifier(m_method->currentIndex()));
  const bool canRun = !supervised || (m_trainEnabled && m_modelTrained);
  m_run->setEnabled(!busy && m_haveSamples && canRun);
  m_write->setEnabled(!busy && m_classified);
  m_cancel->setEnabled(busy);
  m_progress->setVisible(busy);
  m_train->setEnabled(!busy && m_trainEnabled);
  // busy 对偶文案同 m_cancel 先例（任务进行中 vs 当前没有运行中的任务）。
  m_train->setToolTip(busy ? tr("任务进行中") : m_trainReason);
  if (busy)
    m_run->setToolTip(tr("任务进行中")); // 优先级高于监督族禁用原因分支
  else if (!m_haveSamples)
    m_run->setToolTip(tr("请先读取至少两个通道"));
  else if (supervised && !canRun) {
    // 禁用原因按 enabled 分流：训练态都未到位（!m_trainEnabled）才取
    // controller 下发的 reason——setTrainingState(true, warnings) 时该字段
    // 是非阻断警告，不能拿来当禁用原因；训练态可用但模型未训练（方法不
    // 匹配/刚清标注）落固定文案。reason 为空 = 裸面板构造期兜底
    // （controller 首启 refreshTrainingState 后不可达）。
    const QString disableReason = m_trainEnabled ? QString() : m_trainReason;
    m_run->setToolTip(disableReason.isEmpty()
                          ? tr("请先完成标注并训练监督模型")
                          : disableReason);
  } else
    m_run->setToolTip(QString());
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
void CrossplotPanel::setTrainingState(bool enabled, const QString &reason) {
  m_trainEnabled = enabled;
  m_trainReason = reason;
  setBusy(m_busy); // 训练按钮三态 + 监督族运行按钮联动都经这里重算
}
void CrossplotPanel::setTrainingSummary(const QString &summary) {
  m_trainingSummary->setText(summary);
}
void CrossplotPanel::setModelTrained(bool trained) {
  m_modelTrained = trained;
  setBusy(m_busy);
}
void CrossplotPanel::setTrainingQuality(const QVariantMap &report) {
  const QStringList names = report.value("classNames").toStringList();
  const auto cells = report.value("confusionCells").toList();
  const auto precision = report.value("precision").toList();
  const auto recall = report.value("recall").toList();
  if (names.isEmpty()) {
    m_quality->clear();
    return;
  }
  const int c = names.size();
  const auto &t = PaleoTheme::tokens();
  // 百分数一位小数；NaN（无预测的类）以破折号如实呈现，不伪造 0。
  auto pct = [](double v) {
    return std::isfinite(v)
               ? QString::number(v * 100, 'f', 1) + QStringLiteral("%")
               : QStringLiteral("—");
  };
  QString html =
      tr("<div><b>交叉验证质量（%1 折）</b>：查准率（precision）与查全率"
         "（recall）逐类列出；混淆矩阵行 = 真实类别，列 = 预测类别。</div>")
          .arg(report.value("folds").toInt());
  html += QStringLiteral("<table cellspacing='2' cellpadding='2'>");
  html += QStringLiteral("<tr><td></td>");
  for (const QString &name : names)
    html += QStringLiteral("<td><b>%1</b></td>").arg(name.toHtmlEscaped());
  html += QStringLiteral("</tr>");
  for (int i = 0; i < c; ++i) {
    html += QStringLiteral("<tr><td><b>%1</b></td>")
                .arg(names[i].toHtmlEscaped());
    for (int j = 0; j < c; ++j)
      html += QStringLiteral("<td>%1</td>")
                  .arg(cells.value(qsizetype(i) * c + j).toLongLong());
    html += QStringLiteral("</tr>");
  }
  html += QStringLiteral("</table>");
  QStringList metrics;
  for (int i = 0; i < c && i < precision.size() && i < recall.size(); ++i)
    metrics << tr("%1：查准率 %2，查全率 %3")
                  .arg(names[i].toHtmlEscaped())
                  .arg(pct(precision[i].toDouble()))
                  .arg(pct(recall[i].toDouble()));
  html += QStringLiteral("<div>%1</div>").arg(metrics.join(QStringLiteral(" · ")));
  const QStringList warnings = report.value("warnings").toStringList();
  if (!warnings.isEmpty()) {
    QStringList escaped;
    for (const QString &w : warnings)
      escaped << w.toHtmlEscaped();
    html += QStringLiteral("<div style='color:%1'>%2%3</div>")
                .arg(t.textMuted.name(), tr("提示："),
                     escaped.join(QStringLiteral("<br/>")));
  }
  m_quality->setText(html);
}
} // namespace paleo::crossplot
