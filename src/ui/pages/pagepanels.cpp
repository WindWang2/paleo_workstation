#include "pagepanels.h"

#include "../../catalog/datacatalog.h"
#include "../../io/dataimportservice.h"
#include "../datapreview/datapreviewtabs.h"
#include "../../workflow/workflows.h"    // signal names + ValidationWorkflow::validate
#include "../../qgis/qgislayerservice.h" // declared() — forward-declares Qgs*, none included
#include "../../metadata/layermanifest.h" // LayerDeclaration fields
#include "../../domain/types.h"          // ValidationIssue fields

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

// ---------------------------------------------------------------------------
// pagepanels.h fixes the public shape of these classes and declares no data
// members, so service bindings ride on dynamic QObject properties (same idiom
// as workflows.cpp) and child widgets are located by objectName.
// ---------------------------------------------------------------------------
namespace
{
  const char kLayersProp[] = "paleo.page.layers"; // QObject* (QgisLayerService)
  const char kWfProp[]     = "paleo.page.wf";     // QObject* (page workflow)

  // DESIGN.md `label` token: 8pt muted captions mark panel groups.
  QLabel *caption(const QString &text, QWidget *parent)
  {
    auto *l = new QLabel(text, parent);
    QFont f = l->font();
    f.setPointSize(8);
    l->setFont(f);
    l->setStyleSheet(QStringLiteral("color: #5D6E80;")); // text-muted
    return l;
  }

  QVBoxLayout *panelLayout(QWidget *page)
  {
    auto *lay = new QVBoxLayout(page);
    lay->setContentsMargins(8, 8, 8, 8); // spacing.sm
    lay->setSpacing(8);
    return lay;
  }

  template <typename T>
  T *child(QObject *root, const char *name)
  {
    return root->findChild<T *>(QLatin1String(name));
  }

  // §42.4: an empty asset table shows a guidance row — never a blank panel.
  void refreshAssetEmptyState(QTableWidget *t)
  {
    if (t->rowCount() > 0)
      return;
    t->insertRow(0);
    auto *it = new QTableWidgetItem(
        DataPage::tr("还没有数据资产 — 通过上方「数据导入」添加井、地震或边界数据"));
    it->setFlags(Qt::NoItemFlags);
    it->setForeground(QColor(QStringLiteral("#5D6E80"))); // text-muted
    t->setItem(0, 0, it);
    t->setSpan(0, 0, 1, t->columnCount());
  }

  QString severityText(ValidationIssue::Severity s)
  {
    switch (s)
    {
      case ValidationIssue::Info:    return ValidatePage::tr("信息");
      case ValidationIssue::Error:   return ValidatePage::tr("错误");
      case ValidationIssue::Warning: // fall through
      default:                       return ValidatePage::tr("警告");
    }
  }

  // Semantic colors pair with the severity text, never stand alone (§42.16).
  QColor severityColor(ValidationIssue::Severity s)
  {
    switch (s)
    {
      case ValidationIssue::Info:  return QColor(QStringLiteral("#1B73D0")); // primary
      case ValidationIssue::Error: return QColor(QStringLiteral("#E53935")); // error
      default:                     return QColor(QStringLiteral("#F29900")); // warning
    }
  }
} // namespace

