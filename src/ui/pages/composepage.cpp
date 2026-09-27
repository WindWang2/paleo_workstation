// 层：视图
#include "composepage.h"
#include "pageshared.h"
#include "../paleotheme.h"
#include "../../services/previewdoc.h"
#include "../../workflow/workflows.h"
#include "../../qgis/qgislayerservice.h"
#include "../../metadata/layermanifest.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

using namespace paleo::pagesinternal;

ComposePage::ComposePage(CompositionWorkflow *wf, QgisLayerService *layers, QWidget *parent)
  : QWidget(parent)
{
  setProperty(kLayersProp, QVariant::fromValue(static_cast<QObject *>(layers)));

  auto *lay = panelLayout(this);

  // ---- wave/mapping-pipeline 阶段C+E：D61 编图链 / 导出 / 版本按钮块 ----
  lay->addWidget(caption(tr("编图链（等时差 × 层间速度 → 等厚图）"), this));
  // D8 厚度触发：命名「生成 <层位> 等厚图」，未选层位禁用并写原因；使能态
  // 由壳经 setThicknessHorizon 跟 activeHorizon 联动。
  auto *chain = new QPushButton(tr("生成等厚图"), this);
  chain->setObjectName(QStringLiteral("thicknessChainButton"));
  chain->setEnabled(false);
  chain->setToolTip(tr("先在顶部层位 chip 选择层位"));
  chain->setAccessibleName(tr("生成等厚图"));
  connect(chain, &QPushButton::clicked, this, [this] { emit thicknessChainRequested(); });
  lay->addWidget(chain);

  auto *exportPdf = new QPushButton(tr("导出层位图 PDF"), this);
  exportPdf->setObjectName(QStringLiteral("exportPdfButton"));
  exportPdf->setAccessibleName(tr("导出层位图 PDF"));
  connect(exportPdf, &QPushButton::clicked, this, [this] { emit exportPdfRequested(); });
  lay->addWidget(exportPdf);

  auto *saveVersion = new QPushButton(tr("保存版本"), this);
  saveVersion->setObjectName(QStringLiteral("saveVersionButton"));
  saveVersion->setAccessibleName(tr("保存版本"));
  connect(saveVersion, &QPushButton::clicked, this, [this] { emit saveVersionRequested(); });
  lay->addWidget(saveVersion);

  auto *publish = new QPushButton(tr("发布"), this);
  publish->setObjectName(QStringLiteral("publishButton"));
  publish->setAccessibleName(tr("发布"));
  publish->setEnabled(false); // 发布门：PDF 能导出之后再暴露（shell 开闸）
  publish->setToolTip(tr("导出 PDF 后再保存")); // 门控原因（阶段E：PDF 先行）
  connect(publish, &QPushButton::clicked, this, [this] { emit publishRequested(); });
  lay->addWidget(publish);

  // 版本状态标注（已发布 / 编辑中 / 无版本），跟着 shell 的发布门刷新走。
  auto *publishState = new QLabel(this);
  publishState->setObjectName(QStringLiteral("publishStateLabel"));
  publishState->setAccessibleName(tr("版本发布状态"));
  publishState->setWordWrap(true);
  lay->addWidget(publishState);
  lay->addSpacing(16); // spacing.md between groups

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
  // autoplan §5C：厚度栅格不是相编码；工程里没有相编码栅格时明确说明，
  // 而不是笼统的「还没有可转面的栅格」。
  connect(polygonize, &QPushButton::clicked, this, [this, rasterCombo] {
    if (!rasterCombo->currentData().toString().isEmpty())
      return;
    auto *status = child<QLabel>(this, "statusLabel");
    auto *layers = qobject_cast<QgisLayerService *>(
        property(kLayersProp).value<QObject *>());
    QVector<LayerDeclaration> declared;
    if (status && layers && layers->tryDeclared(&declared))
    {
      const bool anyRaster = std::any_of(
          declared.cbegin(), declared.cend(), [](const LayerDeclaration &d) {
            return d.type.compare(QLatin1String("raster"), Qt::CaseInsensitive) == 0;
          });
      if (anyRaster)
        status->setText(tr("没有相编码栅格，这一工区不从厚度生成相"));
    }
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
  // A raster declared after this page exists (e.g. a fresh ONNX prediction)
  // still lands in the combo; the declaration set is the source of truth.
  if (layers)
    connect(layers, &QgisLayerService::layerDeclared, this,
            [this](const QString &) { refreshFactors(); });

  refreshFactors();
}

void ComposePage::setPublishEnabled(bool enabled)
{
  setPublishState(enabled, -1, -1); // 旧调用形态：不带残差评估
}

void ComposePage::setPublishState(bool hasPdf, int covered, int total)
{
  auto *btn = child<QPushButton>(this, "publishButton");
  if (!btn)
    return;
  // covered<0 = 调用方没评估残差 → 不拿残差卡门；其余情形须 total>0 且全
  // 覆盖（每口井都有残差或原因，§177）。
  const bool residualsOk = covered < 0 || (total > 0 && covered == total);
  btn->setEnabled(hasPdf && residualsOk);
  QString tip;
  if (!hasPdf)
    tip = tr("导出 PDF 后再保存");
  else if (!residualsOk)
    tip = total > 0 ? tr("还有 %1/%2 口井没有残差或原因 — 先在验证页运行验证")
                        .arg(total - covered)
                        .arg(total)
                    : tr("先在验证页运行验证");
  btn->setToolTip(tip);
}

void ComposePage::setVersionState(int version, bool published)
{
  if (auto *label = child<QLabel>(this, "publishStateLabel"))
  {
    // T27：版本状态胶囊——已发布=绿、编辑中=橙、无版本=中性「未计算」类。
    label->setText(version <= 0 ? tr("还没有保存的版本")
                   : published  ? tr("已发布 · v%1").arg(version)
                                : tr("编辑中 · v%1").arg(version));
    label->setStyleSheet(PaleoTheme::capsuleStyleSheet(
        version <= 0 ? PaleoTheme::CapsuleKind::Neutral
                     : (published ? PaleoTheme::CapsuleKind::Success
                                  : PaleoTheme::CapsuleKind::Warning)));
  }
  if (auto *save = child<QPushButton>(this, "saveVersionButton"))
  {
    save->setText(published ? tr("保存新版本") : tr("保存版本"));
    // tooltip 写明改名原因；ribbon 镜像靠 ToolTipChange 顺带同步文案。
    save->setToolTip(published ? tr("v%1 已发布、快照只读 — 继续保存会产生新版本").arg(version)
                               : tr("把当前编图保存为版本快照"));
  }
}

void ComposePage::setThicknessHorizon(const QString &horizon)
{
  auto *chain = child<QPushButton>(this, "thicknessChainButton");
  if (!chain)
    return;
  chain->setEnabled(!horizon.isEmpty());
  chain->setText(horizon.isEmpty() ? tr("生成等厚图")
                                  : tr("生成 %1 等厚图").arg(horizon));
  chain->setAccessibleName(chain->text());
  chain->setToolTip(horizon.isEmpty()
                        ? tr("先在顶部层位 chip 选择层位")
                        : tr("等时差（双向 ms）× 层间速度 IDW → 厚度栅格"));
}

void ComposePage::refreshFactors()
{
  auto *list = child<QListWidget>(this, "factorList");
  if (!list)
    return;
  auto *layers = qobject_cast<QgisLayerService *>(
      property(kLayersProp).value<QObject *>());
  // Manifest read failure keeps current content — an empty declaration set
  // here would silently blank the page.
  QVector<LayerDeclaration> declared;
  if (!layers || !layers->tryDeclared(&declared))
    return;
  list->clear();
  for (const LayerDeclaration &d : declared)
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
  for (const LayerDeclaration &d : declared)
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