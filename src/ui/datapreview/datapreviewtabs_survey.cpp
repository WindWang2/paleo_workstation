// 层：视图
// token 例外：DESIGN 数据符号例外：QGIS 测区轮廓与透明填充，不改变地图符号色。（tools/ui-token-exceptions.json 精确计数）。
#include "datapreviewtabs.h"
#include "projectstatisticspage.h"

#include <QTabWidget>
#include "datapreviewtabs_internal.h"

#include "../paleoicons.h"
#include "../paleoviewport.h"                // PaleoToolRow
#include "../../qgis/projectmapreference.h"  // 底图克隆/版权/范围/测区 CRS（paleo::mapreference）
#include <qgsmapcanvas.h>
#include <qgslayertree.h>
#include <qgsproject.h>
#include <QAction>
#include "../../qgis/wellpointnavigation.h"
#include <QTimer>

#include <cmath>
#include <memory>

// 共享辅助来自内部头——与 datapreviewtabs.cpp 用同一 using 引入（族内先例：
// datapreviewtabworkbook/tabxml 同款）。
using namespace paleo::datapreview_detail;

void DataPreviewTabs::openSurveyArea()
{
  const QString key = QStringLiteral("survey_area");
  // 页内容按建页时刻快照 catalog 井位与工程底图克隆——测区节点在
  // 树里常驻、随时可激活，页可能早于数据就绪落成降级态（0 井、10×10 默认
  // 范围、无底图）。激活一律按现状整体重建：旧复用路径只能回灌井位，底图
  // 永不合愈。开销与初次建页相同（内存层 + 底图克隆）。
  if (m_pageOfAsset.contains(key))
    closeAssetTab(key);

  QWidget *page = new QWidget(this);
  auto *pageLay = new QVBoxLayout(page);
  pageLay->setContentsMargins(0, 0, 0, 0);
  pageLay->setSpacing(0);

  QWidget *content = buildSurveyAreaContent(page);
  pageLay->addWidget(content ? content : stateLabel(tr("无法生成测区地图"), page, true), 1);
  if (content) attachOnlineBasemap(content);

  const int idx = m_tabs->addTab(page, PaleoIcons::qgisTheme(QStringLiteral("mIconPolygonLayer.svg")), tr("测区全景地图"));
  m_pageOfAsset.insert(key, page);
  m_tabs->setVisible(true);
  m_emptyLabel->setVisible(false);
  m_tabs->setCurrentIndex(idx);
}

void DataPreviewTabs::openProjectDashboard()
{
  const QString key = QStringLiteral("project_dashboard");
  if (auto *existing = m_pageOfAsset.value(key)) {
    m_tabs->setCurrentWidget(existing); return;
  }
  auto *page = new ProjectStatisticsPage(m_doc ? m_doc->catalog() : nullptr, m_doc, this);
  connect(page, &ProjectStatisticsPage::surveyMapRequested, this, &DataPreviewTabs::openSurveyArea);
  const int index = m_tabs->addTab(page, PaleoIcons::qgisTheme(QStringLiteral("mLayoutItemChart.svg")), tr("工区看板"));
  m_pageOfAsset.insert(key, page);
  m_emptyLabel->hide(); m_tabs->show(); m_tabs->setCurrentIndex(index);
}

namespace {
void requestSurveyAreaRange(PreviewDocService *doc,PreviewMapPage *mapPage,QLabel *crsLabel,
  const std::shared_ptr<QgsRectangle> &completeExtent,const std::shared_ptr<QgsCoordinateReferenceSystem> &extentCrs,
  const std::function<void()> &zoomFull) {
  if(doc) {
    QPointer<PreviewMapPage> guard(mapPage);
    doc->requestProjectPlanarRange([guard,completeExtent,extentCrs,zoomFull,crsLabel](const AreaPlanarRanges &ranges) {
      if(!guard)return;
      const auto &range=ranges.merged;
      const QgsCoordinateReferenceSystem crs(range.coordinateSystem);
      if(!range.valid||!crs.isValid()) {
        crsLabel->setToolTip(QObject::tr("完整工区范围尚不可用：%1").arg(ranges.status));return;
      }
      const QgsRectangle extent(range.minX,range.minY,range.maxX,range.maxY);
      if(auto *layer=paleo::mapreference::areaRangeLayer(extent,crs,QObject::tr("工区范围"),PaleoTheme::tokens().textMuted,guard)) {
        layer->setObjectName(QStringLiteral("surveyAreaRangeLayer"));
        guard->addMapLayer(layer,layer->name(),QString());
        *completeExtent=extent;*extentCrs=crs;
        crsLabel->setToolTip(QObject::tr("完整工区范围：井口与地震数据范围并集 · %1").arg(range.coordinateSystem));
        zoomFull();
      }
    });
  }

}
}

