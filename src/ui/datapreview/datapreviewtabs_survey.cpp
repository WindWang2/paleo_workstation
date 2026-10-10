// 层：视图
// token 例外：DESIGN 数据符号例外：QGIS 测区轮廓与透明填充，不改变地图符号色。（tools/ui-token-exceptions.json 精确计数）。
#include "datapreviewtabs.h"
#include "datapreviewtabs_internal.h"

#include "../paleoicons.h"
#include "../paleoviewport.h"                // PaleoToolRow
#include "../../qgis/projectmapreference.h"  // 底图克隆/版权/范围（paleo::mapreference）
#include <qgsmapcanvas.h>
#include <qgslayertree.h>
#include <qgsproject.h>
#include <qgsrubberband.h>
#include <QAction>
#include <QTimer>

#include <cmath>
#include <memory>

// 共享辅助来自内部头——与 datapreviewtabs.cpp 用同一 using 引入（族内先例：
// datapreviewtabworkbook/tabxml 同款）。
using namespace paleo::datapreview_detail;

void DataPreviewTabs::openSurveyArea()
{
  const QString key = QStringLiteral("survey_area");
  if (QWidget *existing = m_pageOfAsset.value(key))
  {
    m_tabs->setCurrentIndex(m_tabs->indexOf(existing));
    // 井位是固定私有层——重开时重灌（会话中可能又导入了井）。
    if (auto *vl = existing->findChild<QgsVectorLayer *>(QStringLiteral("surveyWellsLayer")))
    {
      vl->dataProvider()->truncate();
      if (m_doc && m_doc->catalog())
        for (const CatalogEntity &well : m_doc->catalog()->entities(QStringLiteral("well")))
          if (well.hasSurface && std::isfinite(well.surfaceX) && std::isfinite(well.surfaceY))
            addMemoryPoint(vl, well.surfaceX, well.surfaceY, well.name, QStringLiteral("well"));
      vl->updateExtents();
    }
    if (auto *cv = existing->findChild<QgsMapCanvas *>(QStringLiteral("surveyMapCanvas")))
    {
      QgsRectangle target;
      if (auto *band = existing->findChild<QgsRubberBand *>(QStringLiteral("surveyAreaRubberBand")))
        target = band->asGeometry().boundingBox();
      if (!target.isNull() && !target.isEmpty())
      {
        target.grow(qMax(target.width(), target.height()) * 0.08);
        cv->setExtent(target);
      }
      else
      {
        cv->zoomToFullExtent();
      }
      cv->refresh();
    }
    return;
  }

  QWidget *page = new QWidget(this);
  auto *pageLay = new QVBoxLayout(page);
  pageLay->setContentsMargins(0, 0, 0, 0);
  pageLay->setSpacing(0);

  QWidget *content = buildSurveyAreaContent(page);
  pageLay->addWidget(content ? content : stateLabel(tr("无法生成测区地图"), page, true), 1);

  const int idx = m_tabs->addTab(page, PaleoIcons::qgisTheme(QStringLiteral("mIconPolygonLayer.svg")), tr("测区全景地图"));
  m_pageOfAsset.insert(key, page);
  m_tabs->setVisible(true);
  m_emptyLabel->setVisible(false);
  m_tabs->setCurrentIndex(idx);
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

  // 1. 工区概况图应该有比例尺，指南针，工区范围等显示
  auto *btnBoundary = new QToolButton(topBar);
  btnBoundary->setObjectName(QStringLiteral("btnToggleSurveyBoundary"));
  btnBoundary->setText(tr("工区范围"));
  btnBoundary->setToolTip(tr("显示/隐藏工区范围边界多边形"));
  btnBoundary->setCheckable(true);
  btnBoundary->setChecked(true);
  btnBoundary->setIcon(PaleoIcons::qgisTheme(QStringLiteral("mIconPolygonLayer.svg")));
  btnBoundary->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  tbLay->addWidget(btnBoundary);

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

  // 全景是固定内容画布：只挂私有井位层 + 工区范围 rubber band——不桥接
  // QgsProject 图层树（其它画布/图层服务实例化的图层不外溢进来，也不联动）。
  auto *wellsVl = makeMemoryPointLayer(tr("井位"), w);
  wellsVl->setObjectName(QStringLiteral("surveyWellsLayer"));
  QgsRectangle wellsExtent;
  if (m_doc && m_doc->catalog())
  {
    for (const CatalogEntity &well : m_doc->catalog()->entities(QStringLiteral("well")))
    {
      if (!well.hasSurface || !std::isfinite(well.surfaceX) || !std::isfinite(well.surfaceY))
        continue;
      addMemoryPoint(wellsVl, well.surfaceX, well.surfaceY, well.name, QStringLiteral("well"));
      wellsExtent.combineExtentWith(QgsRectangle(well.surfaceX, well.surfaceY,
                                                 well.surfaceX, well.surfaceY));
    }
  }
  styleSurveyWellLayer(wellsVl);
  mapPage->addMapLayer(wellsVl, tr("井位"), QString());

  // 装饰管理器（页内建，测区命名保持既有测试面）
  PaleoDecorationManager *decorMgr = mapPage->decorations();
  decorMgr->setObjectName(QStringLiteral("surveyAreaDecorManager"));
  decorMgr->setScaleBarEnabled(true);
  decorMgr->setNorthArrowEnabled(true);

  // 构建工区边界 (QgsRubberBand)
  CatalogEntity survey;
  if (m_doc && m_doc->catalog())
  {
    const auto surveys = m_doc->catalog()->entities(QStringLiteral("seismic_survey"));
    if (!surveys.isEmpty())
      survey = surveys.first();
  }

  QgsGeometry surveyGeom;
  if (survey.corners.size() >= 3)
  {
    QgsRectangle bounds;
    for (const auto &c : survey.corners)
      if (std::isfinite(c.first) && std::isfinite(c.second))
        bounds.combineExtentWith(QgsRectangle(c.first, c.second, c.first, c.second));
    if (!bounds.isEmpty())
      surveyGeom = QgsGeometry::fromRect(bounds);
  }
  else if (survey.inlineMax > survey.inlineMin && survey.xlineMax > survey.xlineMin)
  {
    surveyGeom = QgsGeometry::fromRect(QgsRectangle(survey.inlineMin, survey.xlineMin,
                                                    survey.inlineMax, survey.xlineMax));
  }
  else if (!wellsExtent.isNull())
  {
    // 无 survey 几何：井位并集做兜底范围（单井退化范围扩 50 m 边）。
    if (wellsExtent.isEmpty())
      wellsExtent.grow(50.0);
    surveyGeom = QgsGeometry::fromRect(wellsExtent);
  }
  if (surveyGeom.isNull())
  {
    // 默认局部测区范围 (10 km × 10 km)
    surveyGeom = QgsGeometry::fromRect(QgsRectangle(0, 0, 10000, 10000));
  }

  auto *boundaryBand = new QgsRubberBand(canvas, Qgis::GeometryType::Polygon);
  boundaryBand->setParent(canvas);
  boundaryBand->setObjectName(QStringLiteral("surveyAreaRubberBand"));
  if (!surveyGeom.isNull() && surveyGeom.isGeosValid())
  {
    boundaryBand->setToGeometry(surveyGeom, localCrs);
  }
  boundaryBand->setColor(QColor(27, 115, 208, 16)); // #1B73D0 浅蓝半透明填充
  boundaryBand->setStrokeColor(QColor(QStringLiteral("#1B73D0"))); // 边界线
  boundaryBand->setWidth(2);
  boundaryBand->setLineStyle(Qt::DashLine);
  boundaryBand->show();

  // 工区范围与坐标系说明标签
  QString extentStr;
  if (!surveyGeom.isNull() && !surveyGeom.boundingBox().isEmpty())
  {
    const QgsRectangle box = surveyGeom.boundingBox();
    const double wKm = box.width() / 1000.0;
    const double hKm = box.height() / 1000.0;
    extentStr = tr("工区范围: %1 km × %2 km · %3")
                    .arg(QString::number(wKm, 'f', 1), QString::number(hKm, 'f', 1),
                         canvas->mapSettings().destinationCrs().type() == Qgis::CrsType::Engineering ? tr("局部工程坐标 (米)")
                             : canvas->mapSettings().destinationCrs().userFriendlyIdentifier());
  }
  else
  {
    extentStr = tr("局部工程坐标系统 (米)");
  }
  auto *crsLabel = new QLabel(extentStr, topBar);
  crsLabel->setObjectName(QStringLiteral("surveyAreaExtentLabel"));
  QFont crsFont = PaleoTheme::monoFont();
  crsFont.setPointSize(PaleoTheme::tokens().labelPt);
  crsLabel->setFont(crsFont);
  PaleoTheme::applyThemedStyleSheet(crsLabel,
                                    [] { return PaleoTheme::mutedCaptionStyleSheet(); });
  tbLay->addWidget(crsLabel);

  lay->addWidget(new PaleoToolRow(topBar, w));
  lay->addWidget(mapPage, 1);

  auto completeSurvey = std::make_shared<QgsGeometry>(surveyGeom);
  auto zoomFull = [canvas, completeSurvey, localCrs]() {
    if (!completeSurvey->isNull() && !completeSurvey->boundingBox().isEmpty())
    {
      QgsRectangle ext = paleo::mapreference::mapExtent(canvas, completeSurvey->boundingBox(), localCrs);
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
  connect(btnBoundary, &QToolButton::toggled, canvas, [boundaryBand, canvas](bool checked) {
    boundaryBand->setVisible(checked);
    canvas->refresh();
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

  if (m_doc && m_doc->catalog())
    for (const auto &asset : m_doc->catalog()->assets())
      if (asset.type == QLatin1String("seismic"))
      {
        bool matches = survey.id.isEmpty();
        for (const auto &link : m_doc->catalog()->linksForAsset(asset.id))
          matches = matches || (!link.unresolved && link.entityId == survey.id);
        if (!matches)
          continue;
        crsLabel->setText(tr("正在后台恢复完整测区范围…"));
        QPointer<QWidget> guard = mapPage;
        m_doc->requestSurveyBounds(asset.id,
            [guard, canvas, localCrs, completeSurvey, boundaryBand, btnBoundary, crsLabel, zoomFull](const auto &corners, const QString &error) {
          if (!guard)
            return;
          QgsRectangle bounds;
          for (const auto &corner : corners)
            bounds.combineExtentWith(QgsRectangle(corner.first, corner.second, corner.first, corner.second));
          if (!bounds.isEmpty()) {
            *completeSurvey = QgsGeometry::fromRect(bounds);
            boundaryBand->setToGeometry(*completeSurvey, localCrs);
            boundaryBand->setVisible(btnBoundary->isChecked());
            crsLabel->setText(QObject::tr("工区范围: %1 km × %2 km · %3")
                .arg(bounds.width() / 1000.0, 0, 'f', 1).arg(bounds.height() / 1000.0, 0, 'f', 1)
                .arg(canvas->mapSettings().destinationCrs().type() == Qgis::CrsType::Engineering ? QObject::tr("局部工程坐标 (米)")
                    : canvas->mapSettings().destinationCrs().userFriendlyIdentifier()));
            zoomFull();
          } else {
            crsLabel->setText(QObject::tr("测区范围恢复失败：%1")
                .arg(error.isEmpty() ? QObject::tr("地震体缺少有效坐标") : error));
          }
        });
        break;
      }

  return w;
}
