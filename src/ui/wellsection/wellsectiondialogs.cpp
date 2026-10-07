// 层：视图
// token 例外：DESIGN 数据符号例外：剖面曲线颜色编辑器的初始/回退色，写入曲线配置。（tools/ui-token-exceptions.json 精确计数）。
#include "wellsectiondialogs.h"

#include "ui/paleotheme.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

#include <QSet>
#include <algorithm>
#include <cmath>
#include <memory>

namespace {
constexpr int kIdRole = Qt::UserRole;

void setSwatch(QToolButton *btn, const QColor &c)
{
  btn->setProperty("swatchColor", c.name());
  btn->setStyleSheet(QStringLiteral("background:%1; border:1px solid %2;")
                         .arg(c.name(), PaleoTheme::tokens().border.name()));
}

QColor swatchColor(QToolButton *btn, const QColor &fallback)
{
  const QColor c(btn->property("swatchColor").toString());
  return c.isValid() ? c : fallback;
}

void pickSwatch(QToolButton *btn, QWidget *parent, const QString &title)
{
  const QColor picked =
      QColorDialog::getColor(swatchColor(btn, Qt::black), parent, title);
  if (picked.isValid())
    setSwatch(btn, picked);
}
} // namespace

// ---------------------------------------------------------------------------
// WellSectionWellsDialog
// ---------------------------------------------------------------------------
WellSectionWellsDialog::WellSectionWellsDialog(
    const QVector<WellSectionPanel::WellChoice> &choices,
    const QStringList &current, const QStringList &mapSelection,
    QWidget *parent)
    : QDialog(parent)
{
  setWindowTitle(tr("选择连井的井与顺序"));
  setMinimumWidth(320);
  setModal(true);

  for (const auto &c : choices)
    if (c.hasCoordinates)
      m_coords.insert(c.id, QPointF(c.x, c.y));

  auto *lay = new QVBoxLayout(this);
  m_search = new QLineEdit(this);
  m_search->setObjectName(QStringLiteral("wellSectionPickSearch"));
  m_search->setPlaceholderText(tr("搜索井名"));
  m_search->setClearButtonEnabled(true);
  lay->addWidget(m_search);

  m_list = new QListWidget(this);
  m_list->setObjectName(QStringLiteral("wellSectionPickList"));
  m_list->setDragDropMode(QAbstractItemView::InternalMove);
  m_list->setDefaultDropAction(Qt::MoveAction);
  m_list->setSelectionMode(QAbstractItemView::SingleSelection);

  // 当前井按剖面序在前（勾选），其余按名排序。
  const auto itemFor = [&](const WellSectionPanel::WellChoice &c,
                           bool checked) {
    QString text = c.name;
    if (!c.hasCoordinates)
      text += tr("（无坐标）");
    auto *it = new QListWidgetItem(text);
    it->setData(kIdRole, c.id);
    it->setFlags(it->flags() | Qt::ItemIsUserCheckable |
                 Qt::ItemIsDragEnabled);
    it->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
    if (!c.hasCoordinates)
      PaleoTheme::setItemTextColor(it, PaleoTheme::ItemTextColor::Muted);
    return it;
  };
  QSet<QString> inCurrent;
  for (const QString &id : current)
    for (const auto &c : choices)
      if (c.id == id)
      {
        m_list->addItem(itemFor(c, true));
        inCurrent.insert(id);
      }
  QVector<WellSectionPanel::WellChoice> rest;
  for (const auto &c : choices)
    if (!inCurrent.contains(c.id))
      rest << c;
  std::sort(rest.begin(), rest.end(),
            [](const auto &a, const auto &b) { return a.name < b.name; });
  for (const auto &c : rest)
    m_list->addItem(itemFor(c, false));

  auto *mid = new QHBoxLayout;
  mid->addWidget(m_list, 1);
  auto *sideCol = new QVBoxLayout;
  const auto mkSide = [&](const char *obj, const QString &tip, const QString &txt) {
    auto *b = new QToolButton(this);
    b->setObjectName(QLatin1String(obj));
    b->setText(txt);
    b->setToolTip(tip);
    sideCol->addWidget(b);
    return b;
  };
  auto *up = mkSide("wellSectionPickUpButton", tr("上移所选井"), tr("↑"));
  auto *down = mkSide("wellSectionPickDownButton", tr("下移所选井"), tr("↓"));
  sideCol->addStretch(1);
  mid->addLayout(sideCol);
  lay->addLayout(mid, 1);

  auto *btnRow = new QHBoxLayout;
  auto *mapBtn = new QPushButton(tr("地图选中"), this);
  mapBtn->setObjectName(QStringLiteral("wellSectionPickMapButton"));
  mapBtn->setToolTip(tr("勾选并追加地图上选中的井"));
  mapBtn->setEnabled(!mapSelection.isEmpty());
  auto *sortBtn = new QPushButton(tr("按井位排序"), this);
  sortBtn->setObjectName(QStringLiteral("wellSectionPickSortButton"));
  sortBtn->setToolTip(tr("按井口坐标的主轴方向排列勾选井"));
  btnRow->addWidget(mapBtn);
  btnRow->addWidget(sortBtn);
  btnRow->addStretch(1);
  lay->addLayout(btnRow);

  auto *box = new QDialogButtonBox(QDialogButtonBox::Ok |
                                       QDialogButtonBox::Cancel,
                                   this);
  box->button(QDialogButtonBox::Ok)->setText(tr("确定"));
  box->button(QDialogButtonBox::Cancel)->setText(tr("取消"));
  connect(box, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
  lay->addWidget(box);

  connect(m_search, &QLineEdit::textChanged, this, [this](const QString &s) {
    for (int i = 0; i < m_list->count(); ++i)
      m_list->item(i)->setHidden(
          !s.isEmpty() &&
          !m_list->item(i)->text().contains(s, Qt::CaseInsensitive));
  });
  connect(up, &QToolButton::clicked, this, [this] { moveCurrent(-1); });
  connect(down, &QToolButton::clicked, this, [this] { moveCurrent(1); });
  const auto sel = std::make_shared<QStringList>(mapSelection);
  connect(mapBtn, &QPushButton::clicked, this,
          [this, sel] { checkAppend(*sel); });
  connect(sortBtn, &QPushButton::clicked, this,
          [this] { sortCheckedByPosition(); });
}

QStringList WellSectionWellsDialog::selectedIds() const
{
  QStringList out;
  for (int i = 0; i < m_list->count(); ++i)
    if (m_list->item(i)->checkState() == Qt::Checked)
      out << m_list->item(i)->data(kIdRole).toString();
  return out;
}

void WellSectionWellsDialog::checkAppend(const QStringList &ids)
{
  for (const QString &id : ids)
    for (int i = 0; i < m_list->count(); ++i)
      if (m_list->item(i)->data(kIdRole).toString() == id)
      {
        QListWidgetItem *it = m_list->takeItem(i);
        it->setCheckState(Qt::Checked);
        m_list->addItem(it); // 追加到末尾，保持地图选择序
        break;
      }
}

void WellSectionWellsDialog::sortCheckedByPosition()
{
  // 勾选井按井位 PCA 主轴投影排序；无坐标的排在末尾。
  QVector<QPair<QString, double>> proj;
  QStringList noCoord;
  for (int i = 0; i < m_list->count(); ++i)
  {
    QListWidgetItem *it = m_list->item(i);
    if (it->checkState() != Qt::Checked)
      continue;
    const QString id = it->data(kIdRole).toString();
    const auto it2 = m_coords.constFind(id);
    if (it2 == m_coords.constEnd())
    {
      noCoord << id;
      continue;
    }
    proj.append({id, 0.0});
    proj.last().second = it2->x(); // 占位，先收集
  }
  if (proj.isEmpty())
    return;
  // 均值 + 协方差 → 主轴方向。
  double mx = 0, my = 0;
  int n = 0;
  for (const auto &p : proj)
  {
    const QPointF c = m_coords.value(p.first);
    mx += c.x();
    my += c.y();
    ++n;
  }
  mx /= n;
  my /= n;
  double sxx = 0, sxy = 0, syy = 0;
  for (const auto &p : proj)
  {
    const QPointF c = m_coords.value(p.first);
    sxx += (c.x() - mx) * (c.x() - mx);
    sxy += (c.x() - mx) * (c.y() - my);
    syy += (c.y() - my) * (c.y() - my);
  }
  const double theta = 0.5 * std::atan2(2 * sxy, sxx - syy);
  const double dx = std::cos(theta), dy = std::sin(theta);
  for (auto &p : proj)
  {
    const QPointF c = m_coords.value(p.first);
    p.second = c.x() * dx + c.y() * dy;
  }
  std::sort(proj.begin(), proj.end(),
            [](const auto &a, const auto &b) { return a.second < b.second; });

  // 重排：勾选项按投影序 + 无坐标勾选项 + 未勾选项（原列表序）。
  QHash<QString, QListWidgetItem *> byId;
  QVector<QListWidgetItem *> rest;
  for (int i = 0; i < m_list->count(); ++i)
  {
    QListWidgetItem *it = m_list->item(i);
    const QString id = it->data(kIdRole).toString();
    byId.insert(id, it);
    if (it->checkState() != Qt::Checked)
      rest << it;
  }
  for (int i = m_list->count() - 1; i >= 0; --i)
    m_list->takeItem(i);
  for (const auto &p : proj)
    if (auto *it = byId.value(p.first))
      m_list->addItem(it);
  for (const QString &id : noCoord)
    if (auto *it = byId.value(id))
      m_list->addItem(it);
  for (auto *it : rest)
    m_list->addItem(it);
}

void WellSectionWellsDialog::moveCurrent(int delta)
{
  const int row = m_list->currentRow();
  const int to = row + delta;
  if (row < 0 || to < 0 || to >= m_list->count())
    return;
  QListWidgetItem *it = m_list->takeItem(row);
  m_list->insertItem(to, it);
  m_list->setCurrentRow(to);
}

// ---------------------------------------------------------------------------
// WellSectionTracksDialog
// ---------------------------------------------------------------------------
WellSectionTracksDialog::WellSectionTracksDialog(
    const wellsection::SectionTemplate &t, const QStringList &mnemonics,
    QWidget *parent)
    : QDialog(parent), m_tpl(t), m_tracks(t.tracks), m_mnemonics(mnemonics)
{
  setWindowTitle(tr("设置显示的井道与参与连井的分层"));
  setModal(true);
  resize(560, 480);

  auto *lay = new QVBoxLayout(this);
  auto *mid = new QHBoxLayout;

  // 左：道列表 + 增删。
  auto *left = new QVBoxLayout;
  m_trackList = new QListWidget(this);
  m_trackList->setObjectName(QStringLiteral("wellSectionTrackList"));
  m_trackList->setDragDropMode(QAbstractItemView::InternalMove);
  m_trackList->setDefaultDropAction(Qt::MoveAction);
  left->addWidget(m_trackList, 1);
  auto *trackBtns = new QHBoxLayout;
  auto *addBtn = new QToolButton(this);
  addBtn->setObjectName(QStringLiteral("wellSectionTrackAddButton"));
  addBtn->setText(tr("添加"));
  addBtn->setPopupMode(QToolButton::InstantPopup);
  auto *addMenu = new QMenu(addBtn);
  const auto addAction = [&](const QString &text, wellsection::TrackKind k) {
    QAction *a = addMenu->addAction(text);
    connect(a, &QAction::triggered, this, [this, k] { addTrack(k); });
  };
  addAction(tr("小层"), wellsection::TrackKind::Zone);
  addAction(tr("曲线"), wellsection::TrackKind::Curve);
  addAction(tr("深度"), wellsection::TrackKind::Depth);
  addAction(tr("岩性"), wellsection::TrackKind::Lithology);
  addAction(tr("相代码充填"), wellsection::TrackKind::Facies);
  addAction(tr("图片道（岩心/薄片照片）"), wellsection::TrackKind::Image);
  addBtn->setMenu(addMenu);
  auto *removeBtn = new QToolButton(this);
  removeBtn->setObjectName(QStringLiteral("wellSectionTrackRemoveButton"));
  removeBtn->setText(tr("删除"));
  trackBtns->addWidget(addBtn);
  trackBtns->addWidget(removeBtn);
  trackBtns->addStretch(1);
  left->addLayout(trackBtns);
  mid->addLayout(left, 0);

  // 右：选中道表单 + 分层过滤节。
  auto *right = new QVBoxLayout;
  auto *form = new QFormLayout;
  m_title = new QLineEdit(this);
  m_title->setObjectName(QStringLiteral("wellSectionTrackTitle"));
  m_width = new QSpinBox(this);
  m_width->setObjectName(QStringLiteral("wellSectionTrackWidth"));
  m_width->setRange(24, 200);
  form->addRow(tr("标题"), m_title);
  form->addRow(tr("宽度"), m_width);

  m_curveForm = new QWidget(this);
  auto *cf = new QFormLayout(m_curveForm);
  cf->setContentsMargins(0, 0, 0, 0);
  const auto mkCombo = [&](QComboBox *&box, const char *obj) {
    box = new QComboBox(m_curveForm);
    box->setObjectName(QLatin1String(obj));
    box->setEditable(true);
    box->addItems(m_mnemonics);
    cf->addRow(obj == QLatin1String("wellSectionCurve1") ? tr("曲线1")
                                                       : tr("曲线2"),
               box);
  };
  const auto mkSpin = [&](QDoubleSpinBox *&sp, const char *obj,
                          const QString &label) {
    sp = new QDoubleSpinBox(m_curveForm);
    sp->setObjectName(QLatin1String(obj));
    sp->setRange(-1e6, 1e6);
    sp->setDecimals(3);
    cf->addRow(label, sp);
  };
  mkCombo(m_curve1, "wellSectionCurve1");
  mkSpin(m_min1, "wellSectionCurve1Min", tr("最小"));
  mkSpin(m_max1, "wellSectionCurve1Max", tr("最大"));
  m_log1 = new QCheckBox(tr("对数坐标"), m_curveForm);
  m_log1->setObjectName(QStringLiteral("wellSectionCurve1Log"));
  cf->addRow(QString(), m_log1);
  m_color1 = new QToolButton(m_curveForm);
  m_color1->setObjectName(QStringLiteral("wellSectionCurve1Color"));
  m_color1->setFixedSize(40, 22);
  connect(m_color1, &QToolButton::clicked, this,
          [this] { pickSwatch(m_color1, this, tr("曲线颜色")); });
  cf->addRow(tr("颜色"), m_color1);
  m_sandFill = new QCheckBox(tr("砂岩充填"), m_curveForm);
  m_sandFill->setObjectName(QStringLiteral("wellSectionSandFill"));
  cf->addRow(QString(), m_sandFill);
  mkSpin(m_cutoff, "wellSectionCutoff", tr("截断值"));

  m_curve2On = new QCheckBox(tr("叠加第二条"), m_curveForm);
  m_curve2On->setObjectName(QStringLiteral("wellSectionCurve2On"));
  cf->addRow(QString(), m_curve2On);
  m_curve2Row = new QWidget(m_curveForm);
  auto *c2f = new QFormLayout(m_curve2Row);
  c2f->setContentsMargins(0, 0, 0, 0);
  m_curve2 = new QComboBox(m_curve2Row);
  m_curve2->setObjectName(QStringLiteral("wellSectionCurve2"));
  m_curve2->setEditable(true);
  m_curve2->addItems(m_mnemonics);
  c2f->addRow(tr("曲线2"), m_curve2);
  m_min2 = new QDoubleSpinBox(m_curve2Row);
  m_min2->setObjectName(QStringLiteral("wellSectionCurve2Min"));
  m_min2->setRange(-1e6, 1e6);
  m_min2->setDecimals(3);
  c2f->addRow(tr("最小"), m_min2);
  m_max2 = new QDoubleSpinBox(m_curve2Row);
  m_max2->setObjectName(QStringLiteral("wellSectionCurve2Max"));
  m_max2->setRange(-1e6, 1e6);
  m_max2->setDecimals(3);
  c2f->addRow(tr("最大"), m_max2);
  m_log2 = new QCheckBox(tr("对数坐标"), m_curve2Row);
  m_log2->setObjectName(QStringLiteral("wellSectionCurve2Log"));
  c2f->addRow(QString(), m_log2);
  m_color2 = new QToolButton(m_curve2Row);
  m_color2->setObjectName(QStringLiteral("wellSectionCurve2Color"));
  m_color2->setFixedSize(40, 22);
  connect(m_color2, &QToolButton::clicked, this,
          [this] { pickSwatch(m_color2, this, tr("曲线颜色")); });
  c2f->addRow(tr("颜色"), m_color2);
  cf->addRow(m_curve2Row);

  m_curve3On = new QCheckBox(tr("叠加第三条"), m_curveForm);
  m_curve3On->setObjectName(QStringLiteral("wellSectionCurve3On"));
  cf->addRow(QString(), m_curve3On);
  m_curve3Row = new QWidget(m_curveForm);
  auto *c3f = new QFormLayout(m_curve3Row);
  c3f->setContentsMargins(0, 0, 0, 0);
  m_curve3 = new QComboBox(m_curve3Row);
  m_curve3->setObjectName(QStringLiteral("wellSectionCurve3"));
  m_curve3->setEditable(true);
  m_curve3->addItems(m_mnemonics);
  c3f->addRow(tr("曲线3"), m_curve3);
  m_min3 = new QDoubleSpinBox(m_curve3Row);
  m_min3->setObjectName(QStringLiteral("wellSectionCurve3Min"));
  m_min3->setRange(-1e6, 1e6);
  m_min3->setDecimals(3);
  c3f->addRow(tr("最小"), m_min3);
  m_max3 = new QDoubleSpinBox(m_curve3Row);
  m_max3->setObjectName(QStringLiteral("wellSectionCurve3Max"));
  m_max3->setRange(-1e6, 1e6);
  m_max3->setDecimals(3);
  c3f->addRow(tr("最大"), m_max3);
  m_log3 = new QCheckBox(tr("对数坐标"), m_curve3Row);
  m_log3->setObjectName(QStringLiteral("wellSectionCurve3Log"));
  c3f->addRow(QString(), m_log3);
  m_color3 = new QToolButton(m_curve3Row);
  m_color3->setObjectName(QStringLiteral("wellSectionCurve3Color"));
  m_color3->setFixedSize(40, 22);
  connect(m_color3, &QToolButton::clicked, this,
          [this] { pickSwatch(m_color3, this, tr("曲线颜色")); });
  c3f->addRow(tr("颜色"), m_color3);
  cf->addRow(m_curve3Row);
  form->addRow(m_curveForm);

  m_lithoForm = new QWidget(this);
  auto *lf = new QFormLayout(m_lithoForm);
  lf->setContentsMargins(0, 0, 0, 0);
  m_lithoSource = new QComboBox(m_lithoForm);
  m_lithoSource->setObjectName(QStringLiteral("wellSectionLithoSource"));
  m_lithoSource->setEditable(true);
  m_lithoSource->addItems(m_mnemonics);
  lf->addRow(tr("来源曲线"), m_lithoSource);
  m_lithoCutoff = new QDoubleSpinBox(m_lithoForm);
  m_lithoCutoff->setObjectName(QStringLiteral("wellSectionLithoCutoff"));
  m_lithoCutoff->setRange(-1e6, 1e6);
  m_lithoCutoff->setDecimals(2);
  lf->addRow(tr("截断值"), m_lithoCutoff);
  form->addRow(m_lithoForm);

  right->addLayout(form);

  // 参与连井的分层。
  auto *filterBox = new QGroupBox(tr("参与连井的分层"), this);
  auto *fl = new QVBoxLayout(filterBox);
  m_filterMapping = new QRadioButton(tr("编图层位"), filterBox);
  m_filterMapping->setObjectName(QStringLiteral("wellSectionFilterMapping"));
  m_filterAll = new QRadioButton(tr("全部分层"), filterBox);
  m_filterAll->setObjectName(QStringLiteral("wellSectionFilterAll"));
  m_filterCustom = new QRadioButton(tr("自定义"), filterBox);
  m_filterCustom->setObjectName(QStringLiteral("wellSectionFilterCustom"));
  fl->addWidget(m_filterMapping);
  fl->addWidget(m_filterAll);
  fl->addWidget(m_filterCustom);
  m_topList = new QListWidget(filterBox);
  m_topList->setObjectName(QStringLiteral("wellSectionTopList"));
  fl->addWidget(m_topList);
  right->addWidget(filterBox, 1);

  mid->addLayout(right, 1);
  lay->addLayout(mid, 1);

  auto *bottom = new QHBoxLayout;
  auto *defBtn = new QPushButton(tr("恢复默认"), this);
  defBtn->setObjectName(QStringLiteral("wellSectionTrackDefaults"));
  bottom->addWidget(defBtn);
  bottom->addStretch(1);
  auto *box = new QDialogButtonBox(QDialogButtonBox::Ok |
                                       QDialogButtonBox::Cancel,
                                   this);
  box->button(QDialogButtonBox::Ok)->setText(tr("确定"));
  box->button(QDialogButtonBox::Cancel)->setText(tr("取消"));
  bottom->addWidget(box);
  lay->addLayout(bottom);

  refreshList();
  m_trackList->setCurrentRow(0);
  // 分层过滤装载。
  switch (m_tpl.topFilter)
  {
    case wellsection::TopFilter::Mapping: m_filterMapping->setChecked(true); break;
    case wellsection::TopFilter::All: m_filterAll->setChecked(true); break;
    case wellsection::TopFilter::Custom: m_filterCustom->setChecked(true); break;
  }

  connect(m_trackList, &QListWidget::currentRowChanged, this,
          [this](int cur) {
            if (m_loading)
              return;
            if (m_formRow >= 0 && m_formRow != cur)
              storeTrack(m_formRow); // 切走前把表单写回旧行
            loadTrack(cur);
          });
  // 拖排后让 m_tracks 与列表同序。
  connect(m_trackList->model(), &QAbstractItemModel::rowsMoved, this,
          [this](const QModelIndex &, int start, int end, const QModelIndex &,
                 int dest) {
            if (start != end)
              return; // 单行拖移
            int to = dest > start ? dest - 1 : dest;
            if (to != start && start < m_tracks.size() && to < m_tracks.size())
            {
              m_tracks.move(start, to);
              if (m_formRow == start)
                m_formRow = to; // 表单跟随被拖的道
            }
            ++m_loading; // 重建列表不打回写
            refreshList();
            --m_loading;
          });
  connect(removeBtn, &QToolButton::clicked, this,
          [this] { removeCurrent(); });
  connect(defBtn, &QPushButton::clicked, this,
          [this] { restoreDefaults(); });
  connect(box, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(m_curve2On, &QCheckBox::toggled, this,
          [this](bool on) { m_curve2Row->setVisible(on); });
  connect(m_curve3On, &QCheckBox::toggled, this,
          [this](bool on) { m_curve3Row->setVisible(on); });
  const auto filterSync = [this] {
    m_topList->setEnabled(m_filterCustom->isChecked());
  };
  for (auto *r : {m_filterMapping, m_filterAll, m_filterCustom})
    connect(r, &QRadioButton::toggled, this, filterSync);
  filterSync();
  loadTrack(m_trackList->currentRow());
}

void WellSectionTracksDialog::setTopNames(const QStringList &names)
{
  m_topList->clear();
  for (const QString &n : names)
  {
    auto *it = new QListWidgetItem(n, m_topList);
    it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
    it->setCheckState(m_tpl.customTops.isEmpty() ||
                              m_tpl.customTops.contains(n)
                          ? Qt::Checked
                          : Qt::Unchecked);
  }
}

void WellSectionTracksDialog::refreshList()
{
  m_trackList->clear();
  for (const auto &t : m_tracks)
    m_trackList->addItem(t.displayTitle());
}

void WellSectionTracksDialog::loadTrack(int row)
{
  if (row < 0 || row >= m_tracks.size())
    return;
  ++m_loading;
  m_formRow = row;
  const auto &t = m_tracks[row];
  m_title->setText(t.title);
  m_width->setValue(t.width);
  const bool isCurve = t.kind == wellsection::TrackKind::Curve;
  const bool isLitho = t.kind == wellsection::TrackKind::Lithology;
  m_curveForm->setVisible(isCurve);
  m_lithoForm->setVisible(isLitho);
  if (isCurve && !t.curves.isEmpty())
  {
    const auto &c1 = t.curves.first();
    m_curve1->setCurrentText(c1.mnemonic);
    m_min1->setValue(c1.min);
    m_max1->setValue(c1.max);
    m_log1->setChecked(c1.logScale);
    setSwatch(m_color1, c1.color);
    const bool two = t.curves.size() > 1;
    m_curve2On->setChecked(two);
    m_curve2Row->setVisible(two);
    if (two)
    {
      const auto &c2 = t.curves.at(1);
      m_curve2->setCurrentText(c2.mnemonic);
      m_min2->setValue(c2.min);
      m_max2->setValue(c2.max);
      m_log2->setChecked(c2.logScale);
      setSwatch(m_color2, c2.color);
    }
    const bool three = t.curves.size() > 2;
    m_curve3On->setChecked(three);
    m_curve3Row->setVisible(three);
    if (three)
    {
      const auto &c3 = t.curves.at(2);
      m_curve3->setCurrentText(c3.mnemonic);
      m_min3->setValue(c3.min);
      m_max3->setValue(c3.max);
      m_log3->setChecked(c3.logScale);
      setSwatch(m_color3, c3.color);
    }
    m_sandFill->setChecked(t.sandFill);
    m_cutoff->setValue(t.cutoff);
  }
  if (isLitho)
  {
    m_lithoSource->setCurrentText(t.sourceMnemonic);
    m_lithoCutoff->setValue(t.cutoff);
  }
  --m_loading;
}

void WellSectionTracksDialog::storeTrack(int row)
{
  if (row < 0 || row >= m_tracks.size())
    return;
  auto &t = m_tracks[row];
  t.title = m_title->text().trimmed();
  t.width = m_width->value();
  if (t.kind == wellsection::TrackKind::Curve)
  {
    // label 表单无此字段——三条曲线都在截断前取既有项保 label。
    const wellsection::CurveStyle keep1 = t.curves.isEmpty()
                                              ? wellsection::CurveStyle{}
                                              : t.curves.first();
    const wellsection::CurveStyle keep2 =
        t.curves.size() > 1 ? t.curves.at(1) : wellsection::CurveStyle{};
    const wellsection::CurveStyle keep3 =
        t.curves.size() > 2 ? t.curves.at(2) : wellsection::CurveStyle{};
    wellsection::CurveStyle c1 = keep1;
    c1.mnemonic = m_curve1->currentText().trimmed();
    c1.min = m_min1->value();
    c1.max = m_max1->value();
    c1.logScale = m_log1->isChecked();
    c1.color = swatchColor(m_color1, c1.color);
    t.curves = {c1};
    if (m_curve2On->isChecked())
    {
      wellsection::CurveStyle c2 = keep2;
      c2.mnemonic = m_curve2->currentText().trimmed();
      c2.min = m_min2->value();
      c2.max = m_max2->value();
      c2.logScale = m_log2->isChecked();
      c2.color = swatchColor(m_color2, QColor(QStringLiteral("#2B59C3")));
      t.curves << c2;
      if (m_curve3On->isChecked())
      {
        wellsection::CurveStyle c3 = keep3;
        c3.mnemonic = m_curve3->currentText().trimmed();
        c3.min = m_min3->value();
        c3.max = m_max3->value();
        c3.logScale = m_log3->isChecked();
        c3.color = swatchColor(m_color3, QColor(QStringLiteral("#1F7A4D")));
        t.curves << c3;
      }
    }
    t.sandFill = m_sandFill->isChecked();
    t.cutoff = m_cutoff->value();
  }
  else if (t.kind == wellsection::TrackKind::Lithology)
  {
    t.sourceMnemonic = m_lithoSource->currentText().trimmed();
    t.cutoff = m_lithoCutoff->value();
  }
  ++m_loading; // refreshList/clear 会打 currentRowChanged——抑制级联
  refreshList();
  m_trackList->setCurrentRow(row);
  --m_loading;
}

void WellSectionTracksDialog::addTrack(wellsection::TrackKind kind)
{
  storeTrack(m_trackList->currentRow());
  wellsection::TrackSpec t;
  t.kind = kind;
  t.width = 56;
  if (kind == wellsection::TrackKind::Image)
    t.width = 90; // 照片道宽（等比缩放基准）
  if (kind == wellsection::TrackKind::Curve)
  {
    wellsection::CurveStyle c;
    c.mnemonic = m_mnemonics.isEmpty() ? QStringLiteral("GR")
                                     : m_mnemonics.first();
    c.min = 0;
    c.max = 150;
    c.color = QColor(QStringLiteral("#333333"));
    t.curves << c;
  }
  m_tracks << t;
  refreshList();
  m_trackList->setCurrentRow(m_tracks.size() - 1);
}

void WellSectionTracksDialog::removeCurrent()
{
  const int row = m_trackList->currentRow();
  if (row < 0 || row >= m_tracks.size())
    return;
  m_tracks.removeAt(row);
  m_formRow = -1; // 被删行的表单作废，不可回写
  ++m_loading;
  refreshList();
  m_trackList->setCurrentRow(qMin(row, m_tracks.size() - 1));
  --m_loading;
  loadTrack(m_trackList->currentRow());
}

void WellSectionTracksDialog::restoreDefaults()
{
  const auto d = wellsection::SectionTemplate::defaults();
  m_tracks = d.tracks;
  m_tpl.topFilter = d.topFilter;
  m_tpl.customTops = d.customTops;
  m_filterMapping->setChecked(true);
  ++m_loading; // clear/setCurrentRow 打 currentRowChanged——别让旧表单写回新模板
  refreshList();
  m_trackList->setCurrentRow(0);
  --m_loading;
  loadTrack(0);
}

wellsection::SectionTemplate WellSectionTracksDialog::result() const
{
  // const 接口：回写当前表单需要可写副本。
  auto *self = const_cast<WellSectionTracksDialog *>(this);
  self->storeTrack(m_trackList->currentRow());
  wellsection::SectionTemplate out = m_tpl;
  out.tracks = m_tracks;
  out.topFilter = m_filterAll->isChecked()
                      ? wellsection::TopFilter::All
                      : m_filterCustom->isChecked()
                            ? wellsection::TopFilter::Custom
                            : wellsection::TopFilter::Mapping;
  out.customTops.clear();
  for (int i = 0; i < m_topList->count(); ++i)
    if (m_topList->item(i)->checkState() == Qt::Checked)
      out.customTops << m_topList->item(i)->text();
  return out;
}
