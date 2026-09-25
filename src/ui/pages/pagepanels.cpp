#include "pagepanels.h"

#include "../../workflow/workflows.h"    // signal names + ValidationWorkflow::validate
#include "../../qgis/qgislayerservice.h" // declared() — forward-declares Qgs*, none included
#include "../../metadata/layermanifest.h" // LayerDeclaration fields
#include "../../domain/types.h"          // ValidationIssue fields

#include <QComboBox>
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
  lay->addWidget(shape);

  auto *draw = new QPushButton(tr("绘制约束"), this);
  draw->setObjectName(QStringLiteral("drawButton"));
  lay->addWidget(draw);
  connect(draw, &QPushButton::clicked, this, [this, horizons, shape, spin] {
    emit drawConstraintRequested(horizons->currentText(),
                                 shape->currentData().toString(), spin->value());
  });

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

  auto *status = new QLabel(this);
  status->setObjectName(QStringLiteral("statusLabel"));
  status->setWordWrap(true);
  lay->addWidget(status);
  if (wf)
    connect(wf, &CompositionWorkflow::compositionDone, status,
            [status](const QString &h, const QString &layerId) {
              status->setText(tr("合成完成：%1 → %2").arg(h, layerId));
            });

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
