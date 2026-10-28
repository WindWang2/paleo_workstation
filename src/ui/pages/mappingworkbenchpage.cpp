// 层：视图
// token 例外：DESIGN 数据符号例外：地质相模型表格中可编辑的颜色值。（tools/ui-token-exceptions.json 精确计数）。
#include "mappingworkbenchpage.h"
#include <QCheckBox>
#include "../faciesdisplaycontrol.h"
#include "../../domain/predictionevidence.h"
#include "../../domain/faciescatalog.h"
#include "../../domain/facieshierarchy.h"
#include "../../domain/singlefactorrequest.h"
#include "../../services/singlefactordef.h"
#include "../../workflow/mappingworkbench.h"
#include "wellreadinesspanel.h"
#include "../../qgis/collapsiblegroup.h"
#include "pageshared.h"
#include "ui/paleotheme.h"
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QShowEvent>
#include <QTimer>
#include <QTableWidget>
#include <QTreeWidget>
#include <QTabWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <climits>

MappingWorkbenchPage::MappingWorkbenchPage(const QString &mode,
                                           MappingWorkbench *workbench,
                                           QWidget *parent)
    : QWidget(parent), m_workbench(workbench), m_mode(mode) {
  m_refreshTimer = new QTimer(this);
  m_refreshTimer->setSingleShot(true);
  connect(m_refreshTimer, &QTimer::timeout, this, [this] { if (isVisible()) refresh(); });
  setObjectName("mappingWorkbench." + mode);
  auto *scroll = new QScrollArea(this);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  auto *body = new QWidget(scroll);
  body->setObjectName("mappingWorkbenchBody." + mode);
  scroll->setWidget(body);
  auto *outer = new QVBoxLayout(this);
  outer->setContentsMargins(0, 0, 0, 0);
  outer->addWidget(scroll);
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
  if (mode == "compose") buildBaseMapStep(body, layout);
  auto *form = new QFormLayout;
  form->setRowWrapPolicy(QFormLayout::WrapLongRows);
  form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  if (mode != "compose") {
    layout->addWidget(paleo::pagesinternal::caption(tr("输入与参数"), body));
    layout->addLayout(form);
  } else { delete form; form = nullptr; }
  if (mode == "predict") {
    label(tr("地震层位窗聚类／预测服务 · 结果待复核\n"
             "选择一个地震体，或勾选一口／多口井。结果按当前层位与相分类"
             "生成。测井仅节选当前地层顶底深范围调用远端服务，"
             "每口井显示一个预测微相点及其亚相、相归属，"
             "并同步工区井段与地层预测信息。"));
    m_status = new QLabel(body);
    m_status->setObjectName("workbenchPredictionStatus");
    m_status->setWordWrap(true);
    m_status->setTextFormat(Qt::PlainText);
    m_status->setFont(PaleoTheme::bodyFont(PaleoTheme::kLabelPt));
    layout->addWidget(m_status);
    const auto showStatus = [this] {
      if (m_status && m_workbench)
        m_status->setText(m_kind && m_kind->currentData() == "seismic_mock"
          ? tr("Mock：先保存本层位测井相预测；沿层位提取 ±12 ms 地震反射窗聚类，按簇内测井相多数票定类，无井簇仅从测井已有类别中随机选择。解释需地质复核。")
          : m_kind && m_kind->currentData() == "wells"
              ? m_workbench->wellPredictionStatusHint() : m_workbench->predictionStatusHint());
    };
    showStatus();
    connect(workbench, &MappingWorkbench::predictionStatusChanged, this,
            showStatus);
    m_kind = new QComboBox(body);
    m_kind->setObjectName("predictionKind");
    m_kind->setAccessibleName(tr("预测类型"));
    m_kind->addItem(tr("地震层位窗聚类 → 微相图（Mock）"), "seismic_mock");
    m_kind->addItem(tr("地震体 → 微相栅格（预测服务）"), "seismic");
    m_kind->addItem(tr("测井 → 当前地层预测微相"), "wells");
    // 井点的相 = 该井在当前层位地层（本层位顶 → 下一层位顶）内厚度最大的相，
    // 与数据管理逐组预测写同一张「测井分层沉积相属性」表。
    m_kind->setItemData(m_kind->count() - 1,
                        tr("每口井在当前地层内厚度最大的微相作为预测结果，并关联亚相和相；"
                           "结果按井、按地层写入「测井分层沉积相属性」表，与数据管理共用"),
                        Qt::ToolTipRole);
    form->addRow(tr("预测类型"), m_kind);
    m_kind->setCurrentIndex(m_kind->findData(QStringLiteral("wells")));
    m_horizonSourceRow = new QWidget(body);
    auto *horizonSource = new QHBoxLayout(m_horizonSourceRow);
    horizonSource->setContentsMargins(0, 0, 0, 0);
    horizonSource->setSpacing(PaleoTheme::tokens().spacingSm);
    horizonSource->addWidget(new QLabel(tr("层位文件"), m_horizonSourceRow));
    m_horizonFile = new QLineEdit(m_horizonSourceRow);
    m_horizonFile->setObjectName("workbenchHorizonFile");
    m_horizonFile->setAccessibleName(tr("当前层位原始文件"));
    m_horizonFile->setPlaceholderText(tr("自动使用已导入层位，或直接选择 DAT"));
    m_horizonFile->setToolTip(tr("按文件中的 Inline、Crossline、时间（ms）直接提取地震窗；无需预先生成时间栅格。"));
    horizonSource->addWidget(m_horizonFile, 1);
    m_chooseHorizonFile = new QPushButton(tr("选择…"), m_horizonSourceRow);
    m_chooseHorizonFile->setObjectName("workbenchChooseHorizonFile");
    m_chooseHorizonFile->setAccessibleName(tr("选择当前层位原始文件"));
    horizonSource->addWidget(m_chooseHorizonFile);
    form->addRow(m_horizonSourceRow);
    connect(m_chooseHorizonFile, &QPushButton::clicked, this, [this] {
      const auto path = QFileDialog::getOpenFileName(this,
          tr("选择 %1 的层位时间文件").arg(m_horizon), m_horizonFile->text(),
          tr("SMI 层位文件 (*.dat *.txt);;所有文件 (*)"));
      if (!path.isEmpty()) m_horizonFile->setText(path);
    });
    connect(workbench, &MappingWorkbench::catalogBound, m_horizonFile, &QLineEdit::clear);
    connect(m_kind, &QComboBox::currentIndexChanged, this, showStatus);
    showStatus();
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
          (m_kind->currentData() != "wells" ||
           checkedInputs().size() == m_inputs->count());
      const QSignalBlocker blocker(m_inputs);
      for (int i = 0; i < m_inputs->count(); ++i)
        m_inputs->item(i)->setCheckState(
            !clear && (m_kind->currentData() == "wells" || i == 0)
                ? Qt::Checked
                : Qt::Unchecked);
      updateState();
    });
    auto *run = new QGridLayout;  // 两行网格，避免四钮挤一行撑破窄停靠栏
    run->addWidget(button("predict", tr("运行预测")), 0, 0);
    run->addWidget(button("cancel", tr("取消")), 0, 1);
    run->addWidget(button("wellattributes", tr("岩性与相属性")), 1, 0);
    run->addWidget(button("wellformations", tr("分层沉积相属性")), 1, 1);  // 与分层沉积相表共用结果
    layout->addLayout(run);
    layout->addWidget(new WellReadinessPanel(workbench, m_inputs, [this] { return std::pair{m_horizon,
        m_kind->currentData() == "wells" ? checkedInputs() : QStringList()}; }, body));
    m_progress = new QProgressBar(body);
    m_progress->setObjectName("workbenchPredictionProgress");
    m_progress->setAccessibleName(tr("预测进度"));
    m_progress->setRange(0, 100);
    m_progress->setVisible(false);
    layout->addWidget(m_progress);
    connect(workbench, &MappingWorkbench::predictionProgress, this, [this](int percent) {
      m_progress->setRange(0, 100);
      m_progress->setValue(percent);
    });
    connect(workbench, &MappingWorkbench::predictionDetailChanged, this, [this](const QString &detail) {
      m_progress->setFormat(detail + tr(" · %p%"));
      m_progress->setToolTip(detail);
    });
    connect(workbench, &MappingWorkbench::errorOccurred, this, [this](const QString &error) {
      if (!m_progress->isHidden() && m_progress->value() < 100) {
        m_progress->setRange(0, 100);
        m_progress->setFormat(tr("预测未完成 · %p%"));
        m_progress->setToolTip(error);
      }
    });
    connect(workbench, &MappingWorkbench::predictionBusyChanged, this,
            [this](bool busy) {
              if (busy) {
                m_progress->show();
                m_progress->setFormat(tr("准备预测 · %p%"));
                m_progress->setRange(0, 0);
                m_progress->setValue(0);
              } else if (m_progress->maximum() == 0 || m_progress->value() < 100) {
                m_progress->setRange(0, 100);
                m_progress->setFormat(tr("预测已停止 · %p%"));
              }
              updateState();
            });
  } else if (mode == "constraint") {
    label(
        tr("主流程在「编图流程」页，按输入 → 处理 → 输出：提取井点数据，按默认方法（约束可选）生成单因素图，"
           "再预览、提取等值线或发布。此页管理已保存的图件与版本，供后续编图引用。"));
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
    buildComposeAdvanced(body, layout);
  }
  m_inputHint = label(QString()); m_inputHint->setVisible(mode != "compose");
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
              if (m_kind && m_kind->currentData() != "wells" &&
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
  layout->addWidget(paleo::pagesinternal::caption(mode == "compose" ? tr("3 · 当前编图与编辑") : tr("图件与版本"), body));
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
  m_allHorizons = new QCheckBox(tr("查看全部层位的图件"), body);
  m_allHorizons->setObjectName(QStringLiteral("workbenchAllHorizons"));
  layout->addWidget(m_allHorizons);
  connect(m_allHorizons, &QCheckBox::toggled, this, &MappingWorkbenchPage::refresh);
  m_resultHint = label(QString());
  m_resultHint->setObjectName("workbenchResultHint");
  PaleoTheme::applyThemedStyleSheet(m_resultHint, [] {
    return PaleoTheme::mutedCaptionStyleSheet();
  });
  auto *actions = new QGridLayout;
  actions->addWidget(button("show", tr("显示 / 定位")), 0, 0);
  actions->addWidget(button("compare", tr("在参考面板中查看")), 0, 1);
  actions->addWidget(button("polygonize", tr("相栅格转相面")), 1, 0);
  actions->addWidget(button("copy", tr("复制底图并编辑")), 1, 1);
  if (mode != "compose") actions->addWidget(button("save", tr("保存图件新版本")), 2, 0);
  actions->addWidget(button("catalog", tr("在数据管理中定位")), 2, 1);
  actions->addWidget(button("delete", tr("删除图件")), 3, 0);
  actions->addWidget(button("graph", tr("资料关联 / 成果追溯")), 3, 1);
  if (mode == "predict")
    actions->addWidget(button("welltracks", tr("查看井道 / 修订预测相")), 4, 0,
                       1, 2);
  actions->addWidget(button("consistency", tr("地震相 × 测井相一致性报告")), 5,
                     0, 1, 2);
  layout->addLayout(actions);
  // M4：外观/来源/分类三组默认折叠，但按页模式记住用户上次的展开状态。
  const auto remember = [&mode](const char *name) { return "workbench/" + mode + "/" + name; };
  auto *appearance = paleo::collapsible::create(tr("相类别与标注"), body, "workbenchAppearance", true, remember("appearance"));
  auto *appearanceLayout = new QVBoxLayout(appearance);
  auto *levelForm = new QFormLayout;
  m_displayLevel = new QComboBox(appearance);
  m_displayLevel->setObjectName("faciesDisplayLevel");
  m_displayLevel->setAccessibleName(tr("相图显示层级"));
  m_displayLevel->addItem(tr("自动（按比例尺切换）"), "auto");
  m_editLevel = new QComboBox(appearance);
  m_editLevel->setObjectName("faciesEditLevel");
  m_editLevel->setAccessibleName(tr("相图编辑层级"));
  for (const auto &level : FaciesHierarchy::levels()) {
    m_displayLevel->addItem(FaciesHierarchy::title(level), level);
    m_editLevel->addItem(FaciesHierarchy::title(level), level);
  }
  m_editLevel->setCurrentIndex(2);
  levelForm->addRow(tr("显示层级"), m_displayLevel);
  levelForm->addRow(tr("编辑层级"), m_editLevel);
  m_featureDisplayLevel = new QComboBox(appearance);
  m_featureDisplayLevel->setObjectName("faciesFeatureDisplayLevel");
  m_featureDisplayLevel->setAccessibleName(tr("选中相面的显示层级"));
  m_featureDisplayLevel->addItem(tr("跟随图层（解除锁定）"), "auto");
  for (const auto &level : FaciesHierarchy::levels())
    m_featureDisplayLevel->addItem(tr("锁定为%1").arg(FaciesHierarchy::title(level)), level);
  levelForm->addRow(tr("选中相面"), m_featureDisplayLevel);
  appearanceLayout->addLayout(levelForm);
  appearanceLayout->addWidget(button("featureDisplayLevel", tr("应用显示层级到选中相面")));
  m_featureDisplayState = new QLabel(appearance);
  m_featureDisplayState->setObjectName("faciesFeatureDisplayState"); m_featureDisplayState->setWordWrap(true);
  m_featureDisplayState->setFont(PaleoTheme::bodyFont(PaleoTheme::kLabelPt)); // 次级状态标签按 label 字阶设字体（DESIGN.md typography.label 8pt，同 :100 m_status）
  appearanceLayout->addWidget(m_featureDisplayState); auto *pinGroup = FaciesPinnedLevelList::createGroup(body, &m_pins); // 固定列表单列一组、默认展开
  connect(m_pins, &FaciesPinnedLevelList::pinRequested, this, [this](const QString &key, const QString &mode) {
    issue("pinFaciesLevel", {{"pin_key", key}, {"display_mode", mode}}); });
  connect(m_displayLevel, &QComboBox::currentIndexChanged, this, [this] {
    issue("displayLevel", {{"display_mode", m_displayLevel->currentData()}}); });
  connect(m_editLevel, &QComboBox::currentIndexChanged, this, [this] { updateState(); });
  auto *hierarchyHint = new QLabel(
      tr("三级共用边界。相／亚相改类会更新整个选中父类，并保留目标分类内兼容的"
         "下级；微相改类只作用于选中面。旧分类打开时自动归入三级体系（相／亚相级"
         "落「未细分」，无法识别的落「其他／未归类」），可在维护表重挂归属。"),
      appearance);
  hierarchyHint->setWordWrap(true);
  appearanceLayout->addWidget(hierarchyHint);
  auto *faciesRow = new QHBoxLayout;
  m_editFacies = new QComboBox(appearance);
  m_editFacies->setAccessibleName(tr("选中要素的相类别"));
  m_editFacies->setObjectName("mapFaciesChoice");
  faciesRow->addWidget(m_editFacies, 1);
  faciesRow->addWidget(button("assignFacies", tr("应用到选中要素")));
  appearanceLayout->addLayout(faciesRow);
  appearanceLayout->addWidget(
      button("selectHierarchy", tr("选中同级分类的全部面")));
  auto *labelMode = new QComboBox(body);
  labelMode->setObjectName("faciesLabelMode");
  labelMode->setAccessibleName(tr("画布文本标注"));
  connect(labelMode, &QComboBox::currentIndexChanged, this, [this] {
    m_labelModeDirty = true;
  });
  labelMode->addItems({tr("隐藏标注"), tr("井名 / 要素序号"), tr("相名称"),
                       tr("井名 / 序号 ＋ 相名称"), tr("分级区间（单因素值域）")});
  labelMode->setCurrentIndex(2);
  auto *labelRow = new QHBoxLayout;
  labelRow->addWidget(new QLabel(tr("画布文本标注"), body));
  labelRow->addWidget(labelMode, 1);
  appearanceLayout->addLayout(labelRow);
  appearanceLayout->addWidget(button("labels", tr("应用标注到选中图件")));
  layout->addWidget(pinGroup); layout->addWidget(appearance); // 固定列表置于折叠的外观组之前，易找
  if (mode == "compose") buildDeliveryStep(body, layout);
  auto *evidenceGroup = paleo::collapsible::create(tr("选中要素的解释证据"), body, "workbenchEvidence", mode == "compose");
  auto *groupLayout = new QVBoxLayout(evidenceGroup);
  buildEvidenceSection(evidenceGroup, groupLayout);
  layout->addWidget(evidenceGroup);
  auto *details = paleo::collapsible::create(tr("来源与生成参数"), body, "workbenchProvenance", true, remember("provenance"));
  auto *detailsLayout = new QVBoxLayout(details);
  m_details = new QLabel(tr("选择图件查看来源、生成参数和文件位置。"), details);
  m_details->setTextFormat(Qt::PlainText);
  m_details->setWordWrap(true);
  detailsLayout->addWidget(m_details);
  layout->addWidget(details);
  m_details->setTextInteractionFlags(Qt::TextSelectableByMouse);
  m_details->setObjectName("workbenchDetails");
  auto *schema = paleo::collapsible::create(tr("当前层位相分类"), body, "workbenchSchema", true, remember("schema"));
  auto *sl = new QVBoxLayout(schema);
  auto *schemaBody = new QWidget(schema);
  sl->addWidget(schemaBody);
  buildFaciesSchema(schemaBody);
  layout->addWidget(schema);
  layout->addStretch();
  connect(m_results, &QTreeWidget::currentItemChanged, this,
          [this] { updateState(); emit targetLayerChanged(selectedLayer()); });
  connect(m_results, &QTreeWidget::itemActivated, this,
          [this] { issue("show"); });
  connect(workbench, &MappingWorkbench::displayChanged, this,
          [this](const QString &id) {
            if (id == selectedLayer())
              updateState();
          });
  connect(workbench, &MappingWorkbench::changed, this,
          &MappingWorkbenchPage::scheduleRefresh);
  wireFaciesUpdates();
  paleo::pagesinternal::shrinkComboBoxesToDock(body);
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
    // 约束页输入与编图同口径：只收当前层位与无层位项（products("")
    // 现在返回全部层位，这里显式圈回，语义与旧 products(h) 一致）。
    if (m_mode == "constraint" && !row.value("horizon").toString().isEmpty() &&
        row.value("horizon").toString() != m_horizon)
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
void MappingWorkbenchPage::scheduleRefresh() {
  m_refreshPending = true;
  if (isVisible() && !m_refreshTimer->isActive()) m_refreshTimer->start(0);
}
void MappingWorkbenchPage::showEvent(QShowEvent *event) {
  QWidget::showEvent(event);
  emit evidenceDetailsRequested(evidenceDetailsVisible());
  if (m_refreshPending) refresh();
}
void MappingWorkbenchPage::refresh() {
  m_refreshPending = false;
  m_refreshTimer->stop();
  m_heading->setText(m_horizon.isEmpty()
                         ? tr("请选择编图层位")
                         : tr("%1 · %2").arg(m_horizon, m_mode == "predict"
                                                            ? tr("智能预测")
                                                        : m_mode == "constraint"
                                                            ? tr("单因素图")
                                                            : tr("智能编图")));
  const auto selected = selectedLayer();
  const auto pointId =
      m_points ? m_points->currentData().toString() : QString();
  QSignalBlocker block(m_results);
  QSignalBlocker pointsBlock(m_points);
  m_results->clear();
  if (m_points)
    m_points->clear();
  // 结果列表显示全部层位的成果（标题自带层位名）——激活层位与预测层位
  // 不一致时（如自动层位 C6 而成果在 D61），列表不再空。成果行排前、
  // 源数据行（井位/测区/底图/层位栅格）排后；点约束/证据源等操作入口
  // 仍限当前层位（跨层位输入由编图校验拒绝）。
  QVariantList all = m_workbench->products(QString());
  std::stable_sort(all.begin(), all.end(),
                   [](const QVariant &x, const QVariant &y) {
                     const auto isProduct = [](const QVariant &row) {
                       const auto id = row.toMap().value("id").toString();
                       return id.startsWith(QStringLiteral("product.")) ||
                              id.startsWith(QStringLiteral("draft."));
                     };
                     return isProduct(x) && !isProduct(y);
                   });
  for (const auto &v : all) {
    auto row = v.toMap();
    const QString rowHorizon = row.value("horizon").toString();
    if (!m_allHorizons->isChecked() && !rowHorizon.isEmpty() && rowHorizon != m_horizon) continue;
    auto *item = new QTreeWidgetItem(
        m_results, {row.value("name").toString(),
                    row.value("version").toInt() > 0
                        ? tr("v%1").arg(row.value("version").toInt())
                        : row.value("unregistered_product").toBool() ? tr("未登记") : tr("源数据")});
    item->setData(0, Qt::UserRole, row);
    item->setToolTip(0, row.value("path").toString());
    if (row.value("id").toString() == selected)
      m_results->setCurrentItem(item);
    if (m_points && row.value("type") == "vector" &&
        !row.value("kind").toString().startsWith("constraint") &&
        (rowHorizon.isEmpty() || rowHorizon == m_horizon))
      m_points->addItem(row.value("name").toString(), row.value("id"));
  }
  if (m_points && m_points->findData(pointId) >= 0)
    m_points->setCurrentIndex(m_points->findData(pointId));
  const auto source = m_evidenceSource->currentData();
  {
    const QSignalBlocker block(m_evidenceSource);
    m_evidenceSource->clear();
    m_evidenceSource->addItem(tr("手工解释（无来源图件）"), QString());
    for (const auto &entry : all) {
      const auto p = entry.toMap();
      const QString hz = p.value("horizon").toString();
      if (p.value("id").toString() != selectedLayer() &&
          (hz.isEmpty() || hz == m_horizon))
        m_evidenceSource->addItem(p.value("name").toString(), p.value("id"));
    }
    m_evidenceSource->setCurrentIndex(
        qMax(0, m_evidenceSource->findData(source)));
  }
  refreshInputs();
  updateState();
}
void MappingWorkbenchPage::showMessage(const QString &message) {
  m_message->setText(message);
  m_message->setVisible(!message.isEmpty());
}
void MappingWorkbenchPage::refreshEvidenceCommandGates() {
  if (!m_workbench || !m_results) return;
  const auto row = m_results->currentItem()
                       ? m_results->currentItem()->data(0, Qt::UserRole).toMap()
                       : QVariantMap();
  const auto evidenceSelection = m_workbench->selectedFeatures(selectedLayer());
  const bool editableEvidence = row.value("draft").toBool() &&
      !evidenceSelection.isEmpty() && nativeSelectionMatchesEvidence();
  auto gate = [this](const QString &name, bool enabled, const QString &reason) {
    if (auto *b = commandButton(name)) {
      b->setEnabled(enabled);
      b->setToolTip(enabled ? b->text() : reason);
    }
  };
  gate("addEvidence",
       editableEvidence && m_evidenceText && !m_evidenceText->toPlainText().trimmed().isEmpty(),
       tr("请先在编辑副本的画布中选中要素，再填写证据"));
  const auto actorText = m_evidenceActor ? m_evidenceActor->text().trimmed() : QString();
  const bool localActorOk = actorText.startsWith(QStringLiteral("local:"))
      && !actorText.mid(6).trimmed().isEmpty();
  const bool accountable = localActorOk && m_evidenceReason
      && !m_evidenceReason->text().trimmed().isEmpty();
  const bool singleEditable = editableEvidence && evidenceSelection.size() == 1
      && !m_evidenceSnapshot.contains("error");
  gate("updateEvidence", singleEditable && accountable && m_evidence && m_evidence->currentItem(),
       tr("选择一个要素及证据，并填写操作人和修订理由"));
  gate("verifyEvidence", singleEditable && accountable && m_evidence && m_evidence->currentItem(),
       tr("选择一个要素及证据，并填写数据核验理由"));
  gate("reviewInterpretation", singleEditable && accountable,
       tr("选择一个要素，并填写解释审核理由；确认解释不等于核验数据"));
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
  gate("wellattributes", horizon && !busy, tr("先打开工程并选择层位"));
  gate("wellformations", horizon && !busy, tr("先打开工程并选择层位"));
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
  if (m_horizonSourceRow) {
    m_horizonSourceRow->setVisible(m_kind->currentData() == "seismic_mock");
    m_horizonFile->setEnabled(horizon && !busy);
    m_chooseHorizonFile->setEnabled(horizon && !busy);
    m_chooseHorizonFile->setToolTip(horizon && !busy
        ? tr("选择 %1 的原始层位时间文件，直接从地震体提取").arg(m_horizon)
        : readiness);
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
  gate("references", horizon && !checkedInputs().isEmpty(),
       tr("勾选单因素图、测井相或地震相后，在参考面板的视图中显示"));
  gate("compose", horizon && !checkedInputs().isEmpty(),
       readiness);
  // G05/G09 证据命令门控（full-audit F6）：影响分析需要「层位 + 不在运行 +
  // 已选图件」（无选中时误报「只有单一版本」）；受控重算需要最近一次影响
  // 报告；反查的层位范围类需要层位（无层位时伪报「无匹配结果」）。
  gate("impactAnalysis", horizon && !busy && selected,
       !horizon ? tr("请先选择层位")
                : busy ? readiness
                       : tr("请先在结果列表选择一个图件"));
  gate("recompute", !busy && m_impactReady,
       busy ? readiness : tr("请先运行影响分析，再按其结果受控重算"));
  {
    const auto reverseKind = m_reverseKind
        ? m_reverseKind->currentData().toString()
        : QString();
    const bool reverseNeedsHorizon =
        reverseKind != QStringLiteral("search") &&
        reverseKind != QStringLiteral("references_source");
    gate("reverseQuery", !busy && (!reverseNeedsHorizon || horizon),
         busy ? readiness
              : reverseNeedsHorizon ? tr("请先选择层位（该反查类型按层位过滤）")
                                    : tr("检索证据"));
  }
  for (const auto &name : {"import", "draw", "schema"})
    gate(name, horizon, tr("请先选择层位"));
  const auto selectedSchema = m_workbench->versionForLayer(selectedLayer())
                                  .extra.value("facies")
                                  .toList();
  const bool wellResult = selected && m_workbench->isWellPrediction(selectedLayer());
  const QSignalBlocker displayBlock(m_displayLevel);
  const QSignalBlocker editBlock(m_editLevel);
  if (wellResult) m_editLevel->setCurrentIndex(2);
  m_displayLevel->setCurrentIndex(qMax(
      0, m_displayLevel->findData(m_workbench->displayMode(selectedLayer()))));
  const bool displayable = m_workbench->supportsFaciesDisplay(selectedLayer());
  m_displayLevel->setEnabled(!wellResult && displayable);
  m_displayLevel->setToolTip(
      wellResult ? tr("测井图件只显示最终预测相") : !displayable
          ? tr("请先选择相图件")
          : FaciesDisplayControl::scaleHint(m_workbench->resolvedLevel(selectedLayer()), m_workbench->displayThresholds()));
  const auto displaySelection = m_workbench->selectedFeatures(selectedLayer());
  const bool canLock = row.value("draft").toBool() && !wellResult &&
      m_workbench->isFaciesPolygon(selectedLayer()) && !selectedSchema.isEmpty() &&
      !displaySelection.isEmpty();
  const auto lockHint = tr("在相面编辑副本中选中面，可固定显示层级；跟随图层可解除锁定");
  m_featureDisplayLevel->setEnabled(canLock); m_featureDisplayLevel->setToolTip(lockHint);
  m_pins->setState(FaciesHierarchy::pinCandidates(selectedSchema), m_workbench->pinnedLevels(selectedLayer()), displayable, tr("固定的相类（含下级）始终按所选层级显示，不随显示层级与比例尺切换；单独锁定的相面仍优先"));
  gate("featureDisplayLevel", canLock, lockHint);
  QSet<QString> selectedDisplayModes;
  for (auto fid : displaySelection)
    selectedDisplayModes.insert(m_workbench->featureDisplayMode(selectedLayer(), fid));
  m_featureDisplayState->setText(displaySelection.isEmpty() ? tr("未选中相面")
      : selectedDisplayModes.size() > 1 ? tr("选中 %1 个面，显示层级设置不同").arg(displaySelection.size())
      : tr("选中 %1 个面：%2").arg(displaySelection.size()).arg(
            selectedDisplayModes.contains("auto") ? tr("跟随图层")
                : tr("锁定为%1").arg(FaciesHierarchy::title(*selectedDisplayModes.cbegin()))));
  m_editLevel->setEnabled(!wellResult && row.value("draft").toBool());
  m_editLevel->setToolTip(wellResult ? tr("每井每地层只修订一个预测相") : m_editLevel->isEnabled()
                              ? tr("选择要修改的相层级")
                              : tr("请先复制相图为编辑副本"));
  const auto oldCode = m_editFacies->currentData();
  m_editFacies->clear();
  QSet<QString> editKeys;
  for (const auto &v : selectedSchema) {
    auto f = v.toMap();
    const auto key =
        FaciesHierarchy::key(f, m_editLevel->currentData().toString());
    if (editKeys.contains(key))
      continue;
    editKeys.insert(key);
    m_editFacies->addItem(QIcon(FaciesCatalog::resourcePath(
                              f.value("icon", f.value("texture")).toString())),
                          wellResult ? f.value("name").toString() : FaciesHierarchy::path(f)
                              .mid(0, m_editLevel->currentIndex() + 1)
                              .join(" / "),
                          f.value("code").toInt());
  }
  int oldIndex = m_editFacies->findData(oldCode);
  if (oldIndex >= 0)
    m_editFacies->setCurrentIndex(oldIndex);
  m_editFacies->setEnabled(row.value("draft").toBool() && !selectedSchema.isEmpty());
  m_editFacies->setToolTip(m_editFacies->isEnabled() ? tr("选择要赋予选中要素的相类别")
                                                  : tr("请先复制相图为编辑副本"));
  gate("assignFacies", row.value("draft").toBool() && !selectedSchema.isEmpty(),
       tr("复制相图后，在画布选中要素，再选择相类别"));
  const auto evidenceSelection = m_workbench->selectedFeatures(selectedLayer());
  // Controller snapshots describe the research target, which may differ from
  // native editing selection after graph navigation. Text/form changes must
  // never replace that target with the previously selected native feature.
  const bool ownsTarget = m_evidenceSnapshotOwned &&
      (m_evidenceSnapshot.isEmpty() || m_evidenceSnapshot.value("layer_id").toString() == selectedLayer());
  // #440：选择未变时复用已有详情快照，避免每次 updateState 都跑空间相交。
  // finding-133：多选(size>1)一律重算——原缓存分支只查 contains("records")，选择集变化会静默显示旧图斑证据。
  const bool selectionMatchesCache = !m_evidenceSnapshot.isEmpty() && m_evidenceSnapshot.value("details_deferred").toBool() != evidenceDetailsVisible()
      && m_evidenceSnapshot.value("layer_id").toString() == selectedLayer()
      && ((evidenceSelection.isEmpty() && !m_evidenceSnapshot.contains("feature_id"))
          || (evidenceSelection.size() == 1 && m_evidenceSnapshot.contains("feature_id")
              && evidenceSelection.first() == m_evidenceSnapshot.value("feature_id").toLongLong()));
  QVariantMap detailSnapshot;
  if (ownsTarget || selectionMatchesCache) {
    detailSnapshot = m_evidenceSnapshot;
  } else {
    detailSnapshot = evidenceDetailsVisible() ? m_workbench->featureEvidence(selectedLayer(), evidenceSelection) : m_workbench->featureEvidenceSummary(selectedLayer(), evidenceSelection);
    if (evidenceSelection.size() > 1)
      detailSnapshot.insert("records", m_workbench->evidence(selectedLayer(), evidenceSelection));
  }
  renderEvidenceSnapshot(detailSnapshot);
  const bool editableEvidence = row.value("draft").toBool() &&
      !evidenceSelection.isEmpty() && nativeSelectionMatchesEvidence();
  gate("selectHierarchy",
       !selectedSchema.isEmpty() && !evidenceSelection.isEmpty(),
       tr("请先在画布选择相要素，再按编辑层级扩展选区"));
  gate("addEvidence",
       editableEvidence && !m_evidenceText->toPlainText().trimmed().isEmpty(),
       tr("请先在编辑副本的画布中选中要素，再填写证据"));
  gate("removeEvidence", editableEvidence && m_evidence->currentItem(),
       tr("请选择当前详情对应的地图要素及证据"));
  const auto actorText = m_evidenceActor->text().trimmed();
  const bool localActorOk = actorText.startsWith(QStringLiteral("local:"))
      && !actorText.mid(6).trimmed().isEmpty();
  const bool accountable = localActorOk && !m_evidenceReason->text().trimmed().isEmpty();
  const bool singleEditable = editableEvidence && evidenceSelection.size() == 1 && !m_evidenceSnapshot.contains("error");
  gate("updateEvidence", singleEditable && accountable && m_evidence->currentItem(), tr("选择一个要素及证据，并填写操作人和修订理由"));
  gate("verifyEvidence", singleEditable && accountable && m_evidence->currentItem(), tr("选择一个要素及证据，并填写数据核验理由"));
  gate("reviewInterpretation", singleEditable && accountable, tr("选择一个要素，并填写解释审核理由；确认解释不等于核验数据"));
  gate("locateEvidence", m_evidence->currentItem() || m_generationEvidence->currentItem(), tr("选择带空间来源的证据；双击来源也可定位"));
  gate("evidenceGraph", selected, tr("选择已保存的图件或地图图斑"));
  gate("welltracks",
       wellResult,
       tr("选择测井预测相或修订结果"));
  gate("consistency", selected && !selectedSchema.isEmpty(),
       tr("选择测井相点或地震相（相栅格 / 相面）结果；另一侧自动取同层位最新结果"));
  gate("labels",
       selected && row.value("type") == "vector" && !selectedSchema.isEmpty(),
       tr("请选择矢量相面或测井相点图"));
  gate("show", selected, tr("请先选择图件"));
  gate("compare", selected, tr("请先选择图件"));
  gate("compareRecompute",
       selected,
       tr("请选择受控重算产生的成果（或其相面）；与普通版本对比的像元级差异"));
  gate("catalog",
       !row.value("asset_id").toString().isEmpty() &&
           !row.value("version_id").toString().isEmpty(),
       tr("请先选择已登记的图件版本；工作副本定位到最近保存版本"));
  gate("graph", !row.value("version_id").toString().isEmpty(),
       tr("请选择已登记的图件版本；工作副本追溯最近保存版本"));
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
  const auto rowId = row.value("id").toString();
  const bool product = rowId.startsWith("product.") ||
                      rowId.startsWith("draft.");
  gate("delete", selected && product && !busy,
       !selected ? tr("请先选择图件")
       : !product ? tr("底图与基础图层请在「数据管理」中移除")
                  : tr("预测运行中，完成后可删除图件"));
  if (selected) {
    m_details->setText(
        tr("%1\n来源：%2\n后续派生：%5\n方法：%3\n文件：%4\n证据链：%6")
            .arg(row.value("mock").toBool() ? tr("模拟派生图件 · 待地质复核")
                                            : tr("已关联工程文件"),
                 row.value("parent_names").toStringList().join("；"),
                 row.value("method").toString(), row.value("path").toString(),
                 row.value("children").toStringList().join("；"),
                 paleo::evidence::summary(row.value("parameters")
                                              .toMap()
                                              .value("evidence")
                                              .toMap())));
    m_details->setToolTip(QString::fromUtf8(
        QJsonDocument::fromVariant(row.value("parameters")).toJson()));
  } else {
    m_details->setText(tr("选择图件查看来源、生成参数和文件位置。"));
    m_details->setToolTip(QString());
  }
}
void MappingWorkbenchPage::issue(const QString &action, QVariantMap p) {
  if (action == "historyOlder" || action == "historyNewer") {
    m_historyPage += action == "historyOlder" ? 1 : -1;
    m_historyPage = qMax(0, m_historyPage); m_evidenceRendered = false; renderEvidenceSnapshot(m_evidenceSnapshot); return;
  }
  if (action == "evidenceGraph") { emit evidenceGraphRequested(m_evidenceSnapshot); return; }
  if (action == "reverseQuery" && m_reverseKind) {
    p.insert("reverse_kind", m_reverseKind->currentData());
    // 参数口径：分层位的类型用当前层位；引用来源用生成来源列表选中行的
    // 版本；检索用检索词输入框。视图只收集参数，不执行查询。
    const auto kind = m_reverseKind->currentData().toString();
    if (kind == QStringLiteral("search"))
      p.insert("reverse_argument", m_reverseText ? m_reverseText->text().trimmed() : QString());
    else if (kind == QStringLiteral("references_source")) {
      const auto row = m_generationEvidence && m_generationEvidence->currentItem()
                           ? m_generationEvidence->currentItem()->data(Qt::UserRole).toMap()
                           : QVariantMap();
      const auto manual = m_reverseText ? m_reverseText->text().trimmed() : QString();
      p.insert("reverse_argument", row.contains("source_version")
                                        ? row.value("source_version")
                                        : QVariant(manual));
    } else
      p.insert("reverse_argument", m_horizon);
    m_lastReverseKind = kind;
    m_lastReverseArgument = p.value("reverse_argument").toString();
  }
  if (action == "locateEvidence") {
    const auto source = m_evidence->currentItem() ? m_evidence->currentItem()->data(Qt::UserRole + 1).toMap()
                                                 : m_generationEvidence->currentItem() ? m_generationEvidence->currentItem()->data(Qt::UserRole).toMap() : QVariantMap{};
    emit evidenceActivated(source); return;
  }
  if (action == "labels")
    p["label_mode"] = findChild<QComboBox *>("faciesLabelMode")->currentIndex();
  if (action == "featureDisplayLevel")
    p.insert("display_mode", m_featureDisplayLevel->currentData());
  p.insert("horizon", m_horizon);
  p.insert("layer", selectedLayer());
  p.insert("inputs", action == "copy" ? QStringList() : checkedInputs());
  if (action == "predict") {
    p.insert("kind", m_kind->currentData());
    if (m_kind->currentData() == "seismic_mock")
      p.insert("horizon_file", m_horizonFile->text().trimmed());
  }
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
  if (action == "selectHierarchy")
    p.insert("edit_level", m_editLevel->currentData());
  if (action == "assignFacies") {
    p.insert("code", m_editFacies->currentData());
    p.insert("edit_level", m_editLevel->currentData());
  }
  if (action == "addEvidence") {
    p.insert("text", m_evidenceText->toPlainText());
    p.insert("level", m_editLevel->currentData());
    p.insert("source_layer", m_evidenceSource->currentData());
    p.insert("source_feature_uid", m_sourceFeatureUid->text().trimmed());
    p.insert("role", m_evidenceRole->currentData());
    p.insert("type", m_evidenceType->currentData());
  }
  if (action == "removeEvidence" || action == "updateEvidence" || action == "verifyEvidence")
    p.insert("evidence_id", m_evidence->currentItem()
                                ? m_evidence->currentItem()->data(Qt::UserRole)
                                : QVariant());
  if (action == "updateEvidence") { p.insert("role", m_evidenceRole->currentData()); p.insert("text", m_evidenceText->toPlainText()); p.insert("source_feature_uid", m_sourceFeatureUid->text().trimmed()); }
  if (action == "verifyEvidence") p.insert("verification", m_verification->currentData());
  if (action == "reviewInterpretation") p.insert("status", m_reviewState->currentData());
  if (action == "addEvidence" || action == "removeEvidence" || action == "updateEvidence" || action == "verifyEvidence" || action == "reviewInterpretation") {
    // #442：忽略页面自由文本身份；落盘一律由工作台 localOperator() 写入。
    p.insert("actor_id", m_workbench->localOperator());
    p.insert("reason", m_evidenceReason->text().trimmed().isEmpty() && action == "addEvidence" ? m_evidenceText->toPlainText().trimmed() : m_evidenceReason->text().trimmed());
    p.insert("expected_revision", m_evidenceSnapshot.value("interpretation").toMap().value("revision", 0));
    if (m_workbench->selectedFeatures(selectedLayer()).size() == 1) p.insert("target_feature_uid", m_evidenceSnapshot.value("feature_uid"));
    p.insert("target_version", m_evidenceSnapshot.value("version_id"));
  }
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
                          {"texture", text(6)},
                          {"aliases", m_facies->item(i, 0) ? m_facies->item(i, 0)->data(Qt::UserRole).toMap().value("aliases") : QVariant()}};
    }
    p.insert("facies", rows);
  }
  emit commandRequested(action, p);
}