// ---------------------------------------------------------------------------
// DataPage — 数据管理
// ---------------------------------------------------------------------------
DataPage::DataPage(QWidget *parent)
  : QWidget(parent)
{
  auto *lay = panelLayout(this);

  lay->addWidget(caption(tr("数据导入"), this));
  const struct { const char *name; const char *text; const char *kind; } kImports[] = {
    {"importWells", QT_TR_NOOP("导入井数据"), "wells"},
    {"importSeismic", QT_TR_NOOP("导入地震数据"), "seismic"},
    {"importBoundary", QT_TR_NOOP("导入边界数据"), "boundary"},
  };
  for (const auto &spec : kImports)
  {
    auto *btn = new QPushButton(tr(spec.text), this);
    btn->setObjectName(QLatin1String(spec.name));
    connect(btn, &QPushButton::clicked, this,
            [this, kind = QLatin1String(spec.kind)] { emit importRequested(kind); });
    lay->addWidget(btn);
  }

  lay->addSpacing(16); // spacing.md between groups
  lay->addWidget(caption(tr("资产"), this));
  auto *table = new QTableWidget(0, 3, this);
  table->setObjectName(QStringLiteral("assetTable"));
  table->setAccessibleName(tr("资产列表"));
  table->setHorizontalHeaderLabels({tr("名称"), tr("类型"), tr("来源")});
  table->verticalHeader()->setVisible(false);
  table->horizontalHeader()->setStretchLastSection(true);
  refreshAssetEmptyState(table);
  lay->addWidget(table, 1);

  // §4 页内预览标签（普通 QTabWidget，dock 面板样式；只在数据管理页出现）。
  auto *preview = new DataPreviewTabs(this);
  preview->setObjectName(QStringLiteral("dataPreview"));
  lay->addWidget(caption(tr("预览"), this));
  lay->addWidget(preview, 2);

  // 列表选中一条资产 → 预览标签（重选聚焦语义由 DataPreviewTabs 实现）。
  connect(table, &QTableWidget::itemSelectionChanged, this, [this, table]() {
    const QList<QTableWidgetItem *> sel = table->selectedItems();
    if (sel.isEmpty())
      return;
    const QString assetId = sel.front()->data(Qt::UserRole).toString();
    if (!assetId.isEmpty())
      emit assetActivated(assetId);
  });
}

void DataPage::refreshAssetTable()
{
  auto *table = findChild<QTableWidget *>(QStringLiteral("assetTable"));
  if (!table)
    return;
  auto *svc = qobject_cast<DataImportService *>(property("paleo.page.importsvc").value<QObject *>());
  if (!svc)
    return;
  const QVector<CatalogAsset> assets = svc->catalog()->assets();
  table->setRowCount(0);
  for (const CatalogAsset &a : assets)
  {
    const int r = table->rowCount();
    table->insertRow(r);
    auto *nameItem = new QTableWidgetItem(a.displayName);
    nameItem->setData(Qt::UserRole, a.id);
    table->setItem(r, 0, nameItem);
    table->setItem(r, 1, new QTableWidgetItem(a.type));
    const CatalogVersion v = svc->catalog()->currentVersion(a.id);
    table->setItem(r, 2, new QTableWidgetItem(v.managed ? tr("受管")
                                                        : tr("外部链接 %1").arg(v.path)));
  }
  refreshAssetEmptyState(table);
}

