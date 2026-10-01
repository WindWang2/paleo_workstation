// 层：视图
#include "predictpage.h"

#if PALEO_HAVE_ORT
#include "../../ai/onnxpredictionservice.h" // runtimeAvailable() 降级文案用
#endif

#include "pageshared.h"

#include "../../ai/onnxpredictionservice.h" // ORT-free header; runtimeAvailable 调用受 PALEO_HAVE_ORT 保护
#include "../../domain/arearules.h"
#include "../../services/algoparamschema.h" // 算法参数 schema 注册表（数据层，只读）
#include "../../workflow/workflows.h"    // signal names
#include "../../qgis/layervocabulary.h"
#include "../../qgis/qgislayerservice.h" // declared()/layerDeclared — forward-declares Qgs*, none included

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

using namespace paleo::pagesinternal;

// 层：视图
// ---------------------------------------------------------------------------
// PredictPage — ①智能预测
// m2/mapping-pages(A)：
//   · 页顶参数区 = 预测类型（predictTypeCombo，词表/英文 stable id）+
//     既有 horizonCombo/algoCombo（objectName 不变）。
//   · paramsFormArea 按算法 schema（algoparamschema）动态建控件；onnx:* 的
//     schema 语义退化为既有三控件（onnxParamsArea 原样保留，行为不变）。
//   · 任务化运行面：setRunBusy/updateProgress/runCancelRequested +
//     cancelRunButton/runProgressBar。
//   · historyList：manifest 当前层位的预测结果声明，行内「显示」发
//     showResultRequested(layerId)；layerDeclared 变化即刷新。
// ---------------------------------------------------------------------------
PredictPage::PredictPage(PredictionWorkflow *wf, QgisLayerService *layers, QWidget *parent)
  : QWidget(parent)
{
  setProperty(kLayersProp, QVariant::fromValue(static_cast<QObject *>(layers)));

  auto *lay = panelLayout(this);

  lay->addWidget(caption(tr("预测类型"), this));
  auto *types = new QComboBox(this);
  types->setObjectName(QStringLiteral("predictTypeCombo"));
  // 词表（显示词 → userData 英文 stable id）：预测语义分类，参与 schema
  // 表单联动（换类型即按当前算法重建表单）并随参数 map 下发。
  types->addItem(tr("沉积相"), QStringLiteral("sedimentary_facies"));
  types->addItem(tr("地震相"), QStringLiteral("seismic_facies"));
  types->addItem(tr("测井相"), QStringLiteral("well_log_facies"));
  lay->addWidget(types);

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
  inputEdit->setPlaceholderText(tr("例如: 0.0, 1.0"));
  paramsLay->addWidget(inputEdit);

  paramsLay->addWidget(caption(tr("输入形状 (逗号分隔整数)"), paramsArea));
  auto *shapeEdit = new QLineEdit(paramsArea);
  shapeEdit->setObjectName(QStringLiteral("onnxShapeEdit"));
  shapeEdit->setPlaceholderText(tr("例如: 1, 1"));
  paramsLay->addWidget(shapeEdit);

  paramsLay->addWidget(caption(tr("输入名称"), paramsArea));
  auto *nameEdit = new QLineEdit(paramsArea);
  nameEdit->setObjectName(QStringLiteral("onnxInputNameEdit"));
  nameEdit->setText(QStringLiteral("x"));
  paramsLay->addWidget(nameEdit);

  // 输出网格是工区合同（PROJECT_AREA_PLAN §3）：标定层位栅格 + 同一套
  // geotransform，行/列/像元来自 project_area.json 的 onnx_grid——尺寸
  // 不符时 workflow 拒绝写盘。文案随工程参数刷新（onnxGridCaption）。
  auto *onnxCap = caption(
      tr("输出固定为 %1 工区网格 %2×%3")
          .arg(AreaRules::active().targetHorizon)
          .arg(AreaRules::active().onnxGrid.rows)
          .arg(AreaRules::active().onnxGrid.cols),
      paramsArea);
  onnxCap->setObjectName(QStringLiteral("onnxGridCaption"));
  paramsLay->addWidget(onnxCap);

  paramsArea->hide();
  lay->addWidget(paramsArea);

  // paleo:* 动态参数表单（schema 驱动）；onnx:* 不用此容器（三控件语义在
  // onnxParamsArea）。objectName "paramsFormArea"，编辑控件 "param.<KEY>"。
  auto *formArea = new QWidget(this);
  formArea->setObjectName(QStringLiteral("paramsFormArea"));
  auto *formLay = new QVBoxLayout(formArea);
  formLay->setContentsMargins(0, 0, 0, 0);
  formLay->setSpacing(8);
  formArea->hide();
  lay->addWidget(formArea);

  // 表单重建同时监听算法与预测类型（联动：schema 解析上下文 = 当前算法 +
  // 当前类型；换类型按默认值重建，保持表单无隐藏状态）。
  const auto refreshForm = [this] { rebuildSchemaForm(); };
  connect(algos, &QComboBox::activated, this, refreshForm);
  connect(algos, &QComboBox::currentIndexChanged, this, refreshForm);
  connect(types, &QComboBox::currentIndexChanged, this, refreshForm);

  auto *run = new QPushButton(tr("运行预测"), this);
  run->setObjectName(QStringLiteral("runButton"));
  lay->addWidget(run);

  // m2(A) 任务化运行面：忙碌时露取消按钮与进度条（DESIGN.md：>1s 的任务
  // 显示进度条；无进度回调的算法进度停在 0，不伪造）。
  auto *cancel = new QPushButton(tr("取消"), this);
  cancel->setObjectName(QStringLiteral("cancelRunButton"));
  cancel->hide();
  lay->addWidget(cancel);
  connect(cancel, &QPushButton::clicked, this, [this] { emit runCancelRequested(); });

  auto *progress = new QProgressBar(this);
  progress->setObjectName(QStringLiteral("runProgressBar"));
  progress->setRange(0, 100);
  progress->setValue(0);
  progress->hide();
  lay->addWidget(progress);

  // 外部工具：MAMCL 地震多属性智能分析（独立 Python 程序）。视图只发意图；
  // venv/依赖/启动编排走壳里的 MamclTool，busy 经 setMamclBusy 回流。
  lay->addWidget(caption(tr("外部工具"), this));
  auto *mamcl = new QPushButton(tr("地震多属性智能分析 (MAMCL)"), this);
  mamcl->setObjectName(QStringLiteral("mamclButton"));
  mamcl->setToolTip(tr("启动独立的 MAMCL 分析程序（首次启动会自动准备 Python 环境）"));
  lay->addWidget(mamcl);
  connect(mamcl, &QPushButton::clicked, this, [this] { emit mamclLaunchRequested(); });

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
    else
    {
      // paleo:*：schema 控件收集（未登记算法 → 空表，行为与拆分前一致）；
      // 非法输入写 statusLabel 且不发意图信号。
      if (!collectSchemaParams(&params))
        return;
    }
    emit runRequested(horizon, algId, params);
  });

  auto *status = new QLabel(this);
  status->setObjectName(QStringLiteral("statusLabel"));
  status->setWordWrap(true);
  lay->addWidget(status);

  // 历史结果：manifest 当前层位的预测产物声明；「显示」发意图给壳。
  lay->addWidget(caption(tr("历史结果"), this));
  auto *history = new QListWidget(this);
  history->setObjectName(QStringLiteral("historyList"));
  lay->addWidget(history, 1);
  connect(history, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
    if (item)
      emit showResultRequested(item->data(Qt::UserRole).toString());
  });
  connect(horizons, &QComboBox::currentIndexChanged, this, [this] { refreshHistory(); });
  if (layers) // 声明落地（含重跑 upsert）即刷新清单
    connect(layers, &QgisLayerService::layerDeclared, this, [this] { refreshHistory(); });

  if (wf) // workflow feedback lands on the status label
  {
    setAlgorithms(wf->availableAlgorithms());
    connect(wf, &PredictionWorkflow::predictionDone, status,
            [status](const QString &h, const QString &layerId) {
              status->setText(tr("预测完成：%1 → %2").arg(h, layerId));
            });
    connect(wf, &PredictionWorkflow::predictionFailed, status,
            [status](const QString &, const QString &error) { status->setText(error); });
#if PALEO_HAVE_ORT
    // 诚实可用性：服务恒绑定，但运行库缺失时 onnx:* 不会出现在算法列表里；
    // 在这里写明原因，而不是静默少列。
    if (!PaleoOnnxService::runtimeAvailable())
      status->setText(tr("ONNX 运行时不可用（vendor/onnxruntime 缺少运行库）"));
#endif
  }
  refreshHistory();
}

