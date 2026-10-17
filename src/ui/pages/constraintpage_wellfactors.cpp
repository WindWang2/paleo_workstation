// 层：视图
// constraintpage_wellfactors — 井点因子域 TU（方向 96 自 constraintpage.cpp
// 拆出，原文行序保持）：wellFactorSection 构建（口径下拉/字段三下拉/提取与
// 维护/结果表）+ 井点因子六方法 + wellAttributes/horizons/factors 触发接线。
#include "constraintpage.h"
#include "constraintpage_internal.h"

#include "pageshared.h"

#include "../../qgis/qgislayerservice.h"
#include "../../services/singlefactordef.h"
#include "../../workflow/workflows.h"

#include <QFileInfo>

#include <QComboBox>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

using namespace paleo::pagesinternal;
using namespace paleo::constraintpage_internal;

void ConstraintPage::buildWellFactorSection( QWidget *content, QVBoxLayout *lay, QComboBox *horizons,
                                             QTableWidget *factors )
{
  auto *wellFactors = new QWidget(content);
  wellFactors->setObjectName(QStringLiteral("wellFactorSection"));
  auto *wellLay = panelLayout(wellFactors);
  wellLay->addWidget(caption(tr("砂层厚度、砂地比等井点数据"), wellFactors));
  // 口径和字段不再给用户选。控件留着是为了旧测试还能找到对象；界面上只留「提取」。
  auto *factorMode = new QComboBox(wellFactors);
  factorMode->setObjectName(QStringLiteral("factorModeCombo"));
  factorMode->addItem(tr("直读字段"), QStringLiteral("direct"));
  factorMode->addItem(tr("比值（分子 ÷ 分母）"), QStringLiteral("ratio"));
  factorMode->hide();
  for (const auto &entry : QVector<QPair<QString, QString>>{
       {QStringLiteral("factorValueFieldCombo"), tr("指标字段")},
       {QStringLiteral("factorNumeratorFieldCombo"), tr("分子字段")},
       {QStringLiteral("factorDenominatorFieldCombo"), tr("分母字段")}}) {
    auto *label = caption(entry.second, wellFactors);
    label->setObjectName(entry.first + QStringLiteral("Caption"));
    label->hide();
    auto *combo = new QComboBox(wellFactors); combo->setObjectName(entry.first);
    combo->setAccessibleName(entry.second); combo->hide();
  }
  auto *extract = new QPushButton(tr("提取"), wellFactors);
  extract->setObjectName(QStringLiteral("extractWellFactorsButton"));
  auto *maintain = new QPushButton(tr("维护井点属性"), wellFactors);
  maintain->setObjectName(QStringLiteral("maintainWellFactorsButton"));
  maintain->setToolTip(tr("按当前层位维护砂厚、层厚、砂地比与其他单因素；保存后重新提取"));
  wellLay->addWidget(maintain);
  connect(maintain, &QPushButton::clicked, this, [this, horizons] { emit maintainWellFactorsRequested(horizons->currentText()); });
  wellLay->addWidget(extract);
  auto *wellTable = new QTableWidget(0, 3, wellFactors);
  wellTable->setObjectName(QStringLiteral("wellFactorTable"));
  wellTable->setAccessibleName(tr("井点因子提取结果"));
  wellTable->setHorizontalHeaderLabels({tr("井名"), tr("因子值"), tr("来源或缺失原因")});
  wellTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
  wellTable->verticalHeader()->hide(); wellTable->horizontalHeader()->setStretchLastSection(true);
  wellLay->addWidget(wellTable);
  auto *wellHint = new QLabel(tr("勾选一个单因素后点「提取」。"), wellFactors);
  wellHint->setObjectName(QStringLiteral("wellFactorHint")); wellHint->setWordWrap(true);
  PaleoTheme::applyThemedStyleSheet(wellHint, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
  wellLay->addWidget(wellHint); lay->addWidget(wellFactors);
  connect(extract, &QPushButton::clicked, this, [this, factors, horizons] {
    const int r = checkedRow(factors); if (r < 0) return;
    emit extractWellFactorsRequested(factorIdOfRow(factors, r), horizons->currentText(), wellFactorParams());
  });
}

void ConstraintPage::wireWellFactorTriggers( ConstraintWorkflow *wf, QComboBox *horizons, QTableWidget *factors )
{
  if (wf) {
    connect(wf, &ConstraintWorkflow::wellAttributesChanged, this, [this] {
      // U10：数据世代 +1——字段缓存键随之失效，下一次刷新重取清单。
      setProperty("paleo.page.fieldsgen", property("paleo.page.fieldsgen").toInt() + 1);
      invalidateWellFactors();
      refreshWellFactorFields();
    });
    connect(wf, &ConstraintWorkflow::wellFactorsExtracted, this,
            [this, horizons, factors](const QString &h, const QString &f) {
              if (h == horizons->currentText() &&
                  f == factorIdOfRow(factors, checkedRow(factors))) {
                refreshWellFactorResults();
                refreshFactorResults();
                updateFactorActionStates();
              }
            });
  }
  connect(horizons, &QComboBox::currentIndexChanged, this, [this] {
    invalidateWellFactors();
    refreshWellFactorFields();
    refreshFactorResults();
  });
  connect(factors, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) {
    if (item && item->column() == 0) {
      invalidateWellFactors();
      refreshWellFactorFields();
      refreshFactorResults();
    }
  });
}