// ---------------------------------------------------------------------------
// PredictPage — ①智能预测
// ---------------------------------------------------------------------------
PredictPage::PredictPage(PredictionWorkflow *wf, QgisLayerService *layers, QWidget *parent)
  : QWidget(parent)
{
  auto *lay = panelLayout(this);

  lay->addWidget(caption(tr("层位"), this));
  auto *horizons = new QComboBox(this);
  horizons->setObjectName(QStringLiteral("horizonCombo"));
  lay->addWidget(horizons);

  lay->addWidget(caption(tr("算法"), this));
  auto *algos = new QComboBox(this);
  algos->setObjectName(QStringLiteral("algoCombo"));
  lay->addWidget(algos);

  // ONNX params area — visible only for onnx:* algorithms
  auto *paramsArea = new QWidget(this);
  paramsArea->setObjectName(QStringLiteral("onnxParamsArea"));
  auto *paramsLay = new QVBoxLayout(paramsArea);
  paramsLay->setContentsMargins(0, 0, 0, 0);
  paramsLay->setSpacing(8);

  paramsLay->addWidget(caption(tr("输入数据 (逗号分隔浮点数)"), paramsArea));
  auto *inputEdit = new QLineEdit(paramsArea);
  inputEdit->setObjectName(QStringLiteral("onnxInputEdit"));
  inputEdit->setPlaceholderText(QStringLiteral("例如: 0.0, 1.0"));
  paramsLay->addWidget(inputEdit);

  paramsLay->addWidget(caption(tr("输入形状 (逗号分隔整数)"), paramsArea));
  auto *shapeEdit = new QLineEdit(paramsArea);
  shapeEdit->setObjectName(QStringLiteral("onnxShapeEdit"));
  shapeEdit->setPlaceholderText(QStringLiteral("例如: 1, 1"));
  paramsLay->addWidget(shapeEdit);

  paramsLay->addWidget(caption(tr("输入名称"), paramsArea));
  auto *nameEdit = new QLineEdit(paramsArea);
  nameEdit->setObjectName(QStringLiteral("onnxInputNameEdit"));
  nameEdit->setText(QStringLiteral("x"));
  paramsLay->addWidget(nameEdit);

  paramsLay->addWidget(caption(tr("输出栅格（可空，按模型形状）"), paramsArea));
  auto *rowsEdit = new QLineEdit(paramsArea);
  rowsEdit->setObjectName(QStringLiteral("onnxRowsEdit"));
  rowsEdit->setPlaceholderText(tr("行数"));
  rowsEdit->setAccessibleName(tr("预测栅格行数"));
  paramsLay->addWidget(rowsEdit);
  auto *colsEdit = new QLineEdit(paramsArea);
  colsEdit->setObjectName(QStringLiteral("onnxColsEdit"));
  colsEdit->setPlaceholderText(tr("列数"));
  colsEdit->setAccessibleName(tr("预测栅格列数"));
  paramsLay->addWidget(colsEdit);
  auto *cellEdit = new QLineEdit(paramsArea);
  cellEdit->setObjectName(QStringLiteral("onnxCellEdit"));
  cellEdit->setPlaceholderText(tr("像元大小，默认 1"));
  cellEdit->setAccessibleName(tr("预测栅格像元大小"));
  paramsLay->addWidget(cellEdit);

  paramsArea->hide();
  lay->addWidget(paramsArea);

  const auto updateVisibility = [algos, paramsArea] {
    const QString alg = algos->currentData().toString();
    paramsArea->setVisible(alg.startsWith(QLatin1String("onnx:")));
  };
  connect(algos, &QComboBox::activated, this, updateVisibility);
  connect(algos, &QComboBox::currentIndexChanged, this, updateVisibility);

  auto *run = new QPushButton(tr("运行预测"), this);
  run->setObjectName(QStringLiteral("runButton"));
  lay->addWidget(run);
  connect(run, &QPushButton::clicked, this, [this, horizons, algos] {
    const QString horizon = horizons->currentText();
    const QString algId = algos->currentData().toString();
    QVariantMap params;
    if (algId.startsWith(QLatin1String("onnx:")))
    {
      params = parseInputParams();
      if (params.isEmpty())
        return;
    }
    emit runRequested(horizon, algId, params);
  });

  auto *status = new QLabel(this);
  status->setObjectName(QStringLiteral("statusLabel"));
  status->setWordWrap(true);
  lay->addWidget(status);
  lay->addStretch(1);

  if (wf) // workflow feedback lands on the status label
  {
    setAlgorithms(wf->availableAlgorithms());
    connect(wf, &PredictionWorkflow::predictionDone, status,
            [status](const QString &h, const QString &layerId) {
              status->setText(tr("预测完成：%1 → %2").arg(h, layerId));
            });
    connect(wf, &PredictionWorkflow::predictionFailed, status,
            [status](const QString &, const QString &error) { status->setText(error); });
  }
  Q_UNUSED(layers); // reserved: shell lists horizons from the layer service
}

void PredictPage::setHorizons(const QStringList &horizons)
{
  if (auto *combo = child<QComboBox>(this, "horizonCombo"))
  {
    combo->clear();
    combo->addItems(horizons);
  }
}