void MappingWorkbenchPage::selectLayer(const QString &id) {
  if (m_refreshPending) refresh();
  for (int i = 0; i < m_results->topLevelItemCount(); ++i) {
    auto *item = m_results->topLevelItem(i);
    if (item->data(0, Qt::UserRole).toMap().value("id").toString() == id) {
      m_results->setCurrentItem(item);
      m_results->scrollToItem(item);
      break;
    }
  }
}

void MappingWorkbenchPage::selectEvidenceSource(const QString &id) {
  if (!m_evidenceSource)
    return;
  const int index = m_evidenceSource->findData(id);
  if (index >= 0)
    m_evidenceSource->setCurrentIndex(index);
  updateState();
}
bool MappingWorkbenchPage::nativeSelectionMatchesEvidence() const {
  if (!m_workbench || m_evidenceSnapshot.contains("error") || m_evidenceSnapshot.isEmpty()) return false;
  const auto ids = m_workbench->selectedFeatures(selectedLayer());
  // This transient fid is only an owner-thread UI gate. Persisted mutations
  // still validate target_feature_uid, version and expected revision.
  return !ids.isEmpty() && m_evidenceSnapshot.value("layer_id").toString() == selectedLayer() &&
      m_evidenceSnapshot.contains("feature_id") && ids.first() == m_evidenceSnapshot.value("feature_id").toLongLong();
}
void MappingWorkbenchPage::setEvidenceSnapshot(const QVariantMap &snapshot) {
  m_evidenceSnapshotOwned = true;
  m_evidenceSnapshot = snapshot;
  updateState();
}
void MappingWorkbenchPage::setImpactAvailable(bool ready) {
  m_impactReady = ready;
  updateState();
}
void MappingWorkbenchPage::setCanGoBack(bool canBack) {
  if (m_evidenceBack)
    m_evidenceBack->setEnabled(canBack);
}
void MappingWorkbenchPage::reissueReverseQuery() {
  if (m_lastReverseKind.isEmpty())
    return;
  emit commandRequested(
      QStringLiteral("reverseQuery"),
      {{QStringLiteral("reverse_kind"), m_lastReverseKind},
       {QStringLiteral("reverse_argument"), m_lastReverseArgument}});
}