QVariantMap ConstraintPage::wellFactorParams() const {
  return {{QStringLiteral("factorMode"), QStringLiteral("auto")}};
}
void ConstraintPage::refreshWellFactorFields() {
  auto *wf = qobject_cast<ConstraintWorkflow *>(property(kWfProp).value<QObject *>());
  auto *h = child<QComboBox>(this, "horizonCombo");
  QString error;
  // U10：字段清单只依赖（层位, 数据声明状态），与因子勾选无关——原实现每次
  // 勾选都在 GUI 线程全要素扫描井点图层 + 双表读取。缓存键 = 层位 +
  // wellAttributesChanged 世代 + 井点图层声明集合；勾选切换只失效因子值。
  // setWellFactorFields 每次仍执行（默认选中逻辑不缓存）。
  // 只读观测计数 paleo.page.fieldrefreshcount = 真实取数（缓存未命中）次数。
  QVariantList fields;
  if (wf) {
    // U10/S5-Med：字段清单依赖井点图层的要素属性内容（wellFactorFields 对
    // 每个值 parseNumeric 才收录字段名）——缓存键除声明集合外须含各 wells
    // 声明 source 文件的 size+mtime，就地编辑（属性表加数值列/填数值）后
    // 下一次刷新即失效，不靠切层位/重声明自愈。
    QStringList wellsDecls;
    if (auto *svc = qobject_cast<QgisLayerService *>(property(kLayersProp).value<QObject *>()))
      for (const LayerDeclaration &d : svc->declared())
        if (d.layerId.startsWith(QStringLiteral("wells"))) {
          const QFileInfo src(d.source.section(QLatin1Char('|'), 0, 0));
          wellsDecls << d.layerId + QChar(0x1f) + d.source + QChar(0x1f) +
                          QString::number(src.size()) + QChar(0x1f) +
                          QString::number(src.lastModified().toMSecsSinceEpoch());
        }
    const QString cacheKey = (h ? h->currentText() : QString()) + QChar(0x1f) +
                             QString::number(property("paleo.page.fieldsgen").toInt()) + QChar(0x1f) +
                             wellsDecls.join(QChar(0x1e));
    if (property("paleo.page.fieldskey").toString() == cacheKey) {
      fields = property("paleo.page.fieldscache").toList();
    } else {
      fields = wf->wellFactorFields(h ? h->currentText() : QString(), &error);
      setProperty("paleo.page.fieldskey", cacheKey);
      setProperty("paleo.page.fieldscache", fields);
      setProperty("paleo.page.fieldrefreshcount", property("paleo.page.fieldrefreshcount").toInt() + 1);
    }
  }
  if (!error.isEmpty()) child<QLabel>(this, "wellFactorHint")->setText(error);
  setWellFactorFields(fields);
  setProperty("paleo.page.fieldsbound", wf != nullptr);
  if (wf && h)
    wf->restoreWellFactors(h->currentText(),
                           factorIdOfRow(child<QTableWidget>(this, "factorTable"),
                                         checkedRow(child<QTableWidget>(this, "factorTable"))),
                           wellFactorParams());
  if (wf && h && !wf->wellFactorRows().isEmpty())
    refreshWellFactorResults();
  updateFactorActionStates();
}
void ConstraintPage::setWellFactorFields(const QVariantList &fields) {
  setProperty("paleo.page.fieldsbound", true);
  QVariantMap old = wellFactorParams();
  auto *factors = child<QTableWidget>(this, "factorTable");
  const QString factor = factorIdOfRow(factors, checkedRow(factors));
  const bool changedFactor = property("paleo.page.extractionFactor").toString() != factor;
  if (changedFactor) old.remove(QStringLiteral("valueField"));
  setProperty("paleo.page.extractionFactor", factor);
  for (const auto &entry : QVector<QPair<QString, QString>>{
       {QStringLiteral("factorValueFieldCombo"), QStringLiteral("valueField")},
       {QStringLiteral("factorNumeratorFieldCombo"), QStringLiteral("numeratorField")},
       {QStringLiteral("factorDenominatorFieldCombo"), QStringLiteral("denominatorField")}}) {
    auto *c = findChild<QComboBox *>(entry.first); if (!c) continue;
    c->blockSignals(true); c->clear();
    for (const QVariant &v : fields) { const auto m = v.toMap(); c->addItem(m.value(QStringLiteral("label")).toString(), m.value(QStringLiteral("id"))); }
    QString selected = old.value(entry.second).toString();
    if (selected.isEmpty() && entry.second == QLatin1String("valueField")) {
      const auto *table = child<QTableWidget>(this, "factorTable"); const int r = checkedRow(table);
      if (r >= 0) selected = SingleFactorRegistry::byId(factorIdOfRow(table, r)).defaultParams.value(QStringLiteral("field")).toString();
    }
    if (selected.isEmpty() && entry.second == QLatin1String("numeratorField")) selected = QStringLiteral("log_sand_thickness_md");
    if (selected.isEmpty() && entry.second == QLatin1String("denominatorField")) selected = QStringLiteral("log_layer_thickness_md");
    int index = c->findData(selected);
    if (index < 0 && entry.second == QLatin1String("valueField")) {
      if (factor == QLatin1String("sandthick")) index = c->findData(QStringLiteral("log_sand_thickness_md"));
      if (factor == QLatin1String("strathick")) index = c->findData(QStringLiteral("log_layer_thickness_md"));
    }
    c->setCurrentIndex(index); c->blockSignals(false);
  }
  if (changedFactor) {
    const bool direct = child<QComboBox>(this, "factorValueFieldCombo")->currentIndex() >= 0;
    auto *mode = child<QComboBox>(this, "factorModeCombo");
    const bool ratioAvailable = child<QComboBox>(this, "factorNumeratorFieldCombo")->currentIndex() >= 0 && child<QComboBox>(this, "factorDenominatorFieldCombo")->currentIndex() >= 0;
    mode->setCurrentIndex(mode->findData(factor == QLatin1String("sandratio") && !direct && ratioAvailable ? QStringLiteral("ratio") : QStringLiteral("direct")));
  }
  updateFactorActionStates();
}
void ConstraintPage::invalidateWellFactors() {
  if (auto *table = findChild<QTableWidget *>(QStringLiteral("wellFactorTable"))) table->setRowCount(0);
  if (auto *hint = findChild<QLabel *>(QStringLiteral("wellFactorHint")))
    hint->setText(tr("尚未提取。点「提取」后，能读的直接用，砂地比可按砂岩厚度÷层厚计算，没有数据的井会写明原因。"));
  markInputsStale(); updateWellFactorActionState();
  updateFactorActionStates();
}
void ConstraintPage::updateWellFactorActionState() {
  auto *button = child<QPushButton>(this, "extractWellFactorsButton"); if (!button) return;
  const bool ready = checkedRow(child<QTableWidget>(this, "factorTable")) >= 0;
  const bool busy = property("paleo.page.runbusy").toBool();
  const bool linesReady = property("paleo.page.constraintsready").toBool();
  button->setEnabled(ready && !busy && linesReady);
  button->setToolTip(busy          ? tr("正在计算，可取消")
                     : !linesReady ? tr("步骤 1：先绘制或导入本层位的间隔线和方向线")
                     : ready       ? tr("提取当前单因素。有井点值就读取，砂地比没有现成值就用砂厚除以层厚，没有数据的井写明原因。")
                                   : tr("请先勾选一个单因素"));
}
void ConstraintPage::refreshWellFactorResults() {
  auto *wf = qobject_cast<ConstraintWorkflow *>(property(kWfProp).value<QObject *>());
  auto *table = child<QTableWidget>(this, "wellFactorTable"); if (!wf || !table) return;
  table->setRowCount(0);
  table->setHorizontalHeaderItem(1, new QTableWidgetItem(tr("因子值")));
  for (const QVariant &v : wf->wellFactorRows()) {
    const auto m = v.toMap(); const int row = table->rowCount(); table->insertRow(row);
    table->setItem(row, 0, new QTableWidgetItem(m.value(QStringLiteral("well_name")).toString()));
    auto *value = new QTableWidgetItem(m.contains(QStringLiteral("value")) ? QString::number(m.value(QStringLiteral("value")).toDouble(), 'g', 8) : QStringLiteral("—"));
    value->setFont(PaleoTheme::monoFont()); table->setItem(row, 1, value);
    auto *detail = new QTableWidgetItem(m.value(QStringLiteral("contributing")).toBool() ? m.value(QStringLiteral("source")).toString() : m.value(QStringLiteral("reason")).toString());
    detail->setToolTip(detail->text()); table->setItem(row, 2, detail);
  }
  child<QLabel>(this, "wellFactorHint")->setText(wf->wellFactorMessage());
  updateFactorActionStates();
}