void PredictPage::setAlgorithms(const QStringList &algIds)
{
  if (auto *combo = child<QComboBox>(this, "algoCombo"))
  {
    combo->clear();
    for (const QString &id : algIds)
    {
      QString display = id;
      if (id.startsWith(QLatin1String("onnx:")))
        display = tr("%1 (ONNX)").arg(id.mid(5));
      combo->addItem(display, id);
    }
    if (auto *paramsArea = child<QWidget>(this, "onnxParamsArea"))
    {
      const QString cur = combo->currentData().toString();
      paramsArea->setVisible(cur.startsWith(QLatin1String("onnx:")));
    }
  }
}

QVariantMap PredictPage::parseInputParams()
{
  auto *status = child<QLabel>(this, "statusLabel");
  auto *inputEdit = child<QLineEdit>(this, "onnxInputEdit");
  auto *shapeEdit = child<QLineEdit>(this, "onnxShapeEdit");
  auto *nameEdit = child<QLineEdit>(this, "onnxInputNameEdit");
  auto *rowsEdit = child<QLineEdit>(this, "onnxRowsEdit");
  auto *colsEdit = child<QLineEdit>(this, "onnxColsEdit");
  auto *cellEdit = child<QLineEdit>(this, "onnxCellEdit");

  if (!inputEdit || !shapeEdit || !nameEdit)
    return {};

  const QString inStr = inputEdit->text().trimmed();
  if (inStr.isEmpty())
  {
    if (status)
      status->setText(tr("输入数据不能为空"));
    return {};
  }
  const QStringList inParts = inStr.split(QLatin1Char(','), Qt::SkipEmptyParts);
  if (inParts.isEmpty())
  {
    if (status)
      status->setText(tr("输入数据不能为空"));
    return {};
  }
  QVariantList inList;
  for (const QString &p : inParts)
  {
    bool ok = false;
    const float val = p.trimmed().toFloat(&ok);
    if (!ok)
    {
      if (status)
        status->setText(tr("输入数据包含非法浮点数: %1").arg(p.trimmed()));
      return {};
    }
    inList.append(val);
  }

  const QString shapeStr = shapeEdit->text().trimmed();
  if (shapeStr.isEmpty())
  {
    if (status)
      status->setText(tr("输入形状不能为空"));
    return {};
  }
  const QStringList shapeParts = shapeStr.split(QLatin1Char(','), Qt::SkipEmptyParts);
  if (shapeParts.isEmpty())
  {
    if (status)
      status->setText(tr("输入形状不能为空"));
    return {};
  }
  QVariantList shapeList;
  for (const QString &p : shapeParts)
  {
    bool ok = false;
    const qint64 val = p.trimmed().toLongLong(&ok);
    if (!ok)
    {
      if (status)
        status->setText(tr("输入形状包含非法整数: %1").arg(p.trimmed()));
      return {};
    }
    shapeList.append(val);
  }

  QString nameStr = nameEdit->text().trimmed();
  if (nameStr.isEmpty())
    nameStr = QStringLiteral("x");

  QVariantMap params;
  params.insert(QStringLiteral("input"), inList);
  params.insert(QStringLiteral("shape"), shapeList);
  params.insert(QStringLiteral("inputName"), nameStr);

  const auto fail = [status](const QString &msg) {
    if (status)
      status->setText(msg);
    return QVariantMap();
  };
  const QString rowsText = rowsEdit ? rowsEdit->text().trimmed() : QString();
  const QString colsText = colsEdit ? colsEdit->text().trimmed() : QString();
  if (rowsText.isEmpty() != colsText.isEmpty())
    return fail(tr("行数和列数需要同时填写"));
  if (!rowsText.isEmpty())
  {
    bool rowOk = false;
    bool colOk = false;
    const int rows = rowsText.toInt(&rowOk);
    const int cols = colsText.toInt(&colOk);
    if (!rowOk || !colOk || rows <= 0 || cols <= 0)
      return fail(tr("行数和列数必须是正整数"));
    params.insert(QStringLiteral("rows"), rows);
    params.insert(QStringLiteral("cols"), cols);
  }
  if (cellEdit && !cellEdit->text().trimmed().isEmpty())
  {
    bool ok = false;
    const double cell = cellEdit->text().trimmed().toDouble(&ok);
    if (!ok || !(cell > 0.0))
      return fail(tr("像元大小必须是正数"));
    params.insert(QStringLiteral("cellSize"), cell);
  }
  return params;
}