void PredictPage::setHorizons(const QStringList &horizons)
{
  if (auto *combo = child<QComboBox>(this, "horizonCombo"))
  {
    combo->clear();
    combo->addItems(horizons); // clear/addItems 触发 currentIndexChanged → 历史清单随层位刷新
  }
}

void PredictPage::setAlgorithms(const QStringList &algIds)
{
  bool hasOnnx = false;
  if (auto *combo = child<QComboBox>(this, "algoCombo"))
  {
    combo->clear();
    for (const QString &id : algIds)
    {
      QString display = id;
      if (id.startsWith(QLatin1String("onnx:")))
      {
        hasOnnx = true;
        display = tr("%1 (ONNX)").arg(id.mid(5));
      }
      combo->addItem(display, id);
    }
    rebuildSchemaForm();
  }
  // 缺模型如实降级（范围5）：运行库在而 models/ 无可用 .onnx → 写明原因，
  // 不静默少列；模型可用时撤下降级文案（只撤我们自己写的——别的人写的
  // 状态文案不动）。运行库缺失的文案在构造期（runtimeAvailable 分支）。
  auto *status = child<QLabel>(this, "statusLabel");
#if PALEO_HAVE_ORT
  static const QString kDegrade = tr("未装模型");
  if (status && PaleoOnnxService::runtimeAvailable())
  {
    if (!hasOnnx)
      status->setText(tr("未装模型：<工程>/models 下无可用 .onnx（注册表如实降级，装入后重开工程）"));
    else if (status->text().contains(kDegrade))
      status->clear();
  }
#endif
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
// m2(A)：schema 驱动的动态表单
// ---------------------------------------------------------------------------

void PredictPage::rebuildSchemaForm()
{
  auto *algos = child<QComboBox>(this, "algoCombo");
  auto *onnxArea = child<QWidget>(this, "onnxParamsArea");
  auto *formArea = child<QWidget>(this, "paramsFormArea");
  if (!algos || !onnxArea || !formArea)
    return;

  const QString algId = algos->currentData().toString();
  onnxArea->setVisible(algId.startsWith(QLatin1String("onnx:")));

  // 重建前按 key 快照已填值——切换算法/预测类型触发的重建对同名参数保留
  // 用户输入（联动不吞状态）。快照存页面属性：跨多次重建（如 smoothing →
  // isopach → smoothing）仍能把值带回来；每次重建叠加当前活控件值。
  QVariantMap carried = property("paleo.page.paramcarry").toMap();
  const auto widgets = formArea->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly);
  for (QWidget *w : widgets)
  {
    const QString name = w->objectName();
    if (!name.startsWith(QLatin1String("param.")))
      continue;
    const QString key = name.mid(6);
    if (auto *spin = qobject_cast<QSpinBox *>(w))
      carried.insert(key, spin->value());
    else if (auto *dspin = qobject_cast<QDoubleSpinBox *>(w))
      carried.insert(key, dspin->value());
    else if (auto *combo = qobject_cast<QComboBox *>(w))
      carried.insert(key, combo->currentData());
    else if (auto *edit = qobject_cast<QLineEdit *>(w))
      carried.insert(key, edit->text());
  }
  setProperty("paleo.page.paramcarry", carried);

  // 立即删除旧行（不用 deleteLater）：重建可能由级联信号触发，悬挂控件会
  // 干扰 findChild<"param.<KEY>"> 的唯一定位。
  auto *formLay = static_cast<QVBoxLayout *>(formArea->layout());
  while (formLay->count() > 0)
  {
    QLayoutItem *item = formLay->takeAt(0);
    if (item->widget())
      delete item->widget();
    delete item;
  }

  if (algId.startsWith(QLatin1String("onnx:")))
  {
    // onnx:* 的参数语义 = 既有三控件（退化呈现），不进 paramsFormArea。
    formArea->hide();
    return;
  }


  const QVector<AlgorithmParamField> fields = AlgorithmParamSchema::fieldsFor(algId);
  if (!AlgorithmParamSchema::knows(algId))
  {
    // 未登记算法（如测试桩 id）：无表单——params map 保持空，行为与拆分
    // 前一致（tst_panels 既有断言依赖空表）。
    formArea->hide();
    return;
  }

  for (const AlgorithmParamField &f : fields)
  {
    formLay->addWidget(caption(f.label, formArea));
    switch (f.type)
    {
      case AlgorithmParamField::Int:
      {
        auto *spin = new QSpinBox(formArea);
        spin->setObjectName(QStringLiteral("param.%1").arg(f.key));
        spin->setRange(f.hasMin ? static_cast<int>(f.minValue) : 0,
                       f.hasMax ? static_cast<int>(f.maxValue) : 99);
        spin->setValue(f.defaultValue.toInt());
        if (carried.contains(f.key))
          spin->setValue(carried.value(f.key).toInt());
        if (!f.suffix.isEmpty())
          spin->setSuffix(f.suffix);
        formLay->addWidget(spin);
        break;
      }
      case AlgorithmParamField::Double:
      {
        auto *spin = new QDoubleSpinBox(formArea);
        spin->setObjectName(QStringLiteral("param.%1").arg(f.key));
        spin->setRange(f.hasMin ? f.minValue : 0.0, f.hasMax ? f.maxValue : 1e9);
        spin->setDecimals(f.decimals);
        spin->setValue(f.defaultValue.toDouble());
        if (carried.contains(f.key))
          spin->setValue(carried.value(f.key).toDouble());
        if (!f.suffix.isEmpty())
          spin->setSuffix(f.suffix);
        formLay->addWidget(spin);
        break;
      }
      case AlgorithmParamField::String:
      {
        auto *edit = new QLineEdit(formArea);
        edit->setObjectName(QStringLiteral("param.%1").arg(f.key));
        edit->setText(f.defaultValue.toString());
        if (carried.contains(f.key))
          edit->setText(carried.value(f.key).toString());
        formLay->addWidget(edit);
        break;
      }
      case AlgorithmParamField::FloatList:
      {
        auto *edit = new QLineEdit(formArea);
        edit->setObjectName(QStringLiteral("param.%1").arg(f.key));
        edit->setPlaceholderText(tr("逗号分隔浮点数"));
        if (carried.contains(f.key))
          edit->setText(carried.value(f.key).toString());
        formLay->addWidget(edit);
        break;
      }
      case AlgorithmParamField::InList:
      {
        auto *combo = new QComboBox(formArea);
        combo->setObjectName(QStringLiteral("param.%1").arg(f.key));
        int defIdx = 0;
        for (int i = 0; i < f.options.size(); ++i)
        {
          combo->addItem(f.options.at(i).label, f.options.at(i).value);
          if (f.options.at(i).value == f.defaultValue)
            defIdx = i;
        }
        combo->setCurrentIndex(defIdx);
        if (carried.contains(f.key))
        {
          const int idx = combo->findData(carried.value(f.key));
          if (idx >= 0)
            combo->setCurrentIndex(idx);
        }
        formLay->addWidget(combo);
        break;
      }
    }
  }
  if (fields.isEmpty())
  {
    // 登记在册但零标量参数（如 paleo:paleo_facies_fusion——图层型参数由
    // 编排解析）：如实说明，不虚构控件。
    auto *note = caption(tr("该算法无标量参数"), formArea);
    note->setObjectName(QStringLiteral("schemaNoFieldsNote"));
    formLay->addWidget(note);
  }
  formArea->setVisible(true);
}