void MappingWorkbenchPage::setReverseResults(const QVariantList &rows,
                                            const QString &error,
                                            const QVariantMap &meta) {
  if (!m_reverseResults)
    return;
  m_reverseResults->clear();
  // 索引新鲜度一行如实说明：同步中/落后/失败都不静默冒充最新结果。
  const auto state = meta.value("index_state").toString();
  QString metaLine;
  if (state == QLatin1String("syncing"))
    metaLine = tr("索引状态：后台同步中，结果为已入库部分");
  else if (state == QLatin1String("stale"))
    metaLine = tr("索引状态：落后最新变更，已在后台发起同步");
  else if (state == QLatin1String("error"))
    metaLine = tr("索引状态：同步失败：%1")
                   .arg(meta.value("index_error").toString());
  else if (state == QLatin1String("fresh"))
    metaLine = tr("索引状态：已是最新");
  if (!metaLine.isEmpty() && meta.contains("total"))
    metaLine += tr("；共 %1 条，显示 %2 条")
                    .arg(meta.value("total").toInt())
                    .arg(meta.value("shown").toInt());
  if (!metaLine.isEmpty()) {
    auto *item = new QListWidgetItem(metaLine, m_reverseResults);
    item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
  }
  if (!error.isEmpty()) {
    auto *item = new QListWidgetItem(error, m_reverseResults);
    item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
    return;
  }
  for (const auto &value : rows.mid(0, 200)) {
    const auto row = value.toMap();
    // 断链历史行与旧版本行加前缀标记——它们可检索但不是有效当前关联。
    const QString marker =
        (row.value("relation_state").toString() == QLatin1String("history") ||
         row.value("unlinked").toBool())
            ? tr("[历史关联] ")
            : !row.value("is_current_version", true).toBool()
                  ? tr("[旧版本] ")
                  : QString();
    auto *item = new QListWidgetItem(
        tr("%1%2 · %3%4\n%5")
            .arg(marker,
                 row.value("horizon").toString().isEmpty()
                     ? tr("未知层位")
                     : row.value("horizon").toString(),
                 row.value("review_state").toString().isEmpty()
                     ? tr("图斑 %1…").arg(row.value("feature_uid").toString().left(12))
                     : row.value("review_state").toString(),
                 row.value("role").toString().isEmpty()
                     ? QString()
                     : QStringLiteral(" · ") + row.value("role").toString(),
                 row.value("text").toString()),
        m_reverseResults);
    item->setData(Qt::UserRole, row);
    item->setToolTip(tr("双击定位图斑（层位 %1，版本 %2…）\n来源：%3\n%4")
                         .arg(row.value("horizon").toString(),
                              row.value("version_id").toString().left(8),
                              row.value("evidence_id").toString(),
                              row.value("hit_reason").toString()));
  }
  if (rows.isEmpty())
    new QListWidgetItem(tr("无匹配结果（索引随成果保存增量更新）"), m_reverseResults);
}

