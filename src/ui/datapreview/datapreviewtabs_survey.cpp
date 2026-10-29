// 层：视图
// token 例外：DESIGN 数据符号例外：QGIS 测区轮廓与透明填充，不改变地图符号色。（tools/ui-token-exceptions.json 精确计数）。
#include "datapreviewtabs.h"
#include "projectstatisticspage.h"
#include "surveytimesectionwidget.h"
#include <QTabWidget>
#include "datapreviewtabs_internal.h"

#include "../paleoicons.h"
#include "../paleoviewport.h"                // PaleoToolRow
#include "../../qgis/projectmapreference.h"  // 底图克隆/版权/范围/测区 CRS（paleo::mapreference）
#include <qgsmapcanvas.h>
#include <qgslayertree.h>
#include <qgsproject.h>
#include <qgsrubberband.h>
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
  // 页内容按建页时刻快照 catalog 井位/工区范围与工程底图克隆——测区节点在
  // 树里常驻、随时可激活，页可能早于数据就绪落成降级态（0 井、10×10 默认
  // 范围、无底图）。激活一律按现状整体重建：旧复用路径只能回灌井位，范围
  // 与底图永不合愈。开销与初次建页相同（内存层 + rubber band + 底图克隆）。
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

  // 全景是固定内容画布：只挂私有井位层 + 工区范围 rubber band——不桥接
  // QgsProject 图层树（其它画布/图层服务实例化的图层不外溢进来，也不联动）。
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

  // 构建工区边界 (QgsRubberBand)
  CatalogEntity survey;
  if (m_doc && m_doc->catalog())
  {
    const auto surveys = m_doc->catalog()->entities(QStringLiteral("seismic_survey"));
    if (!surveys.isEmpty())
      survey = surveys.first();
  }

  // 取界优先级：角点 → 井位并集 → 默认 10×10。
  // 不拿 inline/xline 线号当坐标画框——线号是测网索引（百/千级），与米制
  // 网格不同系（dashboardsurveymap 同口径）：画出来的「工区框」没有空间
  // 意义，还会把全图缩放拖到假位置，井位缩成不可见的点。角点缺失时由
  // 下方 requestSurveyBounds 异步补探（轻量道头探测，规则网格秒回）并写回
  // 目录实体；井位并集只是补探完成前的临时框。
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
  if (surveyGeom.isNull() && !wellsExtent.isNull())
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
    boundaryBand->setToGeometry(surveyGeom, surveyCrs);
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
  auto zoomFull = [canvas, completeSurvey, surveyCrs]() {
    if (!completeSurvey->isNull() && !completeSurvey->boundingBox().isEmpty())
    {
      QgsRectangle ext = paleo::mapreference::mapExtent(canvas, completeSurvey->boundingBox(), surveyCrs);
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

  // 底部地震时间剖面（数据服务测线预览）：沿测区长轴取中线，一次读取。
  if (auto *timeSection = buildTimeSectionStrip(w, tbLay, survey))
    lay->addWidget(timeSection);

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
            [guard, canvas, surveyCrs, completeSurvey, boundaryBand, btnBoundary, crsLabel, zoomFull](const PreviewDocService::SurveyBounds &range) {
          if (!guard)
            return;
          // 范围由数据服务给出（四角点按抽样道头拟合，包围盒同时算好），
          // 界面不再自己从角点拼矩形——旧实现为此重开 SEG-Y 建整份道索引。
          if (!range.valid()) {
            crsLabel->setText(QObject::tr("测区范围不可用：%1")
                .arg(range.detail.isEmpty() ? QObject::tr("地震体缺少有效坐标") : range.detail));
            return;
          }
          const QgsRectangle bounds(range.minX, range.minY, range.maxX, range.maxY);
          if (!bounds.isEmpty()) {
            *completeSurvey = QgsGeometry::fromRect(bounds);
            boundaryBand->setToGeometry(*completeSurvey, surveyCrs);
            boundaryBand->setVisible(btnBoundary->isChecked());
            crsLabel->setText(QObject::tr("工区范围: %1 km × %2 km · %3")
                .arg(bounds.width() / 1000.0, 0, 'f', 1).arg(bounds.height() / 1000.0, 0, 'f', 1)
                .arg(canvas->mapSettings().destinationCrs().type() == Qgis::CrsType::Engineering ? QObject::tr("局部工程坐标 (米)")
                    : canvas->mapSettings().destinationCrs().userFriendlyIdentifier()));
            // 角点是拟合值，把残差与单位如实挂在提示里，不假装是实测角点。
            const QString unit = range.coordinateUnit.isEmpty()
                ? QObject::tr("（单位未声明）") : range.coordinateUnit;
            crsLabel->setToolTip(std::isfinite(range.rmsResidual)
                ? QObject::tr("角点由抽样道头拟合，残差 %1 %2；%3")
                      .arg(range.rmsResidual, 0, 'f', 2).arg(unit, range.detail)
                : QObject::tr("角点由抽样道头拟合，残差未知（%1）；%2").arg(unit, range.detail));
            zoomFull();
          } else {
            crsLabel->setText(QObject::tr("测区范围恢复失败：%1")
                .arg(range.detail.isEmpty() ? QObject::tr("地震体缺少有效坐标") : range.detail));
          }
        });
        break;
      }

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