bool PredictPage::collectSchemaParams(QVariantMap *out)
{
  if (out)
    out->clear();
  auto *algos = child<QComboBox>(this, "algoCombo");
  if (!algos || !out)
    return true;
  const QString algId = algos->currentData().toString();
  if (algId.startsWith(QLatin1String("onnx:")) || !AlgorithmParamSchema::knows(algId))
    return true; // 空 params：与拆分前的 paleo:* 行为一致

  auto *status = child<QLabel>(this, "statusLabel");
  const QVector<AlgorithmParamField> fields = AlgorithmParamSchema::fieldsFor(algId);
  QVariantMap params;
  for (const AlgorithmParamField &f : fields)
  {
    const QString name = QStringLiteral("param.%1").arg(f.key);
    switch (f.type)
    {
      case AlgorithmParamField::Int:
        if (auto *spin = child<QSpinBox>(this, name.toUtf8().constData()))
          params.insert(f.key, spin->value());
        break;
      case AlgorithmParamField::Double:
        if (auto *spin = child<QDoubleSpinBox>(this, name.toUtf8().constData()))
          params.insert(f.key, spin->value());
        break;
      case AlgorithmParamField::String:
        if (auto *edit = child<QLineEdit>(this, name.toUtf8().constData()))
        {
          // 主线7：必填 String 空 → 收集侧拒收（statusLabel 说明 + 不发意图）。
          if (f.required && edit->text().trimmed().isEmpty())
          {
            if (status)
              status->setText(tr("%1 不能为空").arg(f.label));
            return false;
          }
          params.insert(f.key, edit->text().trimmed());
        }
        break;
      case AlgorithmParamField::FloatList:
      {
        auto *edit = child<QLineEdit>(this, name.toUtf8().constData());
        if (!edit)
          break;
        QVariantList vals;
        const QStringList parts = edit->text().split(QLatin1Char(','), Qt::SkipEmptyParts);
        for (const QString &p : parts)
        {
          bool ok = false;
          const float v = p.trimmed().toFloat(&ok);
          if (!ok)
          {
            if (status)
              status->setText(tr("%1 包含非法浮点数: %2").arg(f.label, p.trimmed()));
            return false;
          }
          vals.append(v);
        }
        params.insert(f.key, vals);
        break;
      }
      case AlgorithmParamField::InList:
        if (auto *combo = child<QComboBox>(this, name.toUtf8().constData()))
          params.insert(f.key, combo->currentData());
        break;
    }
  }
  // 预测类型随参数下发（英文 stable id；QgsProcessing 忽略未定义键，无副作用）。
  if (auto *types = child<QComboBox>(this, "predictTypeCombo"))
    params.insert(QStringLiteral("predictType"), types->currentData());
  *out = params;
  // 校验通过即下发：清掉上一轮的失败文案（陈旧错误不残留）。
  if (status)
    status->clear();
  return true;
}

// ---------------------------------------------------------------------------
// m2(A)：任务化运行面
// ---------------------------------------------------------------------------

void PredictPage::setRunBusy(bool busy)
{
  auto *run = child<QPushButton>(this, "runButton");
  auto *cancel = child<QPushButton>(this, "cancelRunButton");
  auto *bar = child<QProgressBar>(this, "runProgressBar");
  auto *status = child<QLabel>(this, "statusLabel");

  if (run)
  {
    run->setEnabled(!busy);
    // DESIGN.md：禁用控件必须带 reason tooltip。
    run->setToolTip(busy ? tr("预测正在运行——完成后自动恢复，或点取消") : QString());
  }
  if (cancel)
    cancel->setVisible(busy);
  if (bar)
  {
    bar->setVisible(busy);
    if (busy)
      bar->setValue(0);
  }
  if (busy && status)
    status->setText(tr("正在运行预测…"));
  setProperty("paleo.page.runbusy", busy);
}

void PredictPage::updateProgress(int percent)
{
  if (auto *bar = child<QProgressBar>(this, "runProgressBar"))
    bar->setValue(qBound(0, percent, 100));
}

void PredictPage::setMamclBusy(bool busy)
{
  if (auto *mamcl = child<QPushButton>(this, "mamclButton"))
  {
    mamcl->setEnabled(!busy);
    // DESIGN.md：禁用控件必须带 reason tooltip。
    mamcl->setToolTip(busy ? tr("MAMCL 环境准备中——完成后自动启动")
                           : tr("启动独立的 MAMCL 分析程序（首次启动会自动准备 Python 环境）"));
  }
}

