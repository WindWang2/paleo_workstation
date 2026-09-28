// 层：视图
#include "composepage.h"

#include "panelshared.h"

#include "../paleotheme.h" // DESIGN.md token 出口：胶囊样式

#include "../../workflow/workflows.h"    // signal names
#include "../../qgis/qgislayerservice.h" // declared() — forward-declares Qgs*, none included
#include "../../metadata/layermanifest.h" // LayerDeclaration fields

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <algorithm>

using namespace PaleoPanel;

namespace
{
  // m2(C)：相属性编辑目标层（动态属性存态——页面类按惯例不持数据成员）。
  constexpr const char kFaciesTargetProp[] = "paleo.page.faciesTarget";

  bool isRaster(const LayerDeclaration &d)
  {
    return d.type.compare(QLatin1String("raster"), Qt::CaseInsensitive) == 0;
  }
  bool inGroup(const LayerDeclaration &d, const char *group)
  {
    return d.group == QLatin1String(group) ||
           d.group.startsWith(QLatin1String(group) + QLatin1Char('/'));
  }
} // namespace

// 层：视图
// ---------------------------------------------------------------------------
// ComposePage — ③综合编图
// ---------------------------------------------------------------------------
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

  // m2(C)：在布局设计器中打开本页版面（导出用的同一主题版面）。
  auto *openDesigner = new QPushButton(tr("在布局设计器中打开"), this);
  openDesigner->setObjectName(QStringLiteral("openDesignerButton"));
  openDesigner->setAccessibleName(tr("在布局设计器中打开"));
  connect(openDesigner, &QPushButton::clicked, this,
          [this] { emit layoutDesignerRequested(); });
  lay->addWidget(openDesigner);

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

  // m2(C)：全选/清空小工具行（融合清单勾选收集的批量入口）。
  {
    auto *row = new QHBoxLayout();
    auto *selectAll = new QPushButton(tr("全选"), this);
    selectAll->setObjectName(QStringLiteral("factorSelectAllButton"));
    selectAll->setAccessibleName(tr("全选因素"));
    connect(selectAll, &QPushButton::clicked, this, [this, list] {
      for (int i = 0; i < list->count(); ++i)
        list->item(i)->setCheckState(Qt::Checked);
    });
    auto *clearAll = new QPushButton(tr("清空"), this);
    clearAll->setObjectName(QStringLiteral("factorClearButton"));
    clearAll->setAccessibleName(tr("清空勾选"));
    connect(clearAll, &QPushButton::clicked, this, [this, list] {
      for (int i = 0; i < list->count(); ++i)
        list->item(i)->setCheckState(Qt::Unchecked);
    });
    row->addWidget(selectAll);
    row->addWidget(clearAll);
    row->addStretch(1);
    lay->addLayout(row);
  }

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

  // ---- m2(C)：相属性区（矢量化产物要素的三字段编辑 → 保存意图信号）----
  lay->addSpacing(16); // spacing.md between groups
  auto *attrSection = new CollapsibleSection(tr("相属性"), this);
  attrSection->setObjectName(QStringLiteral("faciesAttrArea"));
  {
    QVBoxLayout *al = attrSection->containerLayout();
    auto *target = new QLabel(attrSection);
    target->setObjectName(QStringLiteral("faciesTargetLabel"));
    target->setWordWrap(true);
    al->addWidget(target);

    auto *code = new QLineEdit(attrSection);
    code->setObjectName(QStringLiteral("faciesCodeEdit"));
    code->setAccessibleName(tr("相代码"));
    code->setPlaceholderText(tr("相代码（整数）"));
    al->addWidget(code);

    auto *faciesType = new QLineEdit(attrSection);
    faciesType->setObjectName(QStringLiteral("faciesTypeEdit"));
    faciesType->setAccessibleName(tr("相类型"));
    faciesType->setPlaceholderText(tr("相类型（如 辫状河三角洲）"));
    al->addWidget(faciesType);

    auto *comment = new QLineEdit(attrSection);
    comment->setObjectName(QStringLiteral("faciesCommentEdit"));
    comment->setAccessibleName(tr("相备注"));
    comment->setPlaceholderText(tr("备注"));
    al->addWidget(comment);

    auto *save = new QPushButton(tr("保存相属性"), attrSection);
    save->setObjectName(QStringLiteral("faciesAttrSaveButton"));
    save->setAccessibleName(tr("保存相属性"));
    save->setEnabled(false);
    save->setToolTip(tr("先矢量化生成相界图层"));
    connect(save, &QPushButton::clicked, this, [this, code, faciesType, comment] {
      const QString layerId = property(kFaciesTargetProp).toString();
      const QString codeText = code->text().trimmed();
      auto *status = child<QLabel>(this, "statusLabel");
      if (layerId.isEmpty())
      {
        if (status)
          status->setText(tr("还没有相属性目标层 — 先「转为相多边形」"));
        return;
      }
      if (!codeText.isEmpty())
      {
        bool ok = false;
        codeText.toInt(&ok);
        if (!ok)
        {
          if (status)
            status->setText(tr("相代码须是整数：%1").arg(codeText));
          return;
        }
      }
      QVariantMap attrs;
      if (!codeText.isEmpty())
        attrs.insert(QStringLiteral("facies_code"), QVariant(codeText.toInt()));
      if (!faciesType->text().trimmed().isEmpty())
        attrs.insert(QStringLiteral("facies_type"), faciesType->text().trimmed());
      if (!comment->text().trimmed().isEmpty())
        attrs.insert(QStringLiteral("comment"), comment->text().trimmed());
      emit faciesAttributesSaveRequested(layerId, attrs);
      if (status)
        status->setText(tr("已提交相属性：%1（图层当前选中要素）").arg(layerId));
    });
    al->addWidget(save);
  }
  lay->addWidget(attrSection);

  // ---- m2(C)：参考图区（06_Reference 组声明图层勾选叠加）----
  lay->addSpacing(16); // spacing.md between groups
  auto *refSection = new CollapsibleSection(tr("参考图"), this);
  refSection->setObjectName(QStringLiteral("referenceArea"));
  {
    QVBoxLayout *rl = refSection->containerLayout();
    auto *refs = new QListWidget(refSection);
    refs->setObjectName(QStringLiteral("referenceList"));
    refs->setAccessibleName(tr("参考图图层列表"));
    connect(refs, &QListWidget::itemChanged, this, [this](QListWidgetItem *item) {
      if (!item)
        return;
      emit referenceVisibilityRequested(item->data(Qt::UserRole).toString(),
                                        item->checkState() == Qt::Checked);
    });
    rl->addWidget(refs);
  }
  lay->addWidget(refSection);

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

