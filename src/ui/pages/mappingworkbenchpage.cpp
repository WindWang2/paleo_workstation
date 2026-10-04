// 层：视图
// token 例外：DESIGN 数据符号例外：地质相模型表格中可编辑的颜色值。（tools/ui-token-exceptions.json 精确计数）。
#include "ui/paleotheme.h"
#include "mappingworkbenchpage.h"
#include "pageshared.h"
#include <qgscollapsiblegroupbox.h>
#include "../../domain/faciescatalog.h"
#include "../../domain/singlefactorrequest.h"
#include "../../services/singlefactordef.h"
#include "../../workflow/mappingworkbench.h"
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <climits>

MappingWorkbenchPage::MappingWorkbenchPage(const QString &mode,
                                           MappingWorkbench *workbench,
                                           QWidget *parent)
    : QWidget(parent), m_workbench(workbench), m_mode(mode) {
  setObjectName("mappingWorkbench." + mode);
  // The dock manager owns the viewport. A second scroll area here traps the
  // wheel between nested ranges when the parameter groups are expanded.
  auto *body = this;
  auto *layout = new QVBoxLayout(body);
  layout->setContentsMargins(PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm);
  layout->setSpacing(PaleoTheme::tokens().spacingSm);
  m_heading = new QLabel(body);
  QFont title = m_heading->font();
  title.setPointSize(PaleoTheme::tokens().titlePt);
  title.setBold(true);
  m_heading->setFont(title);
  m_heading->setTextFormat(Qt::PlainText);
  layout->addWidget(m_heading);
  auto label = [body, layout](const QString &text) {
    auto *w = new QLabel(text, body);
    w->setWordWrap(true);
    w->setTextFormat(Qt::PlainText);
    layout->addWidget(w);
    return w;
  };
  auto button = [body, this](const QString &name, const QString &text) {
    auto *b = new QPushButton(text, body);
    b->setObjectName("workbench." + name);
    b->setAccessibleName(text);
    if (name == "predict" || name == "factor" || name == "compose")
      paleo::pagesinternal::markPrimaryButton(b);
    connect(b, &QPushButton::clicked, this, [this, name] { issue(name); });
    return b;
  };
  auto *form = new QFormLayout;
  form->setRowWrapPolicy(QFormLayout::WrapLongRows);
  form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  layout->addWidget(paleo::pagesinternal::caption(tr("输入与参数"), body));
  layout->addLayout(form);
  if (mode == "predict") {
    label(tr("远端预测 · 模拟数据 · 待复核\n"
             "选择一个地震体，或勾选一口／多口井。结果按当前层位与相分类"
             "生成。"));
    m_kind = new QComboBox(body);
    m_kind->setObjectName("predictionKind");
    m_kind->setAccessibleName(tr("预测类型"));
    m_kind->addItem(tr("地震体 → 相栅格"), "seismic");
    m_kind->addItem(tr("测井 → 预测相点"), "wells");
    form->addRow(tr("预测类型"), m_kind);
    m_inputs = new QListWidget(body);
    m_inputs->setObjectName("workbenchInputs");
    m_inputs->setFixedHeight(96);
    m_inputs->setAccessibleName(tr("编图输入，按空格勾选"));
    layout->addWidget(m_inputs);
    connect(m_kind, &QComboBox::currentIndexChanged, this,
            [this] { refreshInputs(); });
    auto *all = new QPushButton(body);
    all->setObjectName("workbenchSelectInputs");
    layout->addWidget(all);
    connect(all, &QPushButton::clicked, this, [this] {
      const bool clear = !checkedInputs().isEmpty() &&
          (m_kind->currentData() == "seismic" ||
           checkedInputs().size() == m_inputs->count());
      const QSignalBlocker blocker(m_inputs);
      for (int i = 0; i < m_inputs->count(); ++i)
        m_inputs->item(i)->setCheckState(
            !clear && (m_kind->currentData() == "wells" || i == 0)
                ? Qt::Checked
                : Qt::Unchecked);
      updateState();
    });
    auto *run = new QHBoxLayout;
    run->addWidget(button("predict", tr("运行预测")));
    run->addWidget(button("cancel", tr("取消")));
    layout->addLayout(run);
    m_progress = new QProgressBar(body);
    m_progress->setRange(0, 100);
    m_progress->setVisible(false);
    layout->addWidget(m_progress);
    connect(workbench, &MappingWorkbench::predictionProgress, m_progress,
            &QProgressBar::setValue);
    connect(workbench, &MappingWorkbench::predictionBusyChanged, this,
            [this](bool busy) {
              m_progress->setVisible(busy);
              if (busy)
                m_progress->setValue(0);
              updateState();
            });
  } else if (mode == "constraint") {
    label(tr("1 选择带数值字段的样点\n2 导入或绘制屏障约束线\n3 "
             "生成连续单因素栅格，再提取等值线"));
    m_points = new QComboBox(body);
    m_points->setObjectName("workbenchPoints");
    m_points->setAccessibleName(tr("样点图层"));
    form->addRow(tr("样点图层"), m_points);
    m_factor = new QComboBox(body);
    m_factor->setAccessibleName(tr("单因素"));
    m_factor->addItem(tr("砂岩厚度"), "sandthick");
    m_factor->addItem(tr("砂地比"), "sandratio");
    m_factor->addItem(tr("地层厚度"), "strathick");
    m_factor->addItem(tr("孔隙度"), "poro");
    m_factor->addItem(tr("渗透率"), "perm");
    form->addRow(tr("单因素"), m_factor);
    m_field = new QLineEdit(body);
    m_field->setAccessibleName(tr("数值字段"));
    m_field->setPlaceholderText(tr("输入样点数值字段名"));
    form->addRow(tr("数值字段"), m_field);
    m_cell = new QDoubleSpinBox(body);
    m_cell->setAccessibleName(tr("网格间距，米"));
    m_cell->setRange(.1, 100000);
    m_cell->setValue(100);
    m_cell->setSuffix(tr(" m"));
    form->addRow(tr("网格间距"), m_cell);
    auto *constraints = new QHBoxLayout;
    constraints->addWidget(button("import", tr("导入约束线")));
    constraints->addWidget(button("draw", tr("画布绘制约束线")));
    layout->addLayout(constraints);
    layout->addWidget(button("factor", tr("生成单因素图")));
    m_interval = new QDoubleSpinBox(body);
    m_interval->setAccessibleName(tr("等值线间隔"));
    m_interval->setRange(.001, 1000000);
    m_interval->setDecimals(3);
    m_interval->setValue(1);
    auto *contourForm = new QFormLayout;
    contourForm->setRowWrapPolicy(QFormLayout::WrapLongRows);
    contourForm->addRow(tr("等值线间隔"), m_interval);
    layout->addLayout(contourForm);
    layout->addWidget(button("contours", tr("从选中栅格生成等值线")));
    connect(m_field, &QLineEdit::textChanged, this, [this] { updateState(); });
    connect(m_points, &QComboBox::currentIndexChanged, this,
            [this] { updateState(); });
  } else {
    label(tr("勾选本层位输入，按列表从上到下优先采用有效相值。测井相点使用最近"
             "邻；连续单因素按下方阈值分相。其他图件可打开独立参考窗口。"));
    m_inputs = new QListWidget(body);
    m_inputs->setObjectName("workbenchInputs");
    m_inputs->setFixedHeight(96);
    m_inputs->setAccessibleName(tr("编图输入，按空格勾选"));
    layout->addWidget(m_inputs);
    auto *order = new QHBoxLayout;
    for (bool up : {true, false}) {
      auto *b = new QPushButton(up ? tr("上移优先级") : tr("下移优先级"), body);
      b->setObjectName(up ? "workbenchMoveUp" : "workbenchMoveDown");
      order->addWidget(b);
      connect(b, &QPushButton::clicked, this, [this, up] {
        int i = m_inputs->currentRow(), j = i + (up ? -1 : 1);
        if (i < 0 || j < 0 || j >= m_inputs->count())
          return;
        auto *item = m_inputs->takeItem(i);
        m_inputs->insertItem(j, item);
        m_inputs->setCurrentItem(item);
      });
    }
    layout->addLayout(order);
    m_thresholds = new QLineEdit(body);
    m_thresholds->setAccessibleName(tr("单因素分相阈值"));
    m_thresholds->setPlaceholderText(tr("例如 10, 25（3 类相）"));
    form->addRow(tr("单因素分相阈值"), m_thresholds);
    label(tr("不同量纲的单因素请分别分相。当前一次编图使用同一组阈值；相栅格和"
             "相点不使用阈值。"));
    layout->addWidget(button("compose", tr("生成综合相图与相面")));
  }
  m_inputHint = label(QString());
  m_inputHint->setObjectName("workbenchInputHint");
  PaleoTheme::applyThemedStyleSheet(m_inputHint, [] {
    return PaleoTheme::mutedCaptionStyleSheet();
  });
  if (m_inputs)
    connect(m_inputs, &QListWidget::currentRowChanged, this,
            [this] { updateState(); });
  if (m_inputs)
    connect(m_inputs, &QListWidget::itemChanged, this,
            [this](QListWidgetItem *item) {
              if (m_kind && m_kind->currentData() == "seismic" &&
                  item->checkState() == Qt::Checked) {
                QSignalBlocker block(m_inputs);
                for (int i = 0; i < m_inputs->count(); ++i)
                  if (m_inputs->item(i) != item)
                    m_inputs->item(i)->setCheckState(Qt::Unchecked);
              }
              updateState();
            });
  m_message = label(QString());
  m_message->setObjectName("workbenchMessage");
  m_message->hide();
  m_message->setTextInteractionFlags(Qt::TextSelectableByMouse);
  layout->addSpacing(PaleoTheme::tokens().spacingSm);
  layout->addWidget(paleo::pagesinternal::caption(tr("图件与版本"), body));
  m_results = new QTreeWidget(body);
  m_results->setObjectName("workbenchResults");
  m_results->setHeaderLabels({tr("图件"), tr("版本")});
  m_results->setRootIsDecorated(false);
  m_results->setAccessibleName(tr("本层位图件与版本，按回车显示"));
  m_results->setSelectionMode(QAbstractItemView::SingleSelection);
  m_results->setFixedHeight(120);
  m_results->header()->setSectionResizeMode(0, QHeaderView::Stretch);
  m_results->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  layout->addWidget(m_results);
  m_resultHint = label(QString());
  m_resultHint->setObjectName("workbenchResultHint");
  PaleoTheme::applyThemedStyleSheet(m_resultHint, [] {
    return PaleoTheme::mutedCaptionStyleSheet();
  });
  auto *actions = new QGridLayout;
  actions->addWidget(button("show", tr("显示 / 定位")), 0, 0);
  actions->addWidget(button("compare", tr("打开参考窗口")), 0, 1);
  actions->addWidget(button("polygonize", tr("相栅格转相面")), 1, 0);
  actions->addWidget(button("copy", tr("复制底图并编辑")), 1, 1);
  actions->addWidget(button("save", tr("保存图件新版本")), 2, 0, 1, 2);
  if (mode == "predict")
    actions->addWidget(button("welltracks", tr("查看井道 / 修订测井相")), 3, 0,
                       1, 2);
  layout->addLayout(actions);
  auto *appearance = new QgsCollapsibleGroupBox(tr("相类别与标注"), body);
  appearance->setObjectName("workbenchAppearance");
  appearance->setSaveCollapsedState(false);
  auto *appearanceLayout = new QVBoxLayout(appearance);
  auto *faciesRow = new QHBoxLayout;
  m_editFacies = new QComboBox(appearance);
  m_editFacies->setAccessibleName(tr("选中要素的相类别"));
  m_editFacies->setObjectName("mapFaciesChoice");
  faciesRow->addWidget(m_editFacies, 1);
  faciesRow->addWidget(button("assignFacies", tr("应用到选中要素")));
  appearanceLayout->addLayout(faciesRow);
  auto *labelMode = new QComboBox(body);
  labelMode->setObjectName("faciesLabelMode");
  labelMode->setAccessibleName(tr("画布文本标注"));
  connect(labelMode, &QComboBox::currentIndexChanged, this, [this] {
    m_labelModeDirty = true;
  });
  labelMode->addItems({tr("隐藏标注"), tr("井名 / 要素序号"), tr("相名称"),
                       tr("井名 / 序号 ＋ 相名称")});
  labelMode->setCurrentIndex(3);
  auto *labelRow = new QHBoxLayout;
  labelRow->addWidget(new QLabel(tr("画布文本标注"), body));
  labelRow->addWidget(labelMode, 1);
  appearanceLayout->addLayout(labelRow);
  appearanceLayout->addWidget(button("labels", tr("应用标注到选中图件")));
  layout->addWidget(appearance);
  appearance->setCollapsed(false); // DESIGN.md: groups start expanded.
  auto *details = new QgsCollapsibleGroupBox(tr("来源与生成参数"), body);
  details->setObjectName("workbenchProvenance");
  details->setSaveCollapsedState(false);
  auto *detailsLayout = new QVBoxLayout(details);
  m_details = new QLabel(tr("选择图件查看来源、生成参数和文件位置。"), details);
  m_details->setTextFormat(Qt::PlainText);
  m_details->setWordWrap(true);
  detailsLayout->addWidget(m_details);
  layout->addWidget(details);
  details->setCollapsed(false);
  m_details->setTextInteractionFlags(Qt::TextSelectableByMouse);
  m_details->setObjectName("workbenchDetails");
  auto *schema = new QgsCollapsibleGroupBox(tr("当前层位相分类"), body);
  schema->setObjectName("workbenchSchema");
  schema->setSaveCollapsedState(false);
  auto *sl = new QVBoxLayout(schema);
  auto *schemaBody = new QWidget(schema);
  sl->addWidget(schemaBody);
  auto *sbl = new QVBoxLayout(schemaBody);
  sbl->setContentsMargins(0, 0, 0, 0);
  m_facies = new QTableWidget(0, 7, schemaBody);
  m_facies->setObjectName("faciesSchema");
  m_facies->setAccessibleName(tr("当前层位相分类"));
  m_facies->setHorizontalHeaderLabels({tr("编码"), tr("类别名称"), tr("颜色"),
                                       tr("相"), tr("亚相"), tr("微相"),
                                       tr("纹理")});
  m_facies->horizontalHeader()->setSectionResizeMode(
      QHeaderView::ResizeToContents);
  m_facies->setMinimumHeight(140);
  sbl->addWidget(m_facies);
  auto *library = new QComboBox(schemaBody);
  library->setObjectName("faciesTextureLibrary");
  library->setAccessibleName(tr("地质纹理库"));
  library->setEditable(true);
  library->setInsertPolicy(QComboBox::NoInsert);
  for (const auto &v : FaciesCatalog::library()) {
    auto f = v.toMap();
    library->addItem(
        QIcon(FaciesCatalog::resourcePath(f.value("icon", f.value("texture")).toString())),
        f.value("category").toString() + " · " + f.value("name").toString(), f);
  }
  sbl->addWidget(library);
  auto *useTexture = new QPushButton(tr("将所选纹理应用到分类行"), schemaBody);
  sbl->addWidget(useTexture);
  connect(useTexture, &QPushButton::clicked, this, [this, library] {
    int row = m_facies->currentRow();
    if (row < 0)
      return;
    const auto f = library->currentData().toMap();
    m_facies->setItem(row, 1, new QTableWidgetItem(f.value("name").toString()));
    m_facies->setItem(row, 2,
                      new QTableWidgetItem(f.value("color").toString()));
    m_facies->setItem(row, 6,
                      new QTableWidgetItem(f.value("texture").toString()));
    if (!m_facies->item(row, 3) || m_facies->item(row, 3)->text().isEmpty())
      m_facies->setItem(row, 3,
                        new QTableWidgetItem(f.value("name").toString()));
  });
  auto *sr = new QHBoxLayout;
  auto *add = new QPushButton(tr("新增相"), schemaBody);
  auto *remove = new QPushButton(tr("删除相"), schemaBody);
  sr->addWidget(add);
  sr->addWidget(remove);
  sr->addWidget(button("schema", tr("保存分类")));
  sbl->addLayout(sr);
  auto *hint =
      new QLabel(tr("分类按层位独立保存；已有图件保留生成时分类。选中分类行后可"
                    "从纹理库应用符号；亚相、微相可留空，未分类要素仍会显示。"),
                 schemaBody);
  hint->setWordWrap(true);
  sbl->addWidget(hint);
  connect(add, &QPushButton::clicked, this, [this] {
    int i = m_facies->rowCount();
    m_facies->insertRow(i);
    int code = 1;
    for (int r = 0; r < i; ++r)
      if (m_facies->item(r, 0))
        code = qMax(code, m_facies->item(r, 0)->text().toInt() + 1);
    m_facies->setItem(i, 0, new QTableWidgetItem(QString::number(code)));
    m_facies->setItem(i, 1, new QTableWidgetItem(tr("新相")));
    m_facies->setItem(i, 2, new QTableWidgetItem("#97B4CE"));
    for (int c = 3; c < 7; ++c)
      m_facies->setItem(i, c, new QTableWidgetItem());
    m_facies->setCurrentCell(i, 1);
  });
  connect(remove, &QPushButton::clicked, this,
          [this] { m_facies->removeRow(m_facies->currentRow()); });
  layout->addWidget(schema);
  schema->setCollapsed(false);
  layout->addStretch();
  connect(m_results, &QTreeWidget::currentItemChanged, this,
          [this] { updateState(); });
  connect(m_results, &QTreeWidget::itemActivated, this,
          [this] { issue("show"); });
  connect(workbench, &MappingWorkbench::changed, this,
          &MappingWorkbenchPage::refresh);
  connect(workbench, &MappingWorkbench::faciesChanged, this,
          [this](const QString &h) {
            if (h != m_horizon)
              return;
            const auto saved = m_horizon;
            m_horizon.clear();
            setHorizon(saved);
          });
  refresh();
}
QPushButton *MappingWorkbenchPage::commandButton(const QString &name) const {
  return findChild<QPushButton *>("workbench." + name);
}
QString MappingWorkbenchPage::selectedLayer() const {
  return m_results->currentItem() ? m_results->currentItem()
                                        ->data(0, Qt::UserRole)
                                        .toMap()
                                        .value("id")
                                        .toString()
                                  : QString();
}
QStringList MappingWorkbenchPage::checkedInputs() const {
  QStringList ids;
  if (m_inputs)
    for (int i = 0; i < m_inputs->count(); ++i)
      if (m_inputs->item(i)->checkState() == Qt::Checked)
        ids << m_inputs->item(i)->data(Qt::UserRole).toString();
  return ids;
}
void MappingWorkbenchPage::setHorizon(const QString &h) {
  if (m_horizon == h)
    return;
  m_horizon = h;
  showMessage(QString());
  refresh();
  const auto schema = m_workbench->facies(h);
  m_facies->setRowCount(schema.size());
  for (int i = 0; i < schema.size(); ++i) {
    auto f = schema[i].toMap();
    m_facies->setItem(i, 0, new QTableWidgetItem(f.value("code").toString()));
    m_facies->setItem(i, 1, new QTableWidgetItem(f.value("name").toString()));
    m_facies->setItem(i, 2, new QTableWidgetItem(f.value("color").toString()));
    int c = 3;
    for (const auto &key : {"facies", "subfacies", "microfacies", "texture"})
      m_facies->setItem(i, c++, new QTableWidgetItem(f.value(key).toString()));
  }
}
void MappingWorkbenchPage::refreshInputs() {
  if (!m_inputs)
    return;
  const auto checked = checkedInputs();
  const auto current = m_inputs->currentItem()
      ? m_inputs->currentItem()->data(Qt::UserRole).toString() : QString();
  QStringList order;
  for (int i = 0; i < m_inputs->count(); ++i)
    order << m_inputs->item(i)->data(Qt::UserRole).toString();
  QSignalBlocker blocker(m_inputs);
  m_inputs->clear();
  auto rows = m_mode == "predict"
                  ? m_workbench->inputs(m_kind->currentData().toString())
                  : m_workbench->products(m_horizon);
  if (m_mode == "compose")
    std::stable_sort(rows.begin(), rows.end(),
                     [order](const QVariant &a, const QVariant &b) {
                       int x = order.indexOf(a.toMap().value("id").toString()),
                           y = order.indexOf(b.toMap().value("id").toString());
                       return (x < 0 ? INT_MAX : x) < (y < 0 ? INT_MAX : y);
                     });
  for (const auto &v : rows) {
    auto row = v.toMap();
    if (m_mode == "compose" &&
        (row.value("horizon").toString() != m_horizon ||
         row.value("kind").toString().startsWith("constraint")))
      continue;
    auto *item = new QListWidgetItem(row.value("name").toString(), m_inputs);
    item->setData(Qt::UserRole, row.value("id"));
    item->setToolTip(row.value("name").toString());
    if (row.value("id").toString() == current)
      m_inputs->setCurrentItem(item);
    item->setCheckState(checked.contains(row.value("id").toString())
                            ? Qt::Checked
                            : Qt::Unchecked);
  }
  updateState();
}
void MappingWorkbenchPage::refresh() {
  m_heading->setText(m_horizon.isEmpty()
                         ? tr("请选择编图层位")
                         : tr("%1 · %2").arg(m_horizon, m_mode == "predict"
                                                            ? tr("预测编图")
                                                        : m_mode == "constraint"
                                                            ? tr("单因素图")
                                                            : tr("智能编图")));
  const auto selected = selectedLayer();
  const auto pointId =
      m_points ? m_points->currentData().toString() : QString();
  QSignalBlocker block(m_results);
  m_results->clear();
  if (m_points)
    m_points->clear();
  for (const auto &v : m_workbench->products(m_horizon)) {
    auto row = v.toMap();
    auto *item = new QTreeWidgetItem(
        m_results, {row.value("name").toString(),
                    row.value("version").toInt() > 0
                        ? tr("v%1").arg(row.value("version").toInt())
                        : tr("源数据")});
    item->setData(0, Qt::UserRole, row);
    item->setToolTip(0, row.value("path").toString());
    if (row.value("id").toString() == selected)
      m_results->setCurrentItem(item);
    if (m_points && row.value("type") == "vector" &&
        !row.value("kind").toString().startsWith("constraint"))
      m_points->addItem(row.value("name").toString(), row.value("id"));
  }
  if (m_points && m_points->findData(pointId) >= 0)
    m_points->setCurrentIndex(m_points->findData(pointId));
  refreshInputs();
  updateState();
}
void MappingWorkbenchPage::showMessage(const QString &message) {
  m_message->setText(message);
  m_message->setVisible(!message.isEmpty());
}
void MappingWorkbenchPage::updateState() {
  const bool horizon = !m_horizon.isEmpty();
  const auto row = m_results && m_results->currentItem()
                       ? m_results->currentItem()->data(0, Qt::UserRole).toMap()
                       : QVariantMap();
  const bool selected = !row.isEmpty();
  if (auto *labels = findChild<QComboBox *>("faciesLabelMode")) {
    const QSignalBlocker block(labels);
    const int savedMode = m_workbench->labelMode(selectedLayer());
    if (m_labelLayer != selectedLayer() || !m_labelModeDirty)
      labels->setCurrentIndex(savedMode);
    m_labelLayer = selectedLayer();
    m_labelModeDirty = labels->currentIndex() != savedMode;
    labels->setEnabled(selected && row.value("type") == "vector");
    labels->setToolTip(labels->isEnabled() ? tr("选择标注内容后，点击下方应用")
                                         : tr("请先选择矢量相图"));
  }
  auto gate = [this](const QString &name, bool enabled, const QString &reason) {
    if (auto *b = commandButton(name)) {
      b->setEnabled(enabled);
      b->setToolTip(enabled ? b->text() : reason);
    }
  };
  const bool busy = m_workbench->busy();
  const auto checked = checkedInputs();
  const QString readiness = !horizon ? tr("请先在画布上方选择层位")
      : busy ? tr("预测正在运行，可取消；完成后自动显示图件")
      : m_inputs && m_inputs->count() == 0
          ? (m_mode == "predict" ? tr("暂无可用输入，请在「数据管理」导入并关联地震体或测井数据")
                                  : tr("本层位还没有编图输入，请先生成预测图或单因素图"))
      : checked.isEmpty() ? tr("请勾选输入后运行；可用空格切换勾选")
                          : tr("已选择 %1 项输入").arg(checked.size());
  m_inputHint->setText(m_inputs ? readiness
      : !horizon ? tr("请先在画布上方选择层位")
      : m_points && m_points->count() == 0
          ? tr("暂无样点，请先导入带数值字段的样点图层")
          : tr("填写样点字段后生成单因素图，再选择结果提取等值线"));
  gate("predict", horizon && !busy && !checked.isEmpty(), readiness);
  if (auto *select = findChild<QPushButton *>("workbenchSelectInputs")) {
    const bool wells = m_kind->currentData() == "wells";
    const bool all = !checked.isEmpty() && (!wells || checked.size() == m_inputs->count());
    select->setText(all ? tr("清空选择") : wells ? tr("全选井") : tr("选择首个地震体"));
    select->setEnabled(!busy && m_inputs->count() > 0);
    select->setToolTip(select->isEnabled() ? select->text() : readiness);
  }
  if (m_kind) {
    m_kind->setEnabled(!busy);
    m_kind->setToolTip(busy ? readiness : tr("选择预测所用的数据类型"));
  }
  if (m_inputs) {
    m_inputs->setEnabled(m_mode != "predict" || !busy);
    m_inputs->setToolTip(readiness);
    for (bool up : {true, false}) {
      if (auto *b = findChild<QPushButton *>(up ? "workbenchMoveUp" : "workbenchMoveDown")) {
        const int row = m_inputs->currentRow();
        b->setEnabled(row >= 0 && (up ? row > 0 : row + 1 < m_inputs->count()));
        b->setToolTip(b->isEnabled() ? tr("调整选中输入的编图优先级")
            : row < 0 ? tr("请先选中一项输入") : tr("已到列表边界"));
      }
    }
  }
  m_resultHint->setText(m_results->topLevelItemCount() == 0
      ? tr("暂无图件，完成上方运行后，结果会自动显示在此处和画布上")
      : tr("共 %1 项 · 选中后操作，回车或双击显示图件")
            .arg(m_results->topLevelItemCount()));
  gate("cancel", m_workbench->busy(), tr("没有正在执行的预测"));
  gate("factor",
       horizon && m_points && m_points->count() > 0 &&
           !m_field->text().trimmed().isEmpty(),
       tr("选择样点并填写数值字段"));
  const auto parameters = row.value("parameters").toMap();
  const auto kind = row.value("kind").toString();
  const auto valueSource = parameters.value("value_source").toString();
  const bool analysisRaster =
      paleo::singlefactor::isAnalysisFactorRaster(kind, valueSource);
  const bool cartographic =
      paleo::singlefactor::rejectsQuantitativeUse(kind, valueSource);
  gate("contours", selected && row.value("type") == "raster" && analysisRaster,
       cartographic ? tr("解释性制图成果不能当作分析场提取等值线")
                    : tr("先选择一个连续单因素栅格"));
  gate("compose", horizon && !checkedInputs().isEmpty(),
       readiness);
  for (const auto &name : {"import", "draw", "schema"})
    gate(name, horizon, tr("请先选择层位"));
  const auto selectedSchema = m_workbench->versionForLayer(selectedLayer())
                                  .extra.value("facies")
                                  .toList();
  const auto oldCode = m_editFacies->currentData();
  m_editFacies->clear();
  for (const auto &v : selectedSchema) {
    auto f = v.toMap();
    m_editFacies->addItem(
        QIcon(FaciesCatalog::resourcePath(f.value("icon", f.value("texture")).toString())),
        f.value("name").toString(), f.value("code").toInt());
  }
  int oldIndex = m_editFacies->findData(oldCode);
  if (oldIndex >= 0)
    m_editFacies->setCurrentIndex(oldIndex);
  m_editFacies->setEnabled(row.value("draft").toBool() && !selectedSchema.isEmpty());
  m_editFacies->setToolTip(m_editFacies->isEnabled() ? tr("选择要赋予选中要素的相类别")
                                                  : tr("请先复制相图为编辑副本"));
  gate("assignFacies", row.value("draft").toBool() && !selectedSchema.isEmpty(),
       tr("复制相图后，在画布选中要素，再选择相类别"));
  gate("welltracks",
       selected && !m_workbench->wellPredictions(selectedLayer()).isEmpty(),
       tr("选择包含井段的测井相预测或修订结果"));
  gate("labels",
       selected && row.value("type") == "vector" && !selectedSchema.isEmpty(),
       tr("请选择矢量相面或测井相点图"));
  gate("show", selected, tr("请先选择图件"));
  gate("compare", selected, tr("请先选择图件"));
  gate("polygonize",
       selected && row.value("type") == "raster" && !analysisRaster &&
           !cartographic && !row.value("id").toString().startsWith("factor."),
       cartographic ? tr("解释性制图工作场不能参与分相或转面")
                    : tr("选择相类别栅格；连续单因素须先分相"));
  gate("copy",
       selected && row.value("type") == "vector" &&
           !row.value("horizon").toString().isEmpty() &&
           !row.value("kind").toString().startsWith("constraint") &&
           row.value("kind") != "contour_lines",
       tr("选择相矢量底图；栅格须先转相面"));
  gate("save", row.value("draft").toBool(),
       tr("选择人工编辑工作副本，先保存画布编辑，再生成新版本"));
  if (selected) {
    m_details->setText(
        tr("%1\n来源：%2\n后续派生：%5\n方法：%3\n文件：%4")
            .arg(row.value("mock").toBool() ? tr("模拟派生图件 · 待地质复核")
                                            : tr("已关联工程文件"),
                 row.value("parent_names").toStringList().join("；"),
                 row.value("method").toString(), row.value("path").toString(),
                 row.value("children").toStringList().join("；")));
    m_details->setToolTip(QString::fromUtf8(
        QJsonDocument::fromVariant(row.value("parameters")).toJson()));
  } else {
    m_details->setText(tr("选择图件查看来源、生成参数和文件位置。"));
    m_details->setToolTip(QString());
  }
}
void MappingWorkbenchPage::issue(const QString &action, QVariantMap p) {
  if (action == "labels")
    p["label_mode"] = findChild<QComboBox *>("faciesLabelMode")->currentIndex();
  p.insert("horizon", m_horizon);
  p.insert("layer", selectedLayer());
  p.insert("inputs", action == "copy" && m_mode != "compose" ? QStringList()
                                                             : checkedInputs());
  if (action == "predict")
    p.insert("kind", m_kind->currentData());
  if (action == "factor") {
    p.insert("factor", m_factor->currentData());
    p.insert("pointsLayerId", m_points->currentData());
    p.insert("field", m_field->text().trimmed());
    p.insert("cellSize", m_cell->value());
  }
  if (action == "contours")
    p.insert("interval", m_interval->value());
  if (action == "compose") {
    QVariantList thresholds;
    for (const auto &v : m_thresholds->text().split(
             QRegularExpression("[,，;；\\s]+"), Qt::SkipEmptyParts))
      thresholds << v;
    p.insert("thresholds", thresholds);
  }
  if (action == "assignFacies")
    p.insert("code", m_editFacies->currentData());
  if (action == "schema") {
    QVariantList rows;
    for (int i = 0; i < m_facies->rowCount(); ++i) {
      auto text = [this, i](int c) {
        auto *item = m_facies->item(i, c);
        return item ? item->text() : QString();
      };
      rows << QVariantMap{{"code", text(0)},      {"name", text(1)},
                          {"color", text(2)},     {"facies", text(3)},
                          {"subfacies", text(4)}, {"microfacies", text(5)},
                          {"texture", text(6)}};
    }
    p.insert("facies", rows);
  }
  emit commandRequested(action, p);
}

void MappingWorkbenchPage::selectLayer(const QString &id) {
  for (int i = 0; i < m_results->topLevelItemCount(); ++i) {
    auto *item = m_results->topLevelItem(i);
    if (item->data(0, Qt::UserRole).toMap().value("id").toString() == id) {
      m_results->setCurrentItem(item);
      m_results->scrollToItem(item);
      break;
    }
  }
}