void MappingWorkbenchPage::renderEvidenceSnapshot(const QVariantMap &snapshot) {
  if (m_evidenceRendered && m_renderedEvidenceSnapshot == snapshot) return;
  m_renderedEvidenceSnapshot = snapshot; m_evidenceRendered = true;
  m_evidenceSnapshot = snapshot;
  if (auto *summary = findChild<QLabel *>("faciesEvidenceSelectionSummary")) {
    summary->setText(snapshot.contains("feature_id") ? tr("%1 · 当前图斑已保存 %2 条证据").arg(snapshot.value("horizon").toString()).arg(snapshot.value("records").toList().size()) : tr("请先在地图中选中图斑，再添加解释证据。"));
    summary->setToolTip(snapshot.value("feature_uid").toString());
  }
  if (!m_interpretationState) return;
  const auto currentEvidence = m_evidence->currentItem()
      ? m_evidence->currentItem()->data(Qt::UserRole).toString() : QString();
  {
    const QSignalBlocker block(m_evidence);
    m_evidence->clear();
    for (const auto &entry : snapshot.value("records").toList().mid(0, 200)) {
      auto e = entry.toMap(); if (e.value("unlinked").toBool()) continue;
      if (snapshot.contains("project_id")) e.insert("project_id", snapshot.value("project_id"));
      if (snapshot.contains("project_session")) e.insert("project_session", snapshot.value("project_session"));
      auto *item = new QListWidgetItem(
          tr("%1 · 要素 %2 · %3\n%4")
              .arg(FaciesHierarchy::title(e.value("level").toString()),
                   e.value("feature_id", snapshot.value("feature_id")).toString(),
                   e.value("source_title", tr("手工解释")).toString(),
                   e.value("text").toString() +
                       (e.value("needs_review").toBool() ? tr("（分类已变更，需复核）") : QString())), m_evidence);
      item->setData(Qt::UserRole, e.value("id")); item->setData(Qt::UserRole + 1, e);
      item->setToolTip(tr("解释时分类：%1\n记录时间：%2").arg(
          e.value("category_path").toString(), e.value("created_at").toString()));
      if (e.value("id").toString() == currentEvidence) m_evidence->setCurrentItem(item);
    }
  }
  const auto interpretation = snapshot.value("interpretation").toMap();
  const auto identity = interpretation.value("id").toString();
  if (m_historyTarget != identity) { m_historyTarget = identity; m_historyPage = 0; }
  const int stateIndex = m_reviewState->findData(interpretation.value("status"));
  m_interpretationState->setText(tr("图斑：%1\n解释：%2 · 修订 %3").arg(
      snapshot.value("feature_uid", tr("尚未选择稳定图斑")).toString(),
      interpretation.isEmpty() ? tr("尚未登记人工解释") : m_reviewState->itemText(stateIndex),
      interpretation.value("revision", 0).toString()));
  QStringList quality;
  if (snapshot.contains("error")) quality << snapshot.value("error").toString();
  if (!snapshot.value("message").toString().isEmpty()) quality << snapshot.value("message").toString();
  for (const auto &value : snapshot.value("quality").toList()) {
    const auto finding = value.toMap();
    const auto severity = finding.value("severity").toString();
    quality << tr("%1：%2（来源 %3）").arg(severity == "Blocking" ? tr("阻断") : severity == "Error" ? tr("错误") : severity == "Warning" ? tr("警告") : tr("提示"), finding.value("message").toString(), finding.value("source_version").toString());
  }
  if (snapshot.value("details_deferred").toBool()) quality << tr("详细来源与空间核查在展开更多详情时加载；保存时仍执行完整校验。");
  m_quality->setText(quality.isEmpty() ? tr("未发现已登记证据的完整性问题；缺失证据不等于已有支持") : quality.mid(0, 30).join("\n"));
  {
    const auto selected = m_generationEvidence->currentItem() ? m_generationEvidence->currentItem()->data(Qt::UserRole).toMap().value("id") : QVariant();
    const QSignalBlocker blocker(m_generationEvidence);
    m_generationEvidence->clear();
    auto sources = snapshot.value("spatial_candidates").toList();
    sources += snapshot.value("generation_records").toList();
    for (const auto &value : sources.mid(0, 200)) {
      auto row = value.toMap();
      if (snapshot.contains("project_id")) row.insert("project_id", snapshot.value("project_id"));
      if (snapshot.contains("project_session")) row.insert("project_session", snapshot.value("project_session"));
      auto *item = new QListWidgetItem(tr("%1 · %2").arg(row.value("name", row.value("source_title", row.value("source_version"))).toString(), row.value("origin").toString() == "spatial_candidate" ? tr("空间相交候选") : tr("生成来源")), m_generationEvidence);
      item->setData(Qt::UserRole, row);
      item->setToolTip(tr("来源版本：%1\n生成血缘和空间相交不自动表示支持当前相类型\n%2").arg(row.value("source_version", row.value("version_id")).toString(), row.value("spatial_message").toString()));
      if (row.value("id") == selected) m_generationEvidence->setCurrentItem(item);
    }
  }
  renderEvidenceAttribution(snapshot);
  const auto events = interpretation.value("history").toList();
  const int pages = qMax(1, (int(events.size()) + 49) / 50);
  m_historyPage = qBound(0, m_historyPage, pages - 1);
  const int end = qMax(0, int(events.size()) - m_historyPage * 50), begin = qMax(0, end - 50);
  QStringList history;
  for (int n = end - 1; n >= begin; --n) {
    const auto e = events[n].toMap();
    const auto action = e.value("action").toString();
    const auto actionText = action == "link" ? tr("添加关联") : action == "unlink" ? tr("解除关联") :
        action == "review" ? tr("解释审核") : action == "verify_evidence" ? tr("数据核验") :
        action == "update_evidence" ? tr("修订关联") : action == "edit" ? tr("人工编辑") : tr("历史操作");
    history << tr("%1 · %2 · %3 → %4\n%5\n%6 · 版本 %7").arg(
        e.value("timestamp").toString(), e.value("actor_id").toString(), e.value("before_revision").toString(), e.value("after_revision").toString(),
        e.value("reason").toString(), actionText, e.value("version_id").toString());
  }
  m_evidenceHistory->setPlainText(history.isEmpty() ? tr("尚无人工修订记录") : tr("历史页 %1 / %2（共 %3 条）\n\n").arg(m_historyPage + 1).arg(pages).arg(events.size()) + history.join("\n\n"));
  commandButton("historyOlder")->setEnabled(m_historyPage + 1 < pages);
  commandButton("historyOlder")->setToolTip(tr("每页显示 50 条追加历史记录"));
  commandButton("historyNewer")->setEnabled(m_historyPage > 0);
  commandButton("historyNewer")->setToolTip(tr("返回较新的历史记录"));
}
