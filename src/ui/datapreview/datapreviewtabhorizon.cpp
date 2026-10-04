// 层：视图
// token 例外：DESIGN 数据符号例外：QGIS 极值点高/低两组地质数据符号。（tools/ui-token-exceptions.json 精确计数）。
#include "datapreviewtabs.h"
#include "datapreviewtabs_internal.h"

// 共享辅助来自内部头——与 datapreviewtabs.cpp 用同一 using 引入。
using namespace paleo::datapreview_detail;
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

// 方向20 轮4：buildContent 的「horizon」分支按资产类型析出到本文件。
// 共享辅助（caption8/qssHex/SectionPanel 等）来自内部头，与主文件同一 using。
// 参数是 buildContent 已算好的量；注意段内的 page 是**本段新建**的
// PreviewMapPage（与 buildContent 那个被 Q_UNUSED 丢弃的 page 参数同名
// 但无关），故本函数签名里不收它。

QWidget *DataPreviewTabs::buildHorizonContent(
    DataCatalog *cat, const CatalogAsset &asset, const CatalogVersion &v,
    const QString &assetId, const QString &linkedBoundary,
    QWidget *host, QVBoxLayout *lay)
{
  const CatalogEntity sb =
      linkedBoundary.isEmpty() ? CatalogEntity() : cat->entityById(linkedBoundary);
  const QString pendingNote = sb.extra.value(QStringLiteral("pending")).toBool()
                                  ? tr("未决层位 — 不进入编图 chip")
                                  : QString();
  // D2.9 版本集合：DERIVED 栅格（多次生成按号降序）+ RAW 散点。
  QVector<CatalogVersion> deriveds;
  CatalogVersion raw;
  for (const CatalogVersion &cv : cat->versionsForAsset(assetId))
  {
    if (cv.stage == QLatin1String("DERIVED"))
      deriveds.append(cv);
    else if (cv.stage == QLatin1String("RAW") && cv.versionNumber >= raw.versionNumber)
      raw = cv;
  }
  std::sort(deriveds.begin(), deriveds.end(),
            [](const CatalogVersion &a, const CatalogVersion &b) {
              return a.versionNumber > b.versionNumber;
            });
  const CatalogVersion derived = deriveds.isEmpty() ? CatalogVersion() : deriveds.first();
  const QString gridTxt =
      derived.id.isEmpty()
          ? tr("派生栅格：未生成")
          : tr("网格 %1×%2 · Z %3 %4–%5 · 拒绝 %6 · 碰撞 %7")
                .arg(derived.extra.value(QStringLiteral("grid_rows")).toInt())
                .arg(derived.extra.value(QStringLiteral("grid_cols")).toInt())
                .arg(derived.extra.value(QStringLiteral("z_units")).toString(),
                     QString::number(derived.extra.value(QStringLiteral("z_min")).toDouble(), 'f', 1),
                     QString::number(derived.extra.value(QStringLiteral("z_max")).toDouble(), 'f', 1))
                .arg(derived.extra.value(QStringLiteral("rejected")).toInt())
                .arg(derived.extra.value(QStringLiteral("collisions")).toInt());
  // In the workbench, metadata and version controls live in Data Properties.
  // Standalone previews retain the same controls locally.
  auto *details = new QWidget(m_detailsHost ? m_detailsHost.data() : host);
  details->setObjectName(QStringLiteral("horizonPreviewDetails"));
  auto *detailLayout = new QVBoxLayout(details);
  detailLayout->setContentsMargins(0, 0, 0, 0);
  detailLayout->setSpacing(PaleoTheme::tokens().spacingXs);
  if (m_detailsHost) {
    m_detailsHost->layout()->addWidget(details);
    connect(host, &QObject::destroyed, details, &QObject::deleteLater);
    m_detailsOfAsset.insert(assetId, details);
    syncDetails();
  } else {
    lay->addWidget(details);
  }
  detailLayout->addWidget(caption8(tr("层位 %1").arg(sb.name.isEmpty() ? asset.displayName : sb.name), details));
  auto *grid = new QLabel(gridTxt, details);
  PaleoTheme::applyThemedStyleSheet(grid, [] {
    return QStringLiteral("color: %1;").arg(qssHex(PaleoTheme::tokens().text));
  });
  grid->setWordWrap(true);
  detailLayout->addWidget(grid);
  if (!pendingNote.isEmpty())
  {
    auto *p = warnLabel(pendingNote, details);
    detailLayout->addWidget(p);
  }
  // 「在地图上显示」（§4/T29，语义原样）。
  auto *btn = new QPushButton(tr("在地图上显示"), details);
  btn->setObjectName(QStringLiteral("showOnMapBtn"));
  btn->setAccessibleName(tr("在地图上显示层位 %1").arg(sb.name.isEmpty()
                                                            ? asset.displayName
                                                            : sb.name));
  if (derived.id.isEmpty() || sb.name.isEmpty())
  {
    btn->setEnabled(false);
    btn->setToolTip(tr("还没有这个层位的栅格"));
  }
  else
  {
    const QString layerId = QStringLiteral("horizon.%1").arg(sb.name);
    btn->setProperty("layerId", layerId); // T29：双向同步按 layerId 寻址
    connect(btn, &QPushButton::clicked, this, [this, layerId]() {
      emit showHorizonOnMapRequested(layerId);
    });
  }
  detailLayout->addWidget(btn, 0, Qt::AlignLeft);

  // ---- D2.9 版本切换：≥2 个版本才给下拉；选 RAW → 散点信息卡。
  CatalogVersion chosen;
  const QString chosenId = m_chosenVersionOfAsset.value(assetId);
  for (const CatalogVersion &cv : deriveds)
    if (cv.id == chosenId)
      chosen = cv;
  if (chosen.id.isEmpty() && !raw.id.isEmpty() && raw.id == chosenId)
    chosen = raw;
  if (chosen.id.isEmpty() && !chosenId.isEmpty() && v.id == chosenId) chosen = v;
  if (chosen.id.isEmpty())
    chosen = derived.id.isEmpty() ? raw : derived;
  const int totalVersions = deriveds.size() + (raw.id.isEmpty() ? 0 : 1);
  if (totalVersions > 1)
  {
    auto *verBar = new QWidget(details);
    auto *verLay = new QHBoxLayout(verBar);
    verLay->setContentsMargins(0, 0, 0, 0);
    verLay->addWidget(caption8(tr("版本"), verBar));
    auto *verCombo = new QComboBox(verBar);
    verCombo->setObjectName(QStringLiteral("previewVersionCombo"));
    for (const CatalogVersion &cv : deriveds)
      verCombo->addItem(tr("派生栅格 v%1").arg(cv.versionNumber), cv.id);
    if (!raw.id.isEmpty())
      verCombo->addItem(tr("原始散点 · %1").arg(raw.fileName), raw.id);
    if (verCombo->findData(chosen.id) < 0)
      verCombo->addItem(tr("版本 v%1 · %2").arg(chosen.versionNumber).arg(chosen.stage), chosen.id);
    const int wantIdx = verCombo->findData(chosen.id);
    if (wantIdx >= 0)
      verCombo->setCurrentIndex(wantIdx);
    connect(verCombo, &QComboBox::currentIndexChanged, host,
            [this, assetId, verCombo](int idx) {
              const QString vid = verCombo->itemData(idx).toString();
              if (!vid.isEmpty() && m_chosenVersionOfAsset.value(assetId) != vid)
              {
                m_chosenVersionOfAsset[assetId] = vid;
                rebuildAssetTab(assetId); // 画布即时切换（D2.9）
                emit versionContextChanged(assetId, vid);
              }
            });
    verLay->addWidget(verCombo);
    verLay->addStretch(1);
    detailLayout->addWidget(verBar);
  }

  const bool chosenIsDerived = chosen.stage != QLatin1String("RAW");
  if (!chosenIsDerived)
  {
    // RAW 散点：如实给文件信息卡——散点解析属 io 层，视图不造假地图。
    lay->addWidget(caption8(tr("原始散点文件"), host));
    lay->addWidget(valueLabel(m_doc->absolutePathForVersion(chosen), host, false));
    lay->addWidget(stateLabel(tr("选中「派生栅格」版本可看地图预览"), host), 1);
    return host;
  }

  const QString tifPath = m_doc->absolutePathForVersion(chosen);
  auto raster = std::make_unique<QgsRasterLayer>(
      tifPath, sb.name.isEmpty() ? asset.displayName : sb.name, QStringLiteral("gdal"));
  if (!raster->isValid() || raster->extent().isEmpty())
  {
    // D1.7：数据源损坏给原因页，不给白画布。
    lay->addWidget(PreviewMapStates::buildErrorPage(
                       tr("无法读取层位栅格"), tifPath, host, tr("重试"),
                       [this, assetId] { rebuildAssetTab(assetId); }),
                   1);
    return host;
  }
  // D2.11 大图（>50MB 无金字塔）提示条：降级仍可用（低清先行 + 全图照渲）。
  addRasterPyramidHint(m_doc, raster.get(), assetId, host, lay);

  // ---- P2 地图正文：统一 PreviewMapPage（D1.x 框架全套） ----
  auto *page = new PreviewMapPage(host);
  page->setObjectName(QStringLiteral("horizonPreviewPage"));
  page->setAssetKey(assetId);
  page->setRenderCacheIdentity(assetId, chosen.id);
  page->mapCanvas()->canvas()->setObjectName(QStringLiteral("horizonMapCanvas"));
  page->decorations()->setObjectName(QStringLiteral("horizonDecorManager"));

  const auto sum = PreviewRasterAnalysis::summarize(raster.get());
  if (sum.valid)
    PreviewRasterAnalysis::applyPseudoColorRenderer(
        raster.get(), 1, sum.min, sum.max,
        *PreviewRasterAnalysis::rampPreset(QStringLiteral("depthBlues")), false,
        PreviewRasterAnalysis::Classification::Continuous);
  QgsRasterLayer *rasterRaw = raster.release();
  rasterRaw->setParent(host); // 私有层父子树托管（既有约定：不进 QgsProject）
  page->addMapLayer(rasterRaw, sb.name.isEmpty() ? asset.displayName : sb.name, tifPath);

  // ---- D2.1 等值线 overlay + D5.7 参数化（间距/标注密度） ----
  auto contourDir = std::make_shared<QTemporaryDir>();
  auto currentContour = std::make_shared<QgsVectorLayer *>(nullptr);
  const auto rebuildContours =
      [page, rasterRaw, tifPath, contourDir, currentContour, host](double interval, int density) {
        if (*currentContour)
        {
          page->removeMapLayer(*currentContour); // 旧层出树（父子托管，deleteLater 由父管）
          (*currentContour)->deleteLater();
          *currentContour = nullptr;
        }
        const QString gpkg =
            contourDir->filePath(QStringLiteral("contours_%1.gpkg").arg(interval));
        QString cerr;
        if (!FactorContourService::generateContours(tifPath, gpkg, interval, &cerr))
          return;
        auto *cl = makeContourLayer(gpkg, host, density);
        if (!cl->isValid())
        {
          cl->deleteLater();
          return;
        }
        *currentContour = cl;
        page->addMapLayer(cl, QObject::tr("等值线"), gpkg);
      };
  rebuildContours(10.0, 1);

  // ---- D5.5 统计面板 ----
  {
    auto *statsPage = new QWidget(page);
    auto *statsLay = new QFormLayout(statsPage);
    const auto addStat = [&statsLay, statsPage](const QString &k, const QString &v,
                                                const QString &objectName) {
      auto *l = new QLabel(v, statsPage);
      l->setObjectName(objectName);
      l->setFont(monoFont());
      l->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
      statsLay->addRow(k, l);
    };
    addStat(tr("最小值"), QString::number(sum.min, 'f', 2), QStringLiteral("horizonStatMin"));
    addStat(tr("最大值"), QString::number(sum.max, 'f', 2), QStringLiteral("horizonStatMax"));
    addStat(tr("均值"), QString::number(sum.mean, 'f', 2), QStringLiteral("horizonStatMean"));
    addStat(tr("标准差"), QString::number(sum.stdDev, 'f', 2), QStringLiteral("horizonStatStd"));
    addStat(tr("有效像元占比"), QStringLiteral("%1%").arg(sum.validRatio() * 100.0, 0, 'f', 1),
            QStringLiteral("horizonStatValid"));
    page->addAnalysisTab(tr("统计"), statsPage);
  }

  // ---- D2.3/D5.8 直方图（分箱可调/对数纵轴；拉伸界随符号快调联动） ----
  {
    auto *histPage = new QWidget(page);
    auto *histLay = new QVBoxLayout(histPage);
    histLay->setContentsMargins(PaleoTheme::tokens().spacingXs, PaleoTheme::tokens().spacingXs, PaleoTheme::tokens().spacingXs, PaleoTheme::tokens().spacingXs);
    auto *hist = new PreviewHistogramWidget(false, histPage);
    hist->setObjectName(QStringLiteral("horizonHistogram"));
    const auto refreshHist = [hist, rasterRaw](int bins) {
      hist->setHistogram(PreviewRasterAnalysis::histogram(rasterRaw, bins));
      // 当前渲染界（TOC 快调后随 renderer 读回）。
      if (auto *r = dynamic_cast<QgsSingleBandPseudoColorRenderer *>(rasterRaw->renderer()))
        if (auto *fn = r->shader()->rasterShaderFunction())
          hist->setStretchMarks(fn->minimumValue(), fn->maximumValue());
    };
    refreshHist(64);
    QObject::connect(hist, &PreviewHistogramWidget::binsChanged, histPage, refreshHist);
    QObject::connect(page->tocPanel(), &PreviewTocPanel::rasterStyleChanged, histPage,
                     [refreshHist, hist] { refreshHist(hist->bins()); });
    histLay->addWidget(hist, 1);
    page->addAnalysisTab(tr("直方图"), histPage);
  }

  // ---- D5.7 等值线参数 ----
  {
    auto *contourPage = new QWidget(page);
    auto *cform = new QFormLayout(contourPage);
    auto *intervalSpin = new QDoubleSpinBox(contourPage);
    intervalSpin->setObjectName(QStringLiteral("contourIntervalSpin"));
    intervalSpin->setRange(0.5, 1000.0);
    intervalSpin->setDecimals(1);
    intervalSpin->setValue(10.0);
    intervalSpin->setSuffix(tr(" m"));
    cform->addRow(tr("等值线间距"), intervalSpin);
    auto *densityCombo = new QComboBox(contourPage);
    densityCombo->setObjectName(QStringLiteral("contourDensityCombo"));
    densityCombo->addItem(tr("全部"), 2);
    densityCombo->addItem(tr("稀疏"), 1);
    densityCombo->addItem(tr("关"), 0);
    densityCombo->setCurrentIndex(1);
    cform->addRow(tr("标注密度"), densityCombo);
    const auto applyContours = [rebuildContours, intervalSpin, densityCombo]() {
      rebuildContours(intervalSpin->value(), densityCombo->currentData().toInt());
    };
    QObject::connect(intervalSpin, &QDoubleSpinBox::valueChanged, contourPage,
                     [applyContours](double) { applyContours(); });
    QObject::connect(densityCombo, &QComboBox::currentIndexChanged, contourPage,
                     [applyContours](int) { applyContours(); });
    page->addAnalysisTab(tr("等值线"), contourPage);
  }

  // ---- D5.6 局部极值（峰值/洼地标注开关 + 前列清单） ----
  {
    auto *extPage = new QWidget(page);
    auto *extLay = new QVBoxLayout(extPage);
    auto *extBar = new QWidget(extPage);
    auto *extBarLay = new QHBoxLayout(extBar);
    extBarLay->setContentsMargins(0, 0, 0, 0);
    auto *peakCheck = new QCheckBox(tr("峰值"), extBar);
    peakCheck->setObjectName(QStringLiteral("extremaPeakCheck"));
    auto *lowCheck = new QCheckBox(tr("洼地"), extBar);
    lowCheck->setObjectName(QStringLiteral("extremaLowCheck"));
    extBarLay->addWidget(peakCheck);
    extBarLay->addWidget(lowCheck);
    extBarLay->addStretch(1);
    extLay->addWidget(extBar);
    auto *extTable = new QTableWidget(0, 4, extPage);
    extTable->setObjectName(QStringLiteral("extremaTable"));
    extTable->setHorizontalHeaderLabels({tr("类型"), tr("X"), tr("Y"), tr("值")});
    extTable->verticalHeader()->setVisible(false);
    extTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    extLay->addWidget(extTable, 1);

    auto *peaksBand = new QgsRubberBand(page->mapCanvas()->canvas(), Qgis::GeometryType::Point);
    peaksBand->setParent(page->mapCanvas()->canvas());
    peaksBand->setObjectName(QStringLiteral("horizonPeaksBand"));
    peaksBand->setColor(QColor(217, 38, 38));   // error 红系（语义：高凸）
    peaksBand->setIcon(QgsRubberBand::ICON_CIRCLE);
    peaksBand->setIconSize(6);
    peaksBand->hide();
    auto *lowsBand = new QgsRubberBand(page->mapCanvas()->canvas(), Qgis::GeometryType::Point);
    lowsBand->setParent(page->mapCanvas()->canvas());
    lowsBand->setObjectName(QStringLiteral("horizonLowsBand"));
    lowsBand->setColor(QColor(21, 118, 210));   // 蓝（语义：低洼）
    lowsBand->setIcon(QgsRubberBand::ICON_CIRCLE);
    lowsBand->setIconSize(6);
    lowsBand->hide();

    const auto refreshExtrema = [rasterRaw, peakCheck, lowCheck, peaksBand, lowsBand, extTable, sum]() {
      peaksBand->reset(Qgis::GeometryType::Point);
      lowsBand->reset(Qgis::GeometryType::Point);
      extTable->setRowCount(0);
      if (!peakCheck->isChecked() && !lowCheck->isChecked())
        return;
      const double prom = (sum.valid ? sum.stdDev : 0.0) * 0.5; // 显著性 = 半个标准差
      const auto extrema =
          PreviewRasterAnalysis::localExtrema(rasterRaw, 1, 5, prom, 100);
      for (const auto &e : extrema)
      {
        if (e.peak)
          peaksBand->addPoint(e.pos, false);
        else
          lowsBand->addPoint(e.pos, false);
        const int r = extTable->rowCount();
        extTable->insertRow(r);
        auto *typeItem = new QTableWidgetItem(e.peak ? tr("峰") : tr("洼"));
        auto *xItem = new QTableWidgetItem(QString::number(e.pos.x(), 'f', 1));
        auto *yItem = new QTableWidgetItem(QString::number(e.pos.y(), 'f', 1));
        auto *vItem = new QTableWidgetItem(QString::number(e.value, 'f', 2));
        for (auto *it : {xItem, yItem, vItem})
          setNumericItem(it);
        extTable->setItem(r, 0, typeItem);
        extTable->setItem(r, 1, xItem);
        extTable->setItem(r, 2, yItem);
        extTable->setItem(r, 3, vItem);
      }
      peaksBand->setVisible(peakCheck->isChecked());
      lowsBand->setVisible(lowCheck->isChecked());
    };
    QObject::connect(peakCheck, &QCheckBox::toggled, extPage,
                     [refreshExtrema](bool) { refreshExtrema(); });
    QObject::connect(lowCheck, &QCheckBox::toggled, extPage,
                     [refreshExtrema](bool) { refreshExtrema(); });
    page->addAnalysisTab(tr("极值"), extPage);
  }

  // ---- D5.1 剖面：画布拖线 → 沿线采样 → 剖面图（D5.2 悬停读数在面板内） ----
  QObject::connect(page, &PreviewMapPage::profileLineDrawn, host,
                   [page, rasterRaw](const QgsPointXY &p1, const QgsPointXY &p2, int total) {
                     const auto samples = PreviewRasterAnalysis::sampleProfile(rasterRaw, p1, p2, 200);
                     QVector<PreviewProfilePanel::Sample> pts;
                     pts.reserve(samples.size());
                     for (const auto &sm : samples)
                       pts.append({sm.distance, sm.value, sm.valid});
                     page->profilePanel()->addProfile(tr("剖面 %1").arg(total), pts, p1, p2);
                   });
  page->setProfileEnabled(true); // D1.3：栅格内容才开剖面工具

  // ---- D2.10 同目录组图：同目录可地图化资产一键叠加 ----
  addSiblingOverlayButton(cat, tifPath, assetId, page, host);

  lay->addWidget(page, 1);

  // ---- D6.1/D6.2/D6.7：全图复位 → 缓存命中即上屏；未命中低清先行。 ----
  auto *openTimer = new QElapsedTimer();
  openTimer->start();
  QTimer::singleShot(0, host, [page, openTimer]() {
    page->mapCanvas()->zoomToFullExtent();
    page->primeRenderCache();
    if (!page->mapCanvas()->overlayVisible())
      page->showLowResSnapshot(); // D6.1 低清整图先上（后台精渲随后替换）
    openTimer->invalidate();
    delete openTimer;
  });
  return host;
}