// ---------------------------------------------------------------------------
// ConstraintPage — ②约束与单因素
// ---------------------------------------------------------------------------
ConstraintPage::ConstraintPage(ConstraintWorkflow *wf, QWidget *parent)
  : QWidget(parent)
{
  auto *lay = panelLayout(this);

  lay->addWidget(caption(tr("层位"), this));
  auto *horizons = new QComboBox(this);
  horizons->setObjectName(QStringLiteral("horizonCombo"));
  lay->addWidget(horizons);

  lay->addWidget(caption(tr("约束"), this));
  auto *list = new QListWidget(this);
  list->setObjectName(QStringLiteral("constraintList"));
  list->setAccessibleName(tr("约束列表"));
  lay->addWidget(list, 1);

  auto *spin = new QSpinBox(this);
  spin->setObjectName(QStringLiteral("faciesCodeSpin"));
  spin->setRange(0, 9999);
  spin->setAccessibleName(tr("相代码"));
  lay->addWidget(spin);

  // Shape picker feeds ConstraintDrawController::startCapture's tool choice.
  auto *shape = new QComboBox(this);
  shape->setObjectName(QStringLiteral("shapeCombo"));
  shape->addItem(tr("约束线"), QStringLiteral("line"));
  shape->addItem(tr("约束多边形"), QStringLiteral("polygon"));
  shape->addItem(tr("约束矩形"), QStringLiteral("rect"));
  shape->addItem(tr("约束点"), QStringLiteral("point"));
  shape->addItem(tr("约束圆"), QStringLiteral("circle"));
  shape->addItem(tr("约束椭圆"), QStringLiteral("ellipse"));
  lay->addWidget(shape);

  auto *draw = new QPushButton(tr("绘制约束"), this);
  draw->setObjectName(QStringLiteral("drawButton"));
  lay->addWidget(draw);
  connect(draw, &QPushButton::clicked, this, [this, horizons, shape, spin] {
    emit drawConstraintRequested(horizons->currentText(),
                                 shape->currentData().toString(), spin->value());
  });

  auto *field = new QLineEdit(QStringLiteral("z"), this);
  field->setObjectName(QStringLiteral("idwField"));
  field->setPlaceholderText(tr("井属性字段"));
  field->setAccessibleName(tr("插值字段"));
  lay->addWidget(field);

  auto *cell = new QDoubleSpinBox(this);
  cell->setObjectName(QStringLiteral("idwCellSize"));
  cell->setRange(0.0001, 1.0e9);
  cell->setDecimals(4);
  cell->setValue(1.0);
  cell->setAccessibleName(tr("像元大小"));
  lay->addWidget(cell);

  auto *idw = new QPushButton(tr("插值"), this);
  idw->setObjectName(QStringLiteral("runIdwButton"));
  lay->addWidget(idw);
  connect(idw, &QPushButton::clicked, this, [this, horizons] {
    emit runIdwRequested(horizons->currentText());
  });

  auto *status = new QLabel(this);
  status->setObjectName(QStringLiteral("statusLabel"));
  status->setWordWrap(true);
  lay->addWidget(status);

  if (wf) // workflow feedback lands on the status label
  {
    connect(wf, &ConstraintWorkflow::constraintAdded, status,
            [status](const QString &id) { status->setText(tr("已添加约束 %1").arg(id)); });
    connect(wf, &ConstraintWorkflow::factorDone, status,
            [status](const QString &h, const QString &layerId) {
              status->setText(tr("单因素完成：%1 → %2").arg(h, layerId));
            });
  }
}

