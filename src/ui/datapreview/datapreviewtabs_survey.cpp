// 层：视图
// token 例外：DESIGN 数据符号例外：QGIS 测区轮廓与透明填充，不改变地图符号色。（tools/ui-token-exceptions.json 精确计数）。
#include "datapreviewtabs.h"
#include "projectstatisticspage.h"
#include <QTabWidget>
#include "datapreviewtabs_internal.h"

#include "../paleoicons.h"
#include "../paleoviewport.h"                // PaleoToolRow
#include "../../qgis/projectmapreference.h"  // 底图克隆/版权/范围（paleo::mapreference）
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
  auto *page = new ProjectStatisticsPage(m_doc ? m_doc->catalog() : nullptr, this);
  connect(page, &ProjectStatisticsPage::surveyMapRequested, this, &DataPreviewTabs::openSurveyArea);
  const int index = m_tabs->addTab(page, PaleoIcons::qgisTheme(QStringLiteral("mLayoutItemChart.svg")), tr("工区看板"));
  m_pageOfAsset.insert(key, page);
  m_emptyLabel->hide(); m_tabs->show(); m_tabs->setCurrentIndex(index);
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

  auto *btnFull = new QToolButton(topBar);
  btnFull->setObjectName(QStringLiteral("btnSurveyFullExtent"));
  btnFull->setText(tr("全图"));
  btnFull->setToolTip(tr("缩放到测区全景范围"));
  btnFull->setIcon(PaleoIcons::qgisTheme(QStringLiteral("mActionZoomFullExtent.svg")));
  btnFull->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  tbLay->addWidget(btnFull);

  auto *btnIn = new QToolButton(topBar);
  btnIn->setObjectName(QStringLiteral("btnSurveyZoomIn"));
  btnIn->setText(tr("放大"));
  btnIn->setToolTip(tr("放大地图 (支持鼠标滚轮缩放)"));
  btnIn->setIcon(PaleoIcons::qgisTheme(QStringLiteral("mActionZoomIn.svg")));
  btnIn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  tbLay->addWidget(btnIn);

  auto *btnOut = new QToolButton(topBar);
  btnOut->setObjectName(QStringLiteral("btnSurveyZoomOut"));
  btnOut->setText(tr("缩小"));
  btnOut->setToolTip(tr("缩小地图 (支持鼠标滚轮缩放)"));
  btnOut->setIcon(PaleoIcons::qgisTheme(QStringLiteral("mActionZoomOut.svg")));
  btnOut->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  tbLay->addWidget(btnOut);

  auto *btnPan = new QToolButton(topBar);
  btnPan->setObjectName(QStringLiteral("btnSurveyPan"));
  btnPan->setText(tr("漫游"));
  btnPan->setToolTip(tr("按住鼠标左键拖拽平移地图"));
  btnPan->setIcon(PaleoIcons::qgisTheme(QStringLiteral("mActionPan.svg")));
  btnPan->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  tbLay->addWidget(btnPan);

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
  const auto localCrs = QgsCoordinateReferenceSystem::fromWkt(DataCatalog::localGridCrsWkt());
  if (m_project) {
    canvas->setProject(m_project);
    canvas->mapSettings().setTransformContext(m_project->transformContext());
    mapPage->mapCanvas()->setOverrideCrs(m_project->crs());
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
  {
    mapPage->setOverviewVisible(false);
  }

  // 全景是固定内容画布：只挂私有井位层——不桥接 QgsProject 图层树（其它
  // 画布/图层服务实例化的图层不外溢进来，也不联动）。工区范围带（原由地震
  // 测区角点/井位并集拟合、后台恢复）按用户要求撤下。
  auto *wellsVl = makeMemoryPointLayer(tr("井位"), w);
  wellsVl->setObjectName(QStringLiteral("surveyWellsLayer"));
  QgsRectangle wellsExtent;
  if (m_doc && m_doc->catalog())
  {
    for (const CatalogEntity &well : m_doc->catalog()->entities(QStringLiteral("well")))
    {
      if (!well.hasSurface || !std::isfinite(well.surfaceX) || !std::isfinite(well.surfaceY))
        continue;
      addMemoryPoint(wellsVl, well.surfaceX, well.surfaceY, well.name, QStringLiteral("well"), well.id);
      wellsExtent.combineExtentWith(QgsRectangle(well.surfaceX, well.surfaceY,
                                                 well.surfaceX, well.surfaceY));
    }
  }
  styleSurveyWellLayer(wellsVl);
  mapPage->addMapLayer(wellsVl, tr("井位"), QString()); wireWellNavigation(mapPage, wellsVl);

  // 装饰管理器（页内建，测区命名保持既有测试面）
  PaleoDecorationManager *decorMgr = mapPage->decorations();
  decorMgr->setObjectName(QStringLiteral("surveyAreaDecorManager"));
  decorMgr->setScaleBarEnabled(true);
  decorMgr->setNorthArrowEnabled(true);

  // 取景范围：井位并集（单井退化扩 50 m 边）；无井时 10 km 默认幅面。
  if (wellsExtent.isNull())
    wellsExtent = QgsRectangle(0, 0, 10000, 10000);
  else if (wellsExtent.isEmpty())
    wellsExtent.grow(50.0);

  // 坐标系说明标签（只报 CRS；原「工区范围 X × Y」文案随范围带一并撤下）
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
  auto zoomFull = [canvas, completeExtent, localCrs]() {
    if (!completeExtent->isEmpty())
    {
      QgsRectangle ext = paleo::mapreference::mapExtent(canvas, *completeExtent, localCrs);
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
  connect(btnScaleBar, &QToolButton::toggled, canvas, [decorMgr, canvas](bool checked) {
    decorMgr->setScaleBarEnabled(checked);
    canvas->refresh();
  });
  connect(btnNorthArrow, &QToolButton::toggled, canvas, [decorMgr, canvas](bool checked) {
    decorMgr->setNorthArrowEnabled(checked);
    canvas->refresh();
  });
  connect(btnGrid, &QToolButton::toggled, canvas, [decorMgr, canvas](bool checked) {
    decorMgr->setGridEnabled(checked);
    canvas->refresh();
  });
  connect(btnSwitchMain, &QToolButton::clicked, this, &DataPreviewTabs::requestShowOnMainCanvas);

  // 延迟自适应全图（等几何尺寸就绪）
  QTimer::singleShot(100, mapPage, zoomFull);

  return w;
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