void ComposePage::setFaciesEditTarget(const QString &layerId)
{
  setProperty(kFaciesTargetProp, layerId);
  updateFaciesTargetUi(layerId);
}

void ComposePage::updateFaciesTargetUi(const QString &layerId)
{
  if (auto *label = child<QLabel>(this, "faciesTargetLabel"))
    label->setText(layerId.isEmpty()
                       ? tr("尚未矢量化相界图层 — 先「转为相多边形」")
                       : tr("目标层：%1（保存图层当前选中要素）").arg(layerId));
  if (auto *btn = child<QPushButton>(this, "faciesAttrSaveButton"))
  {
    btn->setEnabled(!layerId.isEmpty());
    btn->setToolTip(layerId.isEmpty() ? tr("先矢量化生成相界图层") : QString());
  }
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
    // m2(C)：融合输入只认栅格因素——B 的 contours.*（矢量，子组或平组）
    // 不是融合输入。
    if (!inGroup(d, "04_SingleFactor") || !isRaster(d))
      continue;
    auto *it = new QListWidgetItem(d.layerId, list);
    it->setData(Qt::UserRole, d.layerId);
    it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
    it->setCheckState(Qt::Unchecked);
  }

  auto *combo = child<QComboBox>(this, "faciesRasterCombo");
  if (combo)
  {
    const QString previous = combo->currentData().toString();
    combo->clear();
    for (const LayerDeclaration &d : declared)
    {
      const bool raster = isRaster(d);
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

  // m2(C)：参考图清单（06_Reference 组声明图层；子组同前缀规则）。
  if (auto *refs = child<QListWidget>(this, "referenceList"))
  {
    const QSignalBlocker block(refs); // populate 阶段不发可见性意图
    refs->clear();
    for (const LayerDeclaration &d : declared)
    {
      if (!inGroup(d, "06_Reference"))
        continue;
      auto *it = new QListWidgetItem(d.title.isEmpty() ? d.layerId : d.title, refs);
      it->setToolTip(d.layerId);
      it->setData(Qt::UserRole, d.layerId);
      it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
      it->setCheckState(Qt::Unchecked);
    }
  }

  // m2(C)：相属性目标层——显式指名（壳 setFaciesEditTarget）优先；声明里
  // 唯一的 facies.* 矢量层兜底自动采纳（独立于壳也能用）。
  QStringList faciesDecls;
  for (const LayerDeclaration &d : declared)
    if (inGroup(d, "05_PaleoMap") &&
        d.layerId.startsWith(QLatin1String("facies.")) && !isRaster(d))
      faciesDecls << d.layerId;
  const QString explicitTarget = property(kFaciesTargetProp).toString();
  QString target;
  if (faciesDecls.contains(explicitTarget))
    target = explicitTarget;
  else if (faciesDecls.size() == 1)
    target = faciesDecls.first();
  updateFaciesTargetUi(target);
}