// ---------------------------------------------------------------------------
// ComposePage — ③综合编图
// ---------------------------------------------------------------------------
ComposePage::ComposePage(CompositionWorkflow *wf, QgisLayerService *layers, QWidget *parent)
  : QWidget(parent)
{
  setProperty(kLayersProp, QVariant::fromValue(static_cast<QObject *>(layers)));

  auto *lay = panelLayout(this);
  lay->addWidget(caption(tr("单因素图层"), this));
  auto *list = new QListWidget(this);
  list->setObjectName(QStringLiteral("factorList"));
  list->setAccessibleName(tr("单因素图层列表"));
  lay->addWidget(list, 1);

  auto *fuse = new QPushButton(tr("合成编图"), this);
  fuse->setObjectName(QStringLiteral("fuseButton"));
  lay->addWidget(fuse);
  connect(fuse, &QPushButton::clicked, this, [this, list] {
    QStringList ids;
    for (int i = 0; i < list->count(); ++i)
      if (list->item(i)->checkState() == Qt::Checked)
        ids << list->item(i)->data(Qt::UserRole).toString();
    emit fuseRequested(ids);
  });

  lay->addWidget(caption(tr("沉积相面"), this));
  auto *rasterCombo = new QComboBox(this);
  rasterCombo->setObjectName(QStringLiteral("faciesRasterCombo"));
  rasterCombo->setAccessibleName(tr("待转面的栅格"));
  lay->addWidget(rasterCombo);

  auto *minArea = new QDoubleSpinBox(this);
  minArea->setObjectName(QStringLiteral("minAreaSpin"));
  minArea->setAccessibleName(tr("碎屑面积阈值"));
  minArea->setDecimals(4);
  minArea->setRange(0.0, 1.0e9);
  minArea->setSingleStep(1.0);
  minArea->setPrefix(tr("最小面积 "));
  lay->addWidget(minArea);

  auto *simplify = new QDoubleSpinBox(this);
  simplify->setObjectName(QStringLiteral("simplifySpin"));
  simplify->setAccessibleName(tr("边界简化容差"));
  simplify->setDecimals(4);
  simplify->setRange(0.0, 1.0e9);
  simplify->setSingleStep(1.0);
  simplify->setPrefix(tr("简化容差 "));
  lay->addWidget(simplify);

  auto *polygonize = new QPushButton(tr("转为相多边形"), this);
  polygonize->setObjectName(QStringLiteral("polygonizeButton"));
  polygonize->setAccessibleName(tr("转为相多边形"));
  lay->addWidget(polygonize);
  connect(polygonize, &QPushButton::clicked, this, [this, rasterCombo, minArea, simplify] {
    const QString layerId = rasterCombo->currentData().toString();
    if (layerId.isEmpty())
    {
      if (auto *status = child<QLabel>(this, "statusLabel"))
        status->setText(tr("还没有可转面的栅格 — 先运行预测或合成编图"));
      return;
    }
    emit polygonizeRequested(layerId, minArea->value(), simplify->value());
  });

  auto *status = new QLabel(this);
  status->setObjectName(QStringLiteral("statusLabel"));
  status->setWordWrap(true);
  lay->addWidget(status);
  if (wf)
  {
    connect(wf, &CompositionWorkflow::compositionDone, status,
            [status](const QString &h, const QString &layerId) {
              status->setText(tr("合成完成：%1 → %2").arg(h, layerId));
            });
    connect(wf, &CompositionWorkflow::faciesPolygonsReady, status,
            [status](const QString &h, const QString &layerId) {
              status->setText(tr("相多边形完成：%1 → %2").arg(h, layerId));
            });
    connect(wf, &CompositionWorkflow::faciesPolygonsFailed, status,
            [status](const QString &, const QString &error) { status->setText(error); });
  }

  refreshFactors();
}

void ComposePage::refreshFactors()
{
  auto *list = child<QListWidget>(this, "factorList");
  if (!list)
    return;
  list->clear();
  auto *layers = qobject_cast<QgisLayerService *>(
      property(kLayersProp).value<QObject *>());
  if (!layers)
    return;
  for (const LayerDeclaration &d : layers->declared())
  {
    if (d.group != QLatin1String("04_SingleFactor"))
      continue;
    auto *it = new QListWidgetItem(d.layerId, list);
    it->setData(Qt::UserRole, d.layerId);
    it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
    it->setCheckState(Qt::Unchecked);
  }

  auto *combo = child<QComboBox>(this, "faciesRasterCombo");
  if (!combo)
    return;
  const QString previous = combo->currentData().toString();
  combo->clear();
  if (!layers)
    return;
  for (const LayerDeclaration &d : layers->declared())
  {
    const bool raster = d.type.compare(QLatin1String("raster"), Qt::CaseInsensitive) == 0;
    const bool groupOk = d.group == QLatin1String("03_Composite") ||
                         d.group == QLatin1String("01_Prediction") ||
                         d.group == QLatin1String("02_Prediction") ||
                         d.group == QLatin1String("03_Predict");
    const bool idOk = d.layerId.startsWith(QLatin1String("composite.")) ||
                      d.layerId.startsWith(QLatin1String("pred.")) ||
                      d.layerId.startsWith(QLatin1String("predict."));
    if (!raster || (!groupOk && !idOk))
      continue;
    combo->addItem(d.layerId, d.layerId);
  }
  const int keep = combo->findData(previous);
  if (keep >= 0)
    combo->setCurrentIndex(keep);
}

// ---------------------------------------------------------------------------
// ValidatePage — ④验证
// ---------------------------------------------------------------------------
ValidatePage::ValidatePage(ValidationWorkflow *wf, QWidget *parent)
  : QWidget(parent)
{
  setProperty(kWfProp, QVariant::fromValue(static_cast<QObject *>(wf)));

  auto *lay = panelLayout(this);
  auto *run = new QPushButton(tr("运行验证"), this);
  run->setObjectName(QStringLiteral("runButton"));
  lay->addWidget(run);
  connect(run, &QPushButton::clicked, this, [this] { populate(); });

  auto *table = new QTableWidget(0, 4, this);
  table->setObjectName(QStringLiteral("issueTable"));
  table->setAccessibleName(tr("验证问题列表"));
  table->setHorizontalHeaderLabels({tr("级别"), tr("代码"), tr("信息"), tr("图层")});
  table->verticalHeader()->setVisible(false);
  table->horizontalHeader()->setStretchLastSection(true);
  lay->addWidget(table, 1);
  connect(table, &QTableWidget::itemDoubleClicked, this, [this, table](QTableWidgetItem *it) {
    if (!it)
      return;
    auto *first = table->item(it->row(), 0); // issue data lives on column 0
    if (first)
      emit locateRequested(first->data(Qt::UserRole).toString(),
                           first->data(Qt::UserRole + 1).toString());
  });

  auto *status = new QLabel(this);
  status->setObjectName(QStringLiteral("statusLabel"));
  lay->addWidget(status);
  if (wf)
    connect(wf, &ValidationWorkflow::validationDone, status,
            [status](int n) { status->setText(tr("发现 %1 个问题").arg(n)); });
}

void ValidatePage::populate()
{
  auto *table = child<QTableWidget>(this, "issueTable");
  if (!table)
    return;
  table->setRowCount(0);
  auto *wf = qobject_cast<ValidationWorkflow *>(
      property(kWfProp).value<QObject *>());
  if (!wf)
    return;
  const QList<ValidationIssue> issues = wf->validate();
  for (const ValidationIssue &v : issues)
  {
    const int row = table->rowCount();
    table->insertRow(row);
    auto *sev = new QTableWidgetItem(severityText(v.severity));
    sev->setForeground(severityColor(v.severity));
    sev->setData(Qt::UserRole, v.layerId);       // locate intent reads these
    sev->setData(Qt::UserRole + 1, v.wktLocation);
    sev->setFlags(sev->flags() & ~Qt::ItemIsEditable);
    table->setItem(row, 0, sev);
    auto *code = new QTableWidgetItem(v.code);
    auto *msg = new QTableWidgetItem(v.message);
    auto *layer = new QTableWidgetItem(v.layerId);
    for (auto *it : {code, msg, layer})
      it->setFlags(it->flags() & ~Qt::ItemIsEditable);
    table->setItem(row, 1, code);
    table->setItem(row, 2, msg);
    table->setItem(row, 3, layer);
  }
}