// ---------------------------------------------------------------------------
// m2(A)：历史结果清单
// ---------------------------------------------------------------------------

void PredictPage::refreshHistory()
{
  auto *list = child<QListWidget>(this, "historyList");
  auto *horizons = child<QComboBox>(this, "horizonCombo");
  if (!list)
    return;
  list->clear();

  QgisLayerService *layers =
      qobject_cast<QgisLayerService *>(property(kLayersProp).value<QObject *>());
  if (!layers || !horizons)
    return; // 未绑定图层服务（如测试桩）→ 空清单

  const QString horizon = horizons->currentText();
  // 兼容清单收口到词表权威（主线1）：02_Prediction 家族 = canonical + 旧名。
  const QStringList predictGroups = PaleoLayerVocabulary::groupFamily(
      QStringLiteral("02_Prediction"));
  for (const LayerDeclaration &d : layers->declared())
  {
    if (d.horizon != horizon)
      continue;
    const bool byGroup = predictGroups.contains(d.group);
    const bool byPrefix = d.layerId.startsWith(QLatin1String("pred.")) ||
                          d.layerId.startsWith(QLatin1String("predict.")) ||
                          d.layerId.startsWith(QLatin1String("confidence."));
    if (!byGroup && !byPrefix)
      continue;

    auto *item = new QListWidgetItem(d.title.isEmpty() ? d.layerId : d.title, list);
    item->setData(Qt::UserRole, d.layerId);
    // C5：行控件 = 标题 label + 「显示」按钮——结果名不能被按钮整行遮蔽。
    auto *roww = new QWidget(list);
    auto *rl = new QHBoxLayout(roww);
    rl->setContentsMargins(4, 1, 4, 1);
    rl->setSpacing(4);
    auto *title = new QLabel(d.title.isEmpty() ? d.layerId : d.title, roww);
    title->setToolTip(d.layerId);
    rl->addWidget(title, 1);
    auto *showBtn = new QPushButton(tr("显示"), roww);
    showBtn->setObjectName(QStringLiteral("historyShowButton"));
    showBtn->setToolTip(tr("在地图上显示 %1").arg(d.layerId));
    const QString layerId = d.layerId;
    connect(showBtn, &QPushButton::clicked, this, [this, layerId] {
      emit showResultRequested(layerId);
    });
    rl->addWidget(showBtn, 0);
    item->setSizeHint(roww->sizeHint());
    list->setItemWidget(item, roww);
  }
}