QWidget *DataPreviewTabs::buildTimeSectionStrip(QWidget *page, QHBoxLayout *topBarLay,
                                                const CatalogEntity &survey)
{
  auto *toggle = new QToolButton(topBarLay->parentWidget());
  toggle->setObjectName(QStringLiteral("btnToggleTimeSection"));
  toggle->setText(tr("时间剖面"));
  toggle->setToolTip(tr("显示/隐藏底部地震时间剖面预览（数据服务测线）"));
  toggle->setCheckable(true);
  toggle->setChecked(true);
  toggle->setToolButtonStyle(Qt::ToolButtonTextOnly);
  topBarLay->addWidget(toggle);

  auto *strip = new SurveyTimeSectionWidget(page);
  strip->setObjectName(QStringLiteral("surveyTimeSectionHost"));
  connect(toggle, &QToolButton::toggled, strip, &QWidget::setVisible);

  const qlonglong modelVolumeId = survey.extra.value(QStringLiteral("areaModelVolumeId")).toLongLong();
  if (!m_doc || modelVolumeId <= 0 || survey.inlineMax <= survey.inlineMin ||
      survey.xlineMax <= survey.xlineMin)
    return strip;
  // 长轴方向做剖面（inline 跨度更大 → 固定中线 crossline、沿 inline 走）。
  const bool alongInline =
      qint64(survey.inlineMax) - survey.inlineMin >= qint64(survey.xlineMax) - survey.xlineMin;
  const QString direction = alongInline ? QStringLiteral("crossline") : QStringLiteral("inline");
  const int midLine = alongInline ? (survey.xlineMin + survey.xlineMax) / 2
                                  : (survey.inlineMin + survey.inlineMax) / 2;
  const int fallbackLine = alongInline ? survey.xlineMin : survey.inlineMin;
  strip->showLoading(tr("正在读取时间剖面…"));
  QPointer<QWidget> guard = strip;
  m_doc->requestSurveyLinePreview(modelVolumeId, direction, midLine,
      [this, modelVolumeId, strip, guard, direction, fallbackLine](const SurveyLinePreview &preview) {
        if (!guard)
          return;
        if (!preview.ok)
        {
          // 中线不在测线步长网格上时回落到轴起点（readSeismicLine 只收真实线号）。
          m_doc->requestSurveyLinePreview(modelVolumeId, direction, fallbackLine,
              [strip, guard](const SurveyLinePreview &fallback) {
                if (guard)
                  strip->setResult(fallback);
              });
          return;
        }
        strip->setResult(preview);
      });
  return strip;
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