QWidget *DataPreviewTabs::buildSurveyAreaContent(QWidget *page)
{
  auto *w = new QWidget(page);
  auto *lay = new QVBoxLayout(w);
  lay->setContentsMargins(0, 0, 0, 0);
  lay->setSpacing(0);

  // 顶部快捷控制条（遵照 DESIGN.md 设计规范；token 活体样式见 stylePreviewToolBar）
  auto *topBar = new QWidget(w);
  auto *tbLay = new QHBoxLayout(topBar);
  tbLay->setContentsMargins(PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingXs, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingXs);
  tbLay->setSpacing(PaleoTheme::tokens().spacingSm);
  stylePreviewToolBar(topBar);

  auto *lblTitle = new QLabel(tr("测区全景地图 (QGIS 画布)"), topBar);
  PaleoTheme::applyThemedStyleSheet(lblTitle, [] {
    return PaleoTheme::metricStyleSheet(QStringLiteral("font-weight: 600; color: %1; font-size: {typography.body}pt;"))
        .arg(qssHex(PaleoTheme::tokens().text));
  });
  tbLay->addWidget(lblTitle);

  tbLay->addSpacing(PaleoTheme::tokens().spacingSm);

  const auto navButton = [&](const char *name, const QString &text, const QString &tip,
                             const QString &icon) {
    auto *btn = new QToolButton(topBar);
    btn->setObjectName(QLatin1String(name));
    btn->setText(text);
    btn->setToolTip(tip);
    btn->setIcon(PaleoIcons::qgisTheme(icon));
    btn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    tbLay->addWidget(btn);
    return btn;
  };
  auto *btnFull = navButton("btnSurveyFullExtent", tr("全图"), tr("缩放到测区全景范围"),
                            QStringLiteral("mActionZoomFullExtent.svg"));
  auto *btnIn = navButton("btnSurveyZoomIn", tr("放大"), tr("放大地图 (支持鼠标滚轮缩放)"),
                          QStringLiteral("mActionZoomIn.svg"));
  auto *btnOut = navButton("btnSurveyZoomOut", tr("缩小"), tr("缩小地图"),
                           QStringLiteral("mActionZoomOut.svg"));
  auto *btnPan = navButton("btnSurveyPan", tr("漫游"), tr("按住鼠标左键拖拽平移地图"),
                           QStringLiteral("mActionPan.svg"));

  // 1. 工区概况图应该有比例尺，指南针等显示
  auto *btnScaleBar = new QToolButton(topBar);
  btnScaleBar->setObjectName(QStringLiteral("btnToggleScaleBar"));
  btnScaleBar->setText(tr("比例尺"));
  btnScaleBar->setToolTip(tr("开启/关闭左下角动态比例尺"));
  btnScaleBar->setCheckable(true);
  btnScaleBar->setChecked(true);
  btnScaleBar->setToolButtonStyle(Qt::ToolButtonTextOnly);
  tbLay->addWidget(btnScaleBar);

  auto *btnNorthArrow = new QToolButton(topBar);
  btnNorthArrow->setObjectName(QStringLiteral("btnToggleNorthArrow"));
  btnNorthArrow->setText(tr("指南针"));
  btnNorthArrow->setToolTip(tr("开启/关闭右上角指北针"));
  btnNorthArrow->setCheckable(true);
  btnNorthArrow->setChecked(true);
  btnNorthArrow->setToolButtonStyle(Qt::ToolButtonTextOnly);
  tbLay->addWidget(btnNorthArrow);

  auto *btnGrid = new QToolButton(topBar);
  btnGrid->setObjectName(QStringLiteral("btnToggleGrid"));
  btnGrid->setText(tr("网格"));
  btnGrid->setToolTip(tr("开启/关闭坐标方格网"));
  btnGrid->setCheckable(true);
  btnGrid->setChecked(false);
  btnGrid->setToolButtonStyle(Qt::ToolButtonTextOnly);
  tbLay->addWidget(btnGrid);

  auto *btnSwitchMain = new QToolButton(topBar);
  btnSwitchMain->setObjectName(QStringLiteral("btnSwitchToMainCanvas"));
  btnSwitchMain->setText(tr("在主画布中查看"));
  btnSwitchMain->setToolTip(tr("切换到主工作区全屏 QGIS 地图画布"));
  btnSwitchMain->setIcon(PaleoIcons::qgisTheme(QStringLiteral("mActionMapSettings.svg")));
  btnSwitchMain->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  tbLay->addWidget(btnSwitchMain);


  tbLay->addStretch(1);

  // P2：测区全景走统一 PreviewMapPage（工具条/状态条/鹰眼/书签/TOC 全套）。
  auto *mapPage = new PreviewMapPage(w);
  mapPage->setTaskService(m_doc ? m_doc->taskService() : nullptr);
  mapPage->setObjectName(QStringLiteral("surveyPreviewPage"));
  mapPage->setProfileEnabled(false); // 全景浏览页不开剖面工具
  QgsMapCanvas *canvas = mapPage->mapCanvas()->canvas();
  canvas->setObjectName(QStringLiteral("surveyMapCanvas"));
  const auto surveyCrs = paleo::mapreference::confirmedSurveyCrs(m_project);
  mapPage->mapCanvas()->setOverrideCrs(surveyCrs);
  if (m_project) {
    canvas->setProject(m_project);
    canvas->mapSettings().setTransformContext(m_project->transformContext());
    mapPage->mapCanvas()->setOverrideCrs(surveyCrs);
    // 独立底图实例归预览页持有，关闭标签不影响主地图。
    const auto layers = m_project->layerTreeRoot()->findLayers();
    for (auto it = layers.crbegin(); it != layers.crend(); ++it) {
      auto *layer = (*it)->layer();
      if (!layer || !layer->customProperty("paleoBasemap").toBool()) continue;
      if (auto *base = paleo::mapreference::offlineBasemap(layer->customProperty("paleoBasemapPath").toString(), layer->name(), canvas))
        mapPage->addMapLayer(base, base->name(), QString());
    }
    auto *sources = new QLabel(paleo::mapreference::attribution(canvas), w);
    sources->setObjectName(QStringLiteral("surveyBasemapAttribution"));
    sources->setWordWrap(true);
    PaleoTheme::applyThemedStyleSheet(sources, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
    lay->addWidget(sources);
  }

  // 全景页不出鹰眼（固定幅面不需要总览缩略图），工具条开关一并摘掉。
  if (auto *ovAction = mapPage->findChild<QAction *>(QStringLiteral("previewOverviewAction")))
  {
    ovAction->setChecked(false); // toggled → setOverviewVisible(false)
    ovAction->setVisible(false);
  }
  else
    mapPage->setOverviewVisible(false);

  // 全景只显示私有井位层及 Data Service 的完整工区范围，不桥接其它图层。
  auto *wellsVl = makeMemoryPointLayer(tr("井位"), w);
  wellsVl->setObjectName(QStringLiteral("surveyWellsLayer"));
  wellsVl->setCrs(surveyCrs);
  QgsRectangle wellsExtent = loadSurveyWells(wellsVl);
  styleSurveyWellLayer(wellsVl);
  mapPage->addMapLayer(wellsVl, tr("井位"), QString()); wireWellNavigation(mapPage, wellsVl);

  // 装饰管理器（页内建，测区命名保持既有测试面）
  PaleoDecorationManager *decorMgr = mapPage->decorations();
  decorMgr->setObjectName(QStringLiteral("surveyAreaDecorManager"));
  decorMgr->setScaleBarEnabled(true);
  decorMgr->setNorthArrowEnabled(true);

  // 先显示井位；完整工区范围异步读取后替换取景，线号不参与坐标计算。
  if (wellsExtent.isNull())
    wellsExtent = QgsRectangle(0, 0, 10000, 10000);
  else if (wellsExtent.isEmpty())
    wellsExtent.grow(50.0);

  // 坐标系标签；完整工区范围由数据服务异步恢复，状态写入工具提示。
  const QString crsText = canvas->mapSettings().destinationCrs().type() == Qgis::CrsType::Engineering
                              ? tr("局部工程坐标 (米)")
                              : canvas->mapSettings().destinationCrs().userFriendlyIdentifier();
  auto *crsLabel = new QLabel(crsText, topBar);
  crsLabel->setObjectName(QStringLiteral("surveyAreaExtentLabel"));
  QFont crsFont = PaleoTheme::monoFont();
  crsFont.setPointSize(PaleoTheme::tokens().labelPt);
  crsLabel->setFont(crsFont);
  PaleoTheme::applyThemedStyleSheet(crsLabel,
                                    [] { return PaleoTheme::mutedCaptionStyleSheet(); });
  tbLay->addWidget(crsLabel);

  lay->addWidget(new PaleoToolRow(topBar, w));
  lay->addWidget(mapPage, 1);

  auto completeExtent = std::make_shared<QgsRectangle>(wellsExtent);
  auto extentCrs = std::make_shared<QgsCoordinateReferenceSystem>(surveyCrs);
  auto zoomFull = [canvas, completeExtent, extentCrs]() {
    if (!completeExtent->isEmpty())
    {
      QgsRectangle ext = paleo::mapreference::mapExtent(canvas, *completeExtent, *extentCrs);
      if (ext.isEmpty()) return;
      ext.grow(qMax(ext.width(), ext.height()) * 0.08);
      canvas->setExtent(ext);
      canvas->refresh();
    }
    else
    {
      canvas->zoomToFullExtent();
      canvas->refresh();
    }
  };

  connect(btnFull, &QToolButton::clicked, mapPage, zoomFull);
  connect(btnIn, &QToolButton::clicked, mapPage, [mapPage]() { mapPage->mapCanvas()->canvas()->zoomIn(); });
  connect(btnOut, &QToolButton::clicked, mapPage, [mapPage]() { mapPage->mapCanvas()->canvas()->zoomOut(); });
  connect(btnPan, &QToolButton::clicked, mapPage, [mapPage]() {
    mapPage->toolManager()->activate(PreviewMapToolManager::kPan);
  });
  // 工区范围常驻；比例尺、指南针和网格保持独立开关。
  const auto wireDecorToggle = [decorMgr, canvas](QToolButton *btn,
                                                  void (PaleoDecorationManager::*setter)(bool)) {
    connect(btn, &QToolButton::toggled, canvas, [decorMgr, canvas, setter](bool checked) {
      (decorMgr->*setter)(checked);
      canvas->refresh();
    });
  };
  wireDecorToggle(btnScaleBar, &PaleoDecorationManager::setScaleBarEnabled);
  wireDecorToggle(btnNorthArrow, &PaleoDecorationManager::setNorthArrowEnabled);
  wireDecorToggle(btnGrid, &PaleoDecorationManager::setGridEnabled);
  connect(btnSwitchMain, &QToolButton::clicked, this, &DataPreviewTabs::requestShowOnMainCanvas);

  requestSurveyAreaRange(m_doc,mapPage,crsLabel,completeExtent,extentCrs,zoomFull);

  // 延迟自适应全图（等几何尺寸就绪）
  QTimer::singleShot(100, mapPage, zoomFull);

  return w;
}

// 有井口坐标的井灌入全景井位层，返回井位并集（无有效井为空矩形）。
QgsRectangle DataPreviewTabs::loadSurveyWells(QgsVectorLayer *wellsVl)
{
  QgsRectangle extent;
  if (!m_doc || !m_doc->catalog())
    return extent;
  for (const CatalogEntity &well : m_doc->catalog()->entities(QStringLiteral("well")))
  {
    if (!well.hasSurface || !std::isfinite(well.surfaceX) || !std::isfinite(well.surfaceY))
      continue;
    addMemoryPoint(wellsVl, well.surfaceX, well.surfaceY, well.name, QStringLiteral("well"), well.id);
    extent.combineExtentWith(QgsRectangle(well.surfaceX, well.surfaceY,
                                          well.surfaceX, well.surfaceY));
  }
  return extent;
}

void DataPreviewTabs::wireWellNavigation(PreviewMapPage *page, QgsVectorLayer *layer) {
  auto *tools = page->toolManager();
  page->mapCanvas()->canvas()->setToolTip(tr("单击井位打开此井的测井道图；拖动可漫游地图。重叠井位可按井名选择。"));
  WellPointNavigation::install(page->mapCanvas()->canvas(), layer,
      {tools->tool(PreviewMapToolManager::kPan), tools->tool(PreviewMapToolManager::kIdentify)},
      [this](const QString &id) {
    if (!m_doc || m_doc->catalog()->entityById(id).entityType != "well") return;
    emit wellSelected(id);
    const auto links = m_doc->catalog()->linksForEntity(id);
    // 井位点击首选测井道图；无测井时依次回退井头、分层、时深。
    for (const auto &role : {QStringLiteral("well_log"), QStringLiteral("well_head"), QStringLiteral("tops"), QStringLiteral("time_depth")})
      for (const auto &link : links)
        if (!link.unresolved && link.isPrimary && link.role == role) {
          openAssetForWell(link.assetId, id); return;
        }
  });
}
