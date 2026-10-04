// 层：视图
#include "datapreviewtabs.h"
#include "datapreviewtabs_internal.h"

// 共享辅助来自内部头——与 datapreviewtabs.cpp 用同一 using 引入。
using namespace paleo::datapreview_detail;
#include "../paleoviewport.h"
#include "../paleotheme.h" // DESIGN.md token 出口（颜色/字阶/活体样式共用）
#include "../paleoicons.h" // 角落最大化/还原自绘图标
#include "../../catalog/datacatalog.h"
#include "../../domain/seismic/nicestep.h"
#include "../../domain/wellrecords.h"     // WellTopRecord/TimeDepthTable（domain 纯数据）
#include "../../domain/sectiontrace.h"    // SegyTrace/SegySectionGrid（domain 纯数据）
#include "../../io/lasdoc.h"              // LasCurve（白名单：数据模型）
#include "../../services/previewdoc.h"    // 唯一数据门面——解析/解码/SHA/PDF 编排全经它（W1）
#include "../../services/welllogset.h"    // 井曲线并集（综合柱状图；只读 ~C 头）
#include "../../services/paleotaskservice.h" // PaleoTask 进度/取消（地震转码区）
#include "../seismic3d/seismic3dviewpanel.h"
#include "../seismicsection/seismicsectioncanvas.h"
#include "../wellcomposite/wellcompositepanel.h"
#include "../decorations/paleodecorations.h"
#include "previewhistogramwidget.h"
#include "previewmappage.h"
#include "previewmapstates.h"
#include "previewprofilepanel.h"
#include "previewtocpanel.h"
#include "../../qgis/factorcontour.h"
#include "../../qgis/previewrasteranalysis.h"
#include <qgsmapcanvas.h>
#include <qgslayertreemapcanvasbridge.h>
#include <qgsmaptoolpan.h>
#include <qgsproject.h>
#include <qgsrasterbandstats.h>
#include <qgsrasterdataprovider.h>
#include <qgsrasterlayer.h>
#include <qgsrastershader.h>
#include <qgscolorrampshader.h>
#include <qgscolorrampimpl.h>
#include <qgssinglebandpseudocolorrenderer.h>
#include <qgsrubberband.h>
#include <qgsexpression.h>
#include <qgsgeometry.h>
#include <qgsvectorlayer.h>
#include <qgsfields.h>
#include <qgscategorizedsymbolrenderer.h>
#include <qgssinglesymbolrenderer.h>
#include <qgssymbol.h>
#include <qgsfillsymbol.h>
#include <qgsmarkersymbol.h>
#include <qgsmarkersymbollayer.h>
#include <qgslinesymbol.h>
#include <qgspallabeling.h>
#include <qgsvectorlayerlabeling.h>
#include <qgstextbuffersettings.h>
#include <QButtonGroup>
#include <QTimer>
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QCryptographicHash>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QFileInfo>
#include <QHeaderView>
#include <QDesktopServices>
#include <QFile>
#include <QHBoxLayout>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPointer>
#include <QMouseEvent>
#include <QPdfDocument>
#include <QPdfView>
#include <QPixmap>
#include <QPushButton>
#include <QProgressBar>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QToolButton>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTableWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <cmath>
#include <limits>
#include "datapreviewtabs_internal.h"

// 方向20 轮4：buildContent 的「geojson / boundary(.geojson)」分支析出到本文件。
// 匹配条件是**双条件**（geojson 类型，或 boundary 类型且文件名以 .geojson 结尾），
// 由 buildContent 判定后调用本函数——条件留在调用点，函数只管渲染。
// 共享辅助（caption8/qssHex/SectionPanel 等）来自内部头，与主文件同一 using。
// 段内的 page 是本段新建的 PreviewMapPage，与 buildContent 那个被 Q_UNUSED
// 丢弃的 page 形参同名但无关，故本函数签名不收它。

QWidget *DataPreviewTabs::buildGeoJsonContent(
    DataCatalog *cat, const CatalogAsset &asset, const CatalogVersion &v,
    const QString &abs, const QString &assetId, QWidget *host, QVBoxLayout *lay)
{
  QJsonDocument doc;
  QString gerr;
  if (!m_doc->geoJsonDocumentAt(abs, &doc, &gerr))
  {
    lay->addWidget(failureState(assetId, gerr, host), 1);
    return host;
  }
  const QJsonArray features = doc.object().value(QStringLiteral("features")).toArray();
  QStringList propKeys;
  for (const QJsonValue &fv : features)
  {
    const QJsonObject props = fv.toObject()
                                  .value(QStringLiteral("properties"))
                                  .toObject();
    for (auto it = props.begin(); it != props.end(); ++it)
      if (!propKeys.contains(it.key()))
        propKeys.append(it.key());
  }

  // 寻找沉积相分类字段候选 (相、亚相、微相、facies 等)
  QStringList faciesCandidates;
  for (const QString &k : propKeys)
  {
    if (k == QLatin1String("相") || k == QLatin1String("微相") || k == QLatin1String("亚相") ||
        k.contains(QStringLiteral("相")) ||
        k.compare(QLatin1String("facies"), Qt::CaseInsensitive) == 0 ||
        k.compare(QLatin1String("sub_facies"), Qt::CaseInsensitive) == 0 ||
        k.compare(QLatin1String("micro_facies"), Qt::CaseInsensitive) == 0)
    {
      faciesCandidates.append(k);
    }
  }
  if (faciesCandidates.isEmpty())
  {
    for (const QString &k : propKeys)
    {
      if (k.compare(QLatin1String("name"), Qt::CaseInsensitive) == 0 ||
          k.compare(QLatin1String("type"), Qt::CaseInsensitive) == 0 ||
          k.compare(QLatin1String("zone"), Qt::CaseInsensitive) == 0)
      {
        faciesCandidates.append(k);
      }
    }
  }
  if (faciesCandidates.isEmpty() && !propKeys.isEmpty())
    faciesCandidates.append(propKeys.first());

  QString activeFaciesField;
  if (faciesCandidates.contains(QStringLiteral("相")))
    activeFaciesField = QStringLiteral("相");
  else if (!faciesCandidates.isEmpty())
    activeFaciesField = faciesCandidates.first();

  // 矢量层（私有，不进 QgsProject）
  auto *vlayer = new QgsVectorLayer(abs, asset.displayName, QStringLiteral("ogr"));
  vlayer->setParent(host);

  // 顶部操作与空间提示工具栏
  auto *topBar = new QWidget(host);
  auto *topLay = new QHBoxLayout(topBar);
  topLay->setContentsMargins(PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingXs, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingXs);
  topLay->setSpacing(PaleoTheme::tokens().spacingSm);
  stylePreviewToolBar(topBar);

  // 视图切换器: 相图地图 / 属性列表
  auto *btnViewMap = new QToolButton(topBar);
  btnViewMap->setObjectName(QStringLiteral("btnViewFaciesMap"));
  btnViewMap->setText(tr("相图地图"));
  btnViewMap->setCheckable(true);
  btnViewMap->setChecked(true);
  btnViewMap->setIcon(PaleoIcons::qgisTheme(QStringLiteral("mIconPolygonLayer.svg")));
  btnViewMap->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  topLay->addWidget(btnViewMap);

  auto *btnViewTable = new QToolButton(topBar);
  btnViewTable->setObjectName(QStringLiteral("btnViewFaciesTable"));
  btnViewTable->setText(tr("属性列表"));
  btnViewTable->setCheckable(true);
  btnViewTable->setChecked(false);
  btnViewTable->setIcon(PaleoIcons::qgisTheme(QStringLiteral("mActionOpenTable.svg")));
  btnViewTable->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  topLay->addWidget(btnViewTable);

  auto *viewGroup = new QButtonGroup(topBar);
  viewGroup->addButton(btnViewMap);
  viewGroup->addButton(btnViewTable);

  topLay->addSpacing(PaleoTheme::tokens().spacingSm);

  // D2.6 名称标注开关
  auto *btnLabels = new QToolButton(topBar);
  btnLabels->setObjectName(QStringLiteral("btnToggleLabels"));
  btnLabels->setText(tr("名称标注"));
  btnLabels->setToolTip(
      vlayer && vlayer->geometryType() == Qgis::GeometryType::Point
          ? tr("显示或隐藏文字标注。点标记保持统一；有沉积相字段时标出相名")
          : tr("显示/隐藏要素名称标注"));
  btnLabels->setCheckable(true);
  btnLabels->setChecked(true);
  btnLabels->setToolButtonStyle(Qt::ToolButtonTextOnly);
  topLay->addWidget(btnLabels);

  // 地图浏览工具
  auto *btnFull = new QToolButton(topBar);
  btnFull->setObjectName(QStringLiteral("btnFaciesFullExtent"));
  btnFull->setText(tr("全图"));
  btnFull->setToolTip(tr("缩放到相图完整范围"));
  btnFull->setIcon(PaleoIcons::qgisTheme(QStringLiteral("mActionZoomFullExtent.svg")));
  btnFull->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  topLay->addWidget(btnFull);

  auto *btnIn = new QToolButton(topBar);
  btnIn->setObjectName(QStringLiteral("btnFaciesZoomIn"));
  btnIn->setText(tr("放大"));
  btnIn->setIcon(PaleoIcons::qgisTheme(QStringLiteral("mActionZoomIn.svg")));
  btnIn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  topLay->addWidget(btnIn);

  auto *btnOut = new QToolButton(topBar);
  btnOut->setObjectName(QStringLiteral("btnFaciesZoomOut"));
  btnOut->setText(tr("缩小"));
  btnOut->setIcon(PaleoIcons::qgisTheme(QStringLiteral("mActionZoomOut.svg")));
  btnOut->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  topLay->addWidget(btnOut);

  auto *btnPan = new QToolButton(topBar);
  btnPan->setObjectName(QStringLiteral("btnFaciesPan"));
  btnPan->setText(tr("漫游"));
  btnPan->setIcon(PaleoIcons::qgisTheme(QStringLiteral("mActionPan.svg")));
  btnPan->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  topLay->addWidget(btnPan);

  // 字段选择下拉框（若有多个相分类字段）
  QComboBox *fieldCombo = nullptr;
  QLabel *fieldLbl = nullptr;
  if (faciesCandidates.size() > 1)
  {
    fieldLbl = caption8(tr("渲染字段:"), topBar);
    topLay->addWidget(fieldLbl);

    fieldCombo = new QComboBox(topBar);
    fieldCombo->setObjectName(QStringLiteral("faciesFieldCombo"));
    fieldCombo->addItems(faciesCandidates);
    if (!activeFaciesField.isEmpty())
      fieldCombo->setCurrentText(activeFaciesField);
    PaleoTheme::applyThemedStyleSheet(fieldCombo, [] {
      const PaleoTheme::ThemeTokens &t = PaleoTheme::tokens();
      return PaleoTheme::metricStyleSheet(QStringLiteral(
          "QComboBox { background: %1; border: 1px solid %2; border-radius: {rounded.sm}px;"
          " padding: {spacing.xs}px {spacing.sm}px; font-size: {typography.label}pt; color: %3; }"
          "QComboBox:hover { border-color: %4; }"))
          .arg(qssHex(t.surface), qssHex(t.border), qssHex(t.text),
               qssHex(t.textDisabled));
    });
    topLay->addWidget(fieldCombo);
  }

  topLay->addSpacing(PaleoTheme::tokens().spacingSm);

  // D11 临时配准入口
  auto *regBtn = new QPushButton(tr("临时配准（手工仿射）…"), host);
  regBtn->setObjectName(QStringLiteral("provisionalRegisterButton"));
  regBtn->setAccessibleName(tr("临时配准"));
  regBtn->setToolTip(tr("手工输入仿射参数，把 GeoJSON 变换到工程局部测网"));
  PaleoTheme::applyThemedStyleSheet(regBtn, [] {
    const PaleoTheme::ThemeTokens &t = PaleoTheme::tokens();
    return PaleoTheme::metricStyleSheet(QStringLiteral(
        "QPushButton { background: %1; border: 1px solid %2; border-radius: {rounded.sm}px;"
        " padding: {spacing.xs}px {spacing.sm}px; font-size: {typography.label}pt; color: %3; }"
        "QPushButton:hover { background: %4; border-color: %5; }"))
        .arg(qssHex(t.surface), qssHex(t.border), qssHex(t.text),
             qssHex(t.surfaceAltRaised), qssHex(t.textDisabled));
  });
  topLay->addWidget(regBtn);

  topLay->addStretch(1);

  // 经纬度与工程测网不同空间：阻断级提示用 error token（原 #D32F2F + 11px）。
  auto *warnLbl = new QLabel(tr("经纬度，与本测网不是同一空间"), host);
  warnLbl->setWordWrap(true);
  PaleoTheme::applyThemedStyleSheet(warnLbl, [] {
    return PaleoTheme::metricStyleSheet(QStringLiteral("color: %1; font-size: {typography.label}pt; font-weight: 500;"))
        .arg(qssHex(PaleoTheme::tokens().errorText));
  });
  topLay->addWidget(warnLbl);
  lay->addWidget(new PaleoToolRow(topBar, host));

  // 「读不出坐标范围」错误就地可见（原实现创建了警告标签却没加进任何布局）。
  auto *boundsErr = warnLabel(QString(), host);
  boundsErr->setObjectName(QStringLiteral("affineBoundsError"));
  boundsErr->setVisible(false);
  lay->addWidget(boundsErr);

  connect(regBtn, &QPushButton::clicked, this, [this, assetId, abs, boundsErr]() {
    double srcB[4];
    QString berr;
    if (!m_doc->geoJsonBounds(abs, srcB, &berr))
    {
      boundsErr->setText(tr("读不出坐标范围：%1").arg(berr));
      boundsErr->setVisible(true);
      return;
    }
    boundsErr->setVisible(false);

    QDialog dlg(this);
    dlg.setWindowTitle(tr("临时配准（手工仿射）"));
    auto *form = new QFormLayout(&dlg);
    auto *srcLbl = new QLabel(
        tr("源坐标范围：X %1–%2 · Y %3–%4")
            .arg(QString::number(srcB[0], 'f', 2), QString::number(srcB[2], 'f', 2),
                 QString::number(srcB[1], 'f', 2), QString::number(srcB[3], 'f', 2)),
        &dlg);
    srcLbl->setWordWrap(true);
    form->addRow(srcLbl);
    auto *gridHint = new QLabel(
        tr("目标：工程局部测网（约 X 0–12800 · Y 0–16400，单位米）"), &dlg);
    gridHint->setWordWrap(true);
    PaleoTheme::applyThemedStyleSheet(gridHint,
                                      [] { return PaleoTheme::mutedCaptionStyleSheet(); });
    form->addRow(gridHint);

    auto *tx = new QDoubleSpinBox(&dlg);
    auto *ty = new QDoubleSpinBox(&dlg);
    auto *sx = new QDoubleSpinBox(&dlg);
    auto *sy = new QDoubleSpinBox(&dlg);
    auto *rot = new QDoubleSpinBox(&dlg);
    for (auto *s : {tx, ty})
    {
      s->setRange(-1e9, 1e9);
      s->setDecimals(2);
      s->setSingleStep(1000.0);
    }
    for (auto *s : {sx, sy})
    {
      s->setRange(1e-6, 1e6);
      s->setDecimals(6);
      s->setValue(1.0);
    }
    rot->setRange(-360.0, 360.0);
    rot->setDecimals(2);
    form->addRow(tr("平移 X（米）"), tx);
    form->addRow(tr("平移 Y（米）"), ty);
    form->addRow(tr("缩放 X"), sx);
    form->addRow(tr("缩放 Y"), sy);
    form->addRow(tr("旋转（度）"), rot);

    auto *dstLbl = new QLabel(&dlg);
    dstLbl->setObjectName(QStringLiteral("affineDstBounds"));
    dstLbl->setWordWrap(true);
    PaleoTheme::applyThemedStyleSheet(dstLbl,
                                      [] { return PaleoTheme::mutedCaptionStyleSheet(); });
    form->addRow(dstLbl);
    const auto refreshDst = [this, srcB, tx, ty, sx, sy, rot, dstLbl]() {
      const QVariantMap p{{QStringLiteral("tx"), tx->value()},
                          {QStringLiteral("ty"), ty->value()},
                          {QStringLiteral("sx"), sx->value()},
                          {QStringLiteral("sy"), sy->value()},
                          {QStringLiteral("rotDeg"), rot->value()}};
      double lo[2], hi[2];
      m_doc->affinePreviewBounds(srcB, p, lo, hi);
      dstLbl->setText(tr("变换后范围：X %1–%2 · Y %3–%4")
                          .arg(QString::number(lo[0], 'f', 1),
                               QString::number(hi[0], 'f', 1),
                               QString::number(lo[1], 'f', 1),
                               QString::number(hi[1], 'f', 1)));
    };
    for (auto *s : {tx, ty, sx, sy, rot})
      connect(s, qOverload<double>(&QDoubleSpinBox::valueChanged), dstLbl, refreshDst);
    refreshDst();

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("登记为临时配准"));
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted)
      return;

    emit provisionalRegistrationRequested(
        assetId, {{QStringLiteral("tx"), tx->value()},
                  {QStringLiteral("ty"), ty->value()},
                  {QStringLiteral("sx"), sx->value()},
                  {QStringLiteral("sy"), sy->value()},
                  {QStringLiteral("rotDeg"), rot->value()}});
  });

  auto *viewStack = new QStackedWidget(host);
  viewStack->setObjectName(QStringLiteral("faciesViewStack"));

  // 1. P2 统一预览地图页（D1.x 工具/TOC/identify/图例全套）
  auto *page = new PreviewMapPage(viewStack);
  page->setObjectName(QStringLiteral("faciesPreviewPage"));
  page->setAssetKey(assetId);
  page->mapCanvas()->canvas()->setObjectName(QStringLiteral("faciesMapCanvas"));
  page->decorations()->setObjectName(QStringLiteral("faciesDecorManager"));
  page->setProfileEnabled(false); // D1.3：矢量内容不开剖面工具

  if (vlayer && vlayer->isValid())
  {
    applyFaciesRendererToLayer(vlayer, activeFaciesField);
    // 经纬度 GeoJSON：画布跟随层 CRS（旧语义），局部网格层保持工程网格。
    page->mapCanvas()->setOverrideCrs(vlayer->crs());
    // D2.4 图例侧栏：从分类渲染器读回 category 色板。
    const auto syncLegend = [page, vlayer]() {
      QVector<PreviewTocPanel::LegendEntry> entries;
      if (auto *r = dynamic_cast<QgsCategorizedSymbolRenderer *>(vlayer->renderer()))
        for (const QgsRendererCategory &c : r->categories())
        {
          if (!c.symbol())
            continue;
          PreviewTocPanel::LegendEntry e;
          e.name = c.label();
          e.color = c.symbol()->color();
          entries.append(e);
        }
      page->tocPanel()->setLegendEntries(entries);
    };
    syncLegend();
    // D4.4 分类字段快调回调（TOC 面板收集参数，相色逻辑在视图层）。
    page->tocPanel()->vectorStyleApplier =
        [vlayer, syncLegend](QgsVectorLayer *, const QString &field, bool categorized,
                             const QColor &) {
          if (categorized && !field.isEmpty())
          {
            applyFaciesRendererToLayer(vlayer, field);
            syncLegend();
          }
        };
    page->addMapLayer(vlayer, asset.displayName, abs);

    // D2.6 名称标注开关
    connect(btnLabels, &QToolButton::toggled, page, [page, vlayer](bool on) {
      vlayer->setLabelsEnabled(on);
      page->mapCanvas()->canvas()->refresh();
    });
    connect(btnFull, &QToolButton::clicked, page, [page, vlayer]() {
      if (vlayer && !vlayer->extent().isEmpty())
        page->mapCanvas()->zoomToLayer(vlayer);
      else
        page->mapCanvas()->zoomToFullExtent();
    });
    connect(btnIn, &QToolButton::clicked, page,
            [page]() { page->mapCanvas()->canvas()->zoomIn(); });
    connect(btnOut, &QToolButton::clicked, page,
            [page]() { page->mapCanvas()->canvas()->zoomOut(); });
    connect(btnPan, &QToolButton::clicked, page, [page]() {
      page->toolManager()->activate(PreviewMapToolManager::kPan);
    });

    if (fieldCombo)
    {
      connect(fieldCombo, &QComboBox::currentTextChanged, page,
              [vlayer, syncLegend, page](const QString &fld) {
                applyFaciesRendererToLayer(vlayer, fld);
                syncLegend();
                page->mapCanvas()->canvas()->refresh();
              });
    }

    // D2.10 同目录叠加
    addSiblingOverlayButton(cat, abs, assetId, page, host);

    QTimer::singleShot(100, page, [page, vlayer]() {
      if (vlayer && !vlayer->extent().isEmpty())
        page->mapCanvas()->zoomToLayer(vlayer);
      else
        page->mapCanvas()->zoomToFullExtent();
    });
  }
  else
  {
    btnViewMap->setEnabled(false);
    btnViewTable->setChecked(true);
  }

  viewStack->addWidget(page);

  // 2. 要素属性表格预览
  auto *table = new QTableWidget(viewStack);
  table->setObjectName(QStringLiteral("geoJsonFeatureTable"));
  table->setAlternatingRowColors(true);
  table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table->setSelectionBehavior(QAbstractItemView::SelectRows);
  PaleoTheme::applyThemedStyleSheet(table, [] {
    const PaleoTheme::ThemeTokens &t = PaleoTheme::tokens();
    return PaleoTheme::metricStyleSheet(QStringLiteral(
        "QTableWidget { background-color: %1; gridline-color: %2; border: 1px solid %2;"
        " font-size: {typography.body}pt; }"
        "QHeaderView::section { background-color: %3; color: %4; border: none;"
        " border-bottom: 1px solid %2; border-right: 1px solid %2; padding: {spacing.xs}px {spacing.sm}px;"
        " font-weight: 500; font-size: {typography.label}pt; }"))
        .arg(qssHex(t.surface), qssHex(t.border), qssHex(t.surfaceAlt),
             qssHex(t.textMuted));
  });

  QStringList headers;
  headers << tr("序号") << tr("几何类型");
  headers.append(propKeys);
  table->setColumnCount(headers.size());
  table->setHorizontalHeaderLabels(headers);

  const int maxRows = qMin(features.size(), 1000);
  table->setRowCount(maxRows);
  for (int r = 0; r < maxRows; ++r)
  {
    const QJsonObject feat = features.at(r).toObject();
    const QString geomType = feat.value(QStringLiteral("geometry")).toObject().value(QStringLiteral("type")).toString();
    const QJsonObject props = feat.value(QStringLiteral("properties")).toObject();

    auto *idItem = new QTableWidgetItem(QString::number(r + 1));
    idItem->setTextAlignment(Qt::AlignCenter);
    idItem->setFlags(idItem->flags() & ~Qt::ItemIsEditable);
    table->setItem(r, 0, idItem);

    auto *geomItem = new QTableWidgetItem(geomType.isEmpty() ? QStringLiteral("—") : geomType);
    geomItem->setTextAlignment(Qt::AlignCenter);
    geomItem->setFlags(geomItem->flags() & ~Qt::ItemIsEditable);
    table->setItem(r, 1, geomItem);

    for (int c = 0; c < propKeys.size(); ++c)
    {
      const QString &key = propKeys.at(c);
      const QJsonValue val = props.value(key);
      QString valStr;
      if (val.isDouble())
        valStr = QString::number(val.toDouble());
      else if (val.isString())
        valStr = val.toString();
      else if (val.isBool())
        valStr = val.toBool() ? QStringLiteral("true") : QStringLiteral("false");
      else if (val.isNull())
        valStr = QStringLiteral("null");
      else
        valStr = QString::fromUtf8(QJsonDocument(val.toArray()).toJson(QJsonDocument::Compact));

      auto *valItem = new QTableWidgetItem(valStr);
      valItem->setFlags(valItem->flags() & ~Qt::ItemIsEditable);
      table->setItem(r, c + 2, valItem);
    }
  }
  table->horizontalHeader()->setStretchLastSection(true);
  table->resizeColumnsToContents();

  // 表格放进容器页：超 1000 条截断时表尾如实注明（原实现静默截断）。
  auto *tablePage = new QWidget(viewStack);
  auto *tablePageLay = new QVBoxLayout(tablePage);
  tablePageLay->setContentsMargins(0, 0, 0, 0);
  tablePageLay->setSpacing(PaleoTheme::tokens().spacingXs);
  tablePageLay->addWidget(table, 1);
  if (features.size() > maxRows)
    tablePageLay->addWidget(
        caption8(tr("已截断，仅显示前 %1 条（共 %2 条）")
                     .arg(maxRows)
                     .arg(features.size()),
                 tablePage));
  viewStack->addWidget(tablePage);

  const auto updateViewMode = [viewStack, btnFull, btnIn, btnOut, btnPan, btnLabels, fieldLbl, fieldCombo](int idx) {
    viewStack->setCurrentIndex(idx);
    const bool isMap = (idx == 0);
    btnFull->setVisible(isMap);
    btnIn->setVisible(isMap);
    btnOut->setVisible(isMap);
    btnPan->setVisible(isMap);
    btnLabels->setVisible(isMap);
    if (fieldLbl) fieldLbl->setVisible(isMap);
    if (fieldCombo) fieldCombo->setVisible(isMap);
  };

  connect(btnViewMap, &QToolButton::clicked, host, [updateViewMode]() { updateViewMode(0); });
  connect(btnViewTable, &QToolButton::clicked, host, [updateViewMode]() { updateViewMode(1); });

  lay->addWidget(viewStack, 1);
  return host;
}
