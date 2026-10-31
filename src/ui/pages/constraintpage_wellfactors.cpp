// 层：视图
// constraintpage_wellfactors — 井点因子域 TU（方向 96 自 constraintpage.cpp
// 拆出，原文行序保持）：wellFactorSection 构建（口径下拉/字段三下拉/提取与
// 维护/结果表）+ 井点因子六方法 + wellAttributes/horizons/factors 触发接线。
#include "ui/windowsizing_internal.h"
#include "constraintpage.h"
#include "constraintpage_internal.h"

#include <cmath>
#include <limits>

#include "pageshared.h"

#include "../../qgis/qgislayerservice.h"
#include "../../services/singlefactordef.h"
#include "../../workflow/workflows.h"

#include <QFileInfo>

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
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
  wellLay->setContentsMargins(0, 0, 0, 0);
  auto *sourceCaption = caption(tr("地层厚度来源"), content);
  sourceCaption->setObjectName(QStringLiteral("factorSourceCaption")); lay->addWidget(sourceCaption);
  auto *source = new QComboBox(content);
  source->setObjectName(QStringLiteral("factorSourceCombo"));
  source->setAccessibleName(tr("地层厚度来源"));
  source->addItem(tr("井分层厚度（MD，默认）"), QStringLiteral("wells"));
  source->addItem(tr("顶、底构造面相减（独立口径）"), QStringLiteral("surfaces"));
  lay->addWidget(source);
  connect(source, &QComboBox::currentIndexChanged, this, [this] {
    invalidateWellFactors(); refreshWellFactorFields(); restoreWellFactorDisplay(); updateFactorActionStates();
  });
  wellLay->addWidget(caption(tr("井点数据 · 选择字段并提取"), wellFactors));
  wellLay->addWidget(caption(tr("取值方式"), wellFactors));
  auto *factorMode = new QComboBox(wellFactors);
  factorMode->setObjectName(QStringLiteral("factorModeCombo"));
  factorMode->addItem(tr("推荐地质口径（自动）"), QStringLiteral("auto"));
  factorMode->addItem(tr("直接使用字段值"), QStringLiteral("direct"));
  factorMode->addItem(tr("比值（分子 ÷ 分母）"), QStringLiteral("ratio"));
  factorMode->setAccessibleName(tr("因子提取口径"));
  wellLay->addWidget(factorMode);
  auto *modeHint = caption(QString(), wellFactors);
  modeHint->setObjectName(QStringLiteral("wellFactorModeHint"));
  modeHint->setWordWrap(true);
  wellLay->addWidget(modeHint);
  auto *geoHint = caption(QString(), wellFactors);
  geoHint->setObjectName(QStringLiteral("wellFactorGeoHint"));
  geoHint->setWordWrap(true);
  wellLay->addWidget(geoHint);
  auto *fieldSection = new CollapsibleSection(tr("高级：技术字段映射"), wellFactors);
  fieldSection->setObjectName(QStringLiteral("wellFactorFieldMappingSection"));
  fieldSection->setExpanded(false);
  auto *fieldLay = fieldSection->containerLayout();
  for (const auto &entry : QVector<QPair<QString, QString>>{
       {QStringLiteral("factorValueFieldCombo"), tr("指标字段")},
       {QStringLiteral("factorNumeratorFieldCombo"), tr("分子字段")},
       {QStringLiteral("factorDenominatorFieldCombo"), tr("分母字段")}}) {
    auto *label = caption(entry.second, wellFactors);
    label->setObjectName(entry.first + QStringLiteral("Caption"));
    fieldLay->addWidget(label);
    auto *combo = new QComboBox(wellFactors); combo->setObjectName(entry.first);
    combo->setAccessibleName(entry.second); combo->setPlaceholderText(tr("请选择字段")); fieldLay->addWidget(combo);
    connect(combo, &QComboBox::currentIndexChanged, this, [this] {
      invalidateWellFactors();
      restoreWellFactorDisplay();
    });
  }
  wellLay->addWidget(fieldSection);
  auto *extract = new QPushButton(tr("获取单因素"), wellFactors);
  extract->setObjectName(QStringLiteral("extractWellFactorsButton"));
  auto *maintain = new QPushButton(tr("维护井点属性"), wellFactors);
  maintain->setObjectName(QStringLiteral("maintainWellFactorsButton"));
  maintain->setToolTip(tr("按当前层位维护砂厚、层厚、砂地比与其他单因素；保存后重新提取"));
  connect(maintain, &QPushButton::clicked, this, [this, horizons] { emit maintainWellFactorsRequested(horizons->currentText()); });
  auto *actions = new QHBoxLayout;
  actions->setSpacing(PaleoTheme::tokens().spacingXs);
  actions->addWidget(extract, 1);
  auto *recompute = new QPushButton(tr("重新读取并计算"), wellFactors);
  recompute->setObjectName(QStringLiteral("recomputeWellFactorsButton"));
  recompute->setToolTip(tr("忽略已保存结果，按当前井资料重新计算并另存新版本"));
  actions->addWidget(recompute);
  actions->addWidget(maintain);
  wellLay->addLayout(actions);
  auto *detailSection = new CollapsibleSection(tr("查看逐井提取结果"), wellFactors);
  detailSection->setObjectName(QStringLiteral("wellFactorDetailsSection"));
  detailSection->setExpanded(false);
  auto *wellTable = new QTableWidget(0, 3, wellFactors);
  wellTable->setObjectName(QStringLiteral("wellFactorTable"));
  wellTable->setAccessibleName(tr("井点因子提取结果"));
  wellTable->setHorizontalHeaderLabels({tr("井名"), tr("因子值"), tr("来源或缺失原因")});
  wellTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
  wellTable->verticalHeader()->hide(); wellTable->horizontalHeader()->setStretchLastSection(true);
  wellTable->setMinimumHeight(paleo::ui_detail::tableHeightForRows(wellTable, 4)); // R6 #842：按行高，不用裸像素
  detailSection->containerLayout()->addWidget(wellTable);
  auto *wellHint = new QLabel(tr("优先读取已保存井值；缺少或来源变化时重新计算并保存。"), wellFactors);
  wellHint->setObjectName(QStringLiteral("wellFactorHint")); wellHint->setWordWrap(true);
  PaleoTheme::applyThemedStyleSheet(wellHint, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
  wellLay->addWidget(wellHint);
  wellLay->addWidget(detailSection);
  lay->addWidget(wellFactors);
  const auto modeChanged = [this, factorMode, modeHint] {
    const QString mode = factorMode->currentData().toString();
    const bool ratio = mode == QLatin1String("ratio");
    const bool automatic = mode == QLatin1String("auto");
    modeHint->setText(automatic ? tr("按当前因素的地质规则取值：优先工程已维护字段，否则按同层段 MD 规则计算；技术字段可在下方展开。")
                       : ratio  ? tr("逐井计算分子 ÷ 分母；分母为 0 或缺值的井不参与。厚度比值请使用同一 MD 层段。")
                                : tr("每口井读取一个数值字段。没有合适的字段时，可先维护井点属性。"));
    if (auto *fields = findChild<QWidget *>(QStringLiteral("wellFactorFieldMappingSection")))
      fields->setVisible(!automatic);
    for (const QString &name : {QStringLiteral("factorValueFieldCombo"), QStringLiteral("factorNumeratorFieldCombo"),
                               QStringLiteral("factorDenominatorFieldCombo")}) {
      const bool visible = !automatic && ((name == QLatin1String("factorValueFieldCombo")) != ratio);
      findChild<QWidget *>(name)->setVisible(visible);
      findChild<QWidget *>(name + QStringLiteral("Caption"))->setVisible(visible);
    }
    refreshWellFactorGeoHint();
    invalidateWellFactors();
    restoreWellFactorDisplay();
  };
  connect(factorMode, &QComboBox::currentIndexChanged, this, modeChanged); modeChanged();
  connect(extract, &QPushButton::clicked, this, [this, factors, horizons] {
    const int r = checkedRow(factors); if (r < 0) return;
    emit extractWellFactorsRequested(factorIdOfRow(factors, r), horizons->currentText(), wellFactorParams());
  });
  connect(recompute, &QPushButton::clicked, this, [this, factors, horizons] {
    const int row = checkedRow(factors); if (row < 0) return;
    auto params = wellFactorParams(); params.insert(QStringLiteral("forceRefresh"), true);
    emit extractWellFactorsRequested(factorIdOfRow(factors, row), horizons->currentText(), params);
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
                // D-13（full-audit-20261020）：行内容归属键——提取完成时表内
                // 行即属（层位, 因子），建议函数据此校验归属。
                setProperty("paleo.page.wellRowsKey", h + QChar(0x1f) + f);
                refreshWellFactorResults();
                refreshFactorResults();
                updateFactorActionStates();
              }
            });
  }
  connect(horizons, &QComboBox::currentIndexChanged, this, [this] {
    invalidateWellFactors();
    refreshWellFactorFields();
    // 构造期已勾上第一行，层位随后才到。口径下拉若没换索引就不会再发信号，
    // 已保存提取必须在这里恢复，不能只靠取值方式的 currentIndexChanged。
    restoreWellFactorDisplay();
    applySuggestedContourInterval();
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
  const auto value = [this](const char *name) { auto *c = findChild<QComboBox *>(QString::fromLatin1(name)); return c ? c->currentData().toString() : QString(); };
  QVariantMap params{{QStringLiteral("factorMode"), value("factorModeCombo")},
          {QStringLiteral("valueField"), value("factorValueFieldCombo")},
          {QStringLiteral("numeratorField"), value("factorNumeratorFieldCombo")},
          {QStringLiteral("denominatorField"), value("factorDenominatorFieldCombo")}};
  const auto *table = findChild<QTableWidget *>(QStringLiteral("factorTable"));
  if (factorIdOfRow(table, checkedRow(table)) == QLatin1String("strathick"))
    params.insert(QStringLiteral("factorSource"), value("factorSourceCombo"));
  if (auto *selection=qobject_cast<SelectionContext *>(property("paleo.selection").value<QObject *>()))
    params.insert(QStringLiteral("unitSelection"),paleo::strat::toMap(selection->activeUnit()));
  return params;
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
  restoreWellFactorDisplay();
  updateFactorActionStates();
}
void ConstraintPage::setWellFactorFields(const QVariantList &fields) {
  setProperty("paleo.page.fieldsbound", true);
  QVariantMap old = wellFactorParams();
  if (auto *table = child<QTableWidget>(this, "factorTable")) {
    const QSignalBlocker blocker(table);
    QStringList names; for (const auto &field : fields) names << field.toMap().value("id").toString().toLower();
    for (int row = 0; row < table->rowCount(); ++row) {
      const auto id = factorIdOfRow(table, row);
      const bool hidden = (id == "poro" && !names.contains("poro") && !names.contains("porosity") && !names.contains("phie")) ||
                          (id == "perm" && !names.contains("perm") && !names.contains("permeability"));
      table->setRowHidden(row, hidden);
      if (hidden && table->item(row, 0)->checkState() == Qt::Checked) table->item(row, 0)->setCheckState(Qt::Unchecked);
    }
    table->setFixedHeight(paleo::ui_detail::tableHeightForRows(table) + PaleoTheme::tokens().spacingXs); // R6 #842：与构造同口径
  }
  auto *factors = child<QTableWidget>(this, "factorTable");
  const QString factor = factorIdOfRow(factors, checkedRow(factors));
  const auto *horizon = child<QComboBox>(this, "horizonCombo");
  const QString horizonText = horizon ? horizon->currentText() : QString();
  const bool changedFactor = property("paleo.page.extractionFactor").toString() != factor;
  // 构造期会先勾上第一行，此时层位还是空的，探测被跳过并把口径锁成「自动」。
  // 层位随后到达时因素没变，必须再探测，否则已保存的 direct 提取被藏起来。
  const bool changedHorizon = property("paleo.page.extractionHorizon").toString() != horizonText;
  if (changedFactor) old.remove(QStringLiteral("valueField"));
  setProperty("paleo.page.extractionFactor", factor);
  setProperty("paleo.page.extractionHorizon", horizonText);
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
      if (r >= 0) selected = factorDefinition(factorIdOfRow(table, r)).defaultParams.value(QStringLiteral("field")).toString();
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
  if (changedFactor || changedHorizon) {
    auto *mode = child<QComboBox>(this, "factorModeCombo");
    bool known = false;
    const auto profile = SingleFactorRegistry::geologicalProfile(factor, &known, wellFactorParams().value("factorSource").toString());
    const bool direct = child<QComboBox>(this, "factorValueFieldCombo")->currentIndex() >= 0;
    const bool ratioAvailable = child<QComboBox>(this, "factorNumeratorFieldCombo")->currentIndex() >= 0 && child<QComboBox>(this, "factorDenominatorFieldCombo")->currentIndex() >= 0;
    const QString fieldMode = factor == QLatin1String("sandratio") && !direct && ratioAvailable ? QStringLiteral("ratio") : QStringLiteral("direct");
    // 已有按字段口径保存的提取版本时沿用该口径（不让默认推荐口径把已保存成果藏起来）；
    // 否则对支持的因素默认推荐地质口径。
    bool savedFieldMode = false;
    if (auto *wf = qobject_cast<ConstraintWorkflow *>(property(kWfProp).value<QObject *>());
        wf && horizon && !horizon->currentText().isEmpty())
    {
      QVariantMap probe = wellFactorParams();
      probe.insert(QStringLiteral("factorMode"), fieldMode);
      savedFieldMode = wf->restoreWellFactors(horizon->currentText(), factor, probe);
    }
    mode->setCurrentIndex(mode->findData(known && profile.supportsAutoMode && !savedFieldMode ? QStringLiteral("auto") : fieldMode));
  }
  updateFactorActionStates();
}
void ConstraintPage::invalidateWellFactors() {
  if (auto *table = findChild<QTableWidget *>(QStringLiteral("wellFactorTable"))) table->setRowCount(0);
  setProperty("paleo.page.wellRowsKey", QString()); // D-13：清表即弃行归属
  if (auto *hint = findChild<QLabel *>(QStringLiteral("wellFactorHint")))
    hint->setText(tr("优先读取已保存井值；缺少或来源变化时重新计算并保存。"));
  markInputsStale(); updateWellFactorActionState();
  updateFactorActionStates();
}
void ConstraintPage::refreshAnalysisUnit() {
  invalidateWellFactors();
  restoreWellFactorDisplay();
  refreshFactorLayers();
  refreshFactorResults();
}
void ConstraintPage::refreshWellFactorGeoHint()
{
  auto *hint = findChild<QLabel *>(QStringLiteral("wellFactorGeoHint"));
  if (!hint)
    return;
  const int row = checkedRow(child<QTableWidget>(this, "factorTable"));
  if (row < 0)
  {
    hint->setText(tr("勾选因素后显示单位、有效域与推荐取值规则。"));
    return;
  }
  bool known = false;
  const auto profile = SingleFactorRegistry::geologicalProfile(factorIdOfRow(child<QTableWidget>(this, "factorTable"), row), &known,
                                                              wellFactorParams().value("factorSource").toString());
  if (!known)
  {
    hint->clear();
    return;
  }
  QString text = tr("%1 · 单位 %2 · 有效域 %3").arg(profile.ruleSentence, profile.unit, profile.validRange);
  if (!profile.note.isEmpty())
    text += QStringLiteral("\n") + profile.note;
  if (!profile.wellPointExtraction)
    text += QStringLiteral("\n") + tr("本因素不走井点提取，请使用其专属计算入口。");
  hint->setText(text);
}

void ConstraintPage::updateWellFactorActionState() {
  auto *button = child<QPushButton>(this, "extractWellFactorsButton"); if (!button) return;
  const auto params = wellFactorParams();
  const QString mode = params.value(QStringLiteral("factorMode")).toString();
  const bool ratio = mode == QLatin1String("ratio");
  const bool automatic = mode == QLatin1String("auto");
  const bool ready = checkedRow(child<QTableWidget>(this, "factorTable")) >= 0 &&
      (automatic || (ratio ? !params.value(QStringLiteral("numeratorField")).toString().isEmpty() && !params.value(QStringLiteral("denominatorField")).toString().isEmpty()
                           : !params.value(QStringLiteral("valueField")).toString().isEmpty()));
  const bool busy = property("paleo.page.runbusy").toBool();
  const auto *horizon = child<QComboBox>(this, "horizonCombo");
  const bool hasHorizon = horizon && !horizon->currentText().trimmed().isEmpty();
  const auto *factors = child<QTableWidget>(this, "factorTable");
  const bool fromWells = interpolantEngine(factorDefinition(factorIdOfRow(factors, checkedRow(factors))).processingAlgId);
  // 井点提取是输入准备，与约束线相互独立（约束只在插值时汇合）。
  button->setVisible(fromWells);
  button->setEnabled(fromWells && ready && hasHorizon && !busy);
  button->setToolTip(busy          ? tr("正在计算，可取消")
                     : !hasHorizon ? tr("输入：请先打开工程并选择层位")
                     : ready       ? tr("优先读取本层位已保存的单因素；缺少结果或来源变化时从井资料读取、计算并保存，供下次使用")
                                   : tr("请勾选因子并选择真实提取字段"));
  if (auto *recompute = child<QPushButton>(this, "recomputeWellFactorsButton"))
    recompute->setEnabled(button->isEnabled());
  refreshWellFactorGeoHint();
  updateOneClickState();
}
void ConstraintPage::refreshWellFactorResults() {
  suggestFactorCellSize();
  auto *wf = qobject_cast<ConstraintWorkflow *>(property(kWfProp).value<QObject *>());
  auto *table = child<QTableWidget>(this, "wellFactorTable"); if (!wf || !table) return;
  table->setRowCount(0);
  const auto params = wellFactorParams();
  table->setHorizontalHeaderItem(1, new QTableWidgetItem(params.value(QStringLiteral("factorMode")) == QStringLiteral("ratio") ? tr("比值（分子 ÷ 分母）") : tr("因子值")));
  for (const QVariant &v : wf->wellFactorRows()) {
    const auto m = v.toMap(); const int row = table->rowCount(); table->insertRow(row);
    table->setItem(row, 0, new QTableWidgetItem(m.value(QStringLiteral("well_name")).toString()));
    auto *value = new QTableWidgetItem(m.contains(QStringLiteral("value")) ? QString::number(m.value(QStringLiteral("value")).toDouble(), 'g', 8) : QStringLiteral("—"));
    value->setFont(PaleoTheme::monoFont()); table->setItem(row, 1, value);
    value->setData(Qt::UserRole, m.value(QStringLiteral("contributing")));
    auto *detail = new QTableWidgetItem(m.value(QStringLiteral("contributing")).toBool() ? m.value(QStringLiteral("source")).toString() : m.value(QStringLiteral("reason")).toString());
    detail->setToolTip(detail->text()); table->setItem(row, 2, detail);
  }
  child<QLabel>(this, "wellFactorHint")->setText(wf->wellFactorMessage());
  applySuggestedContourInterval();
  refreshFactorDefaultsHint();
  updateFactorActionStates();
}

void ConstraintPage::applySuggestedContourInterval()
{
  auto *spin = child<QDoubleSpinBox>(this, "contourIntervalSpin");
  auto *factors = child<QTableWidget>(this, "factorTable");
  if (!spin || !factors)
    return;
  const QString factor = factorIdOfRow(factors, checkedRow(factors));
  if (factor.isEmpty())
    return;
  auto *horizons = child<QComboBox>(this, "horizonCombo");
  const QString key = (horizons ? horizons->currentText() : QString()) + QChar(0x1f) + factor;
  // 手改过的等值距跟层位和因素走，不因键变化被清掉。没改过时下面仍用新极差建议替换默认值。
  if (property("paleo.page.contourIntervalUser").toBool())
  {
    // 方向04 D9：保留手改值，但 0（structural 专用「自动」态）带到非 structural
    // 因素上会被服务层拒绝——只把这个非法残留复位成正默认值。
    if (spin->value() == 0.0 && !checkedFactorIsStructural())
    {
      QSignalBlocker block(spin);
      spin->setValue(20.0);
    }
    return;
  }
  // D-13（full-audit-20261020）：井点行归属守卫。换勾瞬间勾选互斥 handler 先于
  // 清表 handler 执行，表内仍是上一因子的提取行——workflow 在场时行内容必须
  // 归属当前（层位, 因子）才参与建议；不归属 → count 视 0（走既有 step<=0
  // 分支：残留 0 回 20 / 保留用户值），不再用跨因素旧值域伪造建议。
  // wf==nullptr（测试壳）保持旧行为，既有四断言不动。
  auto *wf = qobject_cast<ConstraintWorkflow *>(property(kWfProp).value<QObject *>());
  const bool rowsBelongToFactor =
      !wf || property("paleo.page.wellRowsKey").toString() == key;

  double lo = std::numeric_limits<double>::infinity();
  double hi = -lo;
  int count = 0;
  if (rowsBelongToFactor) {
    if (auto *table = child<QTableWidget>(this, "wellFactorTable")) {
      for (int row = 0; row < table->rowCount(); ++row) {
        const auto *item = table->item(row, 1);
        if (!item || (item->data(Qt::UserRole).isValid() && !item->data(Qt::UserRole).toBool()))
          continue;
        bool ok = false;
        const double value = item->text().toDouble(&ok);
        if (!ok || !std::isfinite(value))
          continue;
        lo = std::min(lo, value);
        hi = std::max(hi, value);
        ++count;
      }
    }
  }
  const auto defaults = SingleFactorRegistry::mapDefaults(factor, lo, hi);
  const double step = defaults.hasRange && (count > 0 || factor == QLatin1String("sandratio")) ? defaults.contourInterval : 0.0;
  QSignalBlocker block(spin);
  if (!(step > 0.0)) {
    if (spin->value() == 0.0 && !checkedFactorIsStructural())
      spin->setValue(20.0);
    // R2-1/MF-1（full-audit-20261020）：无可用建议时复位默认 tooltip——
    // 上一因素的「（旧极差）」文案不得冒充当前因素的数值依据残留。
    spin->setToolTip(tr("默认约为当前单因素有效值极差的 1/6，可再改。"));
    setProperty("paleo.page.contourIntervalKey", key);
    return;
  }
  int decimals = 2;
  for (double scaled = step; decimals < 6 && scaled < 1.0 - 1e-12; scaled *= 10.0)
    ++decimals;
  spin->setDecimals(std::max(2, decimals - 1));
  spin->setValue(step);
  spin->setToolTip(tr("按当前因素推荐值域 %1–%2 %3 设置，可调整。").arg(defaults.minimum, 0, 'g', 6)
                       .arg(defaults.maximum, 0, 'g', 6).arg(defaults.unit));
  setProperty("paleo.page.contourIntervalKey", key);
  setProperty("paleo.page.contourIntervalUser", false);
}
void ConstraintPage::restoreWellFactorDisplay() {
  auto *wf = qobject_cast<ConstraintWorkflow *>(property(kWfProp).value<QObject *>());
  auto *h = child<QComboBox>(this, "horizonCombo");
  auto *factors = child<QTableWidget>(this, "factorTable");
  if (!wf || !h || !factors)
    return;
  const QString factor = factorIdOfRow(factors, checkedRow(factors));
  if (factor.isEmpty())
    return;
  QString restoreError;
  if (wf->restoreWellFactors(h->currentText(), factor, wellFactorParams(), &restoreError)) {
    // D-13：恢复出的行属（层位, 因子）——写入归属键供建议函数校验。
    setProperty("paleo.page.wellRowsKey", h->currentText() + QChar(0x1f) + factor);
    refreshWellFactorResults();
  } else if (!restoreError.isEmpty()) {
    // #811-2：恢复失败（如井点因子图层声明失败）如实显示，不静默留在旧表。
    if (auto *hint = findChild<QLabel *>(QStringLiteral("wellFactorHint")))
      hint->setText(restoreError);
  }
}
