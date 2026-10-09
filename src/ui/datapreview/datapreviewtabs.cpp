// 层：视图
// token 例外：DESIGN 数据符号例外：QGIS 测区轮廓与透明填充，不改变地图符号色。（tools/ui-token-exceptions.json 精确计数）。
#include "datapreviewtabs.h"
#include "officepreviewwidget.h"
#include "../../workflow/officepreviewsession.h"
#include "../paleoviewport.h"

#include "../paleotheme.h" // DESIGN.md token 出口（颜色/字阶/活体样式共用）
#include "../paleoicons.h" // 角落最大化/还原自绘图标

#include "../../catalog/datacatalog.h"
#include "../../domain/seismic/nicestep.h"
#include "../../domain/wellrecords.h"     // WellTopRecord/TimeDepthTable（domain 纯数据）
#include "../../domain/sectiontrace.h"    // SegyTrace/SegySectionGrid（domain 纯数据）
#include "../../io/lasdoc.h"              // LasCurve（白名单：数据模型）
#include "../../services/previewdoc.h"    // 唯一数据门面——解析/解码/SHA/PDF 编排全经它（W1）
#include "../../services/sectiondoc.h"   // SectionDoc 完整定义（方向 59 拆细头；onSectionReady 触碰成员）
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
#include "../../qgis/projectmapreference.h"
#include <qgsmapcanvas.h>
#include <qgslayertreemapcanvasbridge.h>
#include <qgslayertree.h>
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

// ---------------------------------------------------------------------------
// §4 状态文案与九类资产面板。DESIGN.md dock 面板 tokens：
// surface #FFFFFF、border #DFE5EC、text #24303E、text-muted #5D6E80、8pt captions；
// 数值列 JetBrains Mono 9pt 右对齐；语义色 #F29900(警告)/#E53935(失败)。
// ---------------------------------------------------------------------------
#include "datapreviewtabs_internal.h"
// 匿名命名空间已抽成内部头（方向20 轮4）——用 using 引入，避免给 300+ 处
// 调用点逐个加限定（那会把纯机械改动铺满 diff，掩盖真正的结构变更）。
using namespace paleo::datapreview_detail;

// T27 中文化：coordinate_status 枚举 → §4 计划文案。untransformed 用与
// 状态栏/PDF 页脚同一句「工程坐标 · 米 · 未投影」；invalid/missing 用
// 「坐标无效」「没有坐标」，仍 text-muted（#5D6E80）。
QString DataPreviewTabs::coordinateStatusText(const QString &status)
{
  if (status == QLatin1String("ok"))
    return tr("坐标有效");
  if (status == QLatin1String("untransformed"))
    return tr("工程坐标 · 米 · 未投影");
  if (status == QLatin1String("invalid"))
    return tr("坐标无效");
  return tr("没有坐标"); // missing / 空 / 未知
}

void DataPreviewTabs::setHorizonOnMap(const QString &layerId, bool on)
{
  // T29 双向同步：所有绑到该 layerId 的「在地图上显示」按钮跟随图层可见性。
  auto buttons = findChildren<QPushButton *>(QStringLiteral("showOnMapBtn"));
  if (m_detailsHost)
    buttons.append(m_detailsHost->findChildren<QPushButton *>(QStringLiteral("showOnMapBtn")));
  for (QPushButton *btn : buttons)
    if (btn->property("layerId").toString() == layerId)
    {
      btn->setProperty("onMap", on);
      btn->setText(on ? tr("已在地图上") : tr("在地图上显示"));
    }
}

DataPreviewTabs::DataPreviewTabs(QWidget *parent)
  : QWidget(parent)
{
  auto *lay = new QVBoxLayout(this);
  lay->setContentsMargins(0, 0, 0, 0);
  lay->setSpacing(PaleoTheme::tokens().spacingXs);

  m_tabs = new QTabWidget(this);
  m_tabs->setObjectName(QStringLiteral("dataPreviewTabs"));
  m_tabs->setTabsClosable(true);
  m_tabs->setUsesScrollButtons(true); // T32：标签超宽滚动，不挤压
  m_tabs->setAccessibleName(tr("预览"));
  // dock 面板样式（DESIGN.md）：无工作流蓝下划线，安静边框；活体跟随主题。
  PaleoTheme::applyThemedStyleSheet(m_tabs, [] {
    const PaleoTheme::ThemeTokens &t = PaleoTheme::tokens();
    return PaleoTheme::metricStyleSheet(QStringLiteral(
        "QTabWidget::pane { border: 1px solid %1; background: %2; top: -1px; }"
        "QTabBar::tab { padding: {spacing.xs}px {spacing.md}px; color: %3; border: 1px solid %1;"
        " border-bottom: none; background: %2; }"
        "QTabBar::tab:selected { color: %4; font-weight: 600; }"))
        .arg(qssHex(t.border), qssHex(t.surface), qssHex(t.textMuted), qssHex(t.text));
  });
  connect(m_tabs, &QTabWidget::tabCloseRequested, this, [this](int index) {
    const QString assetId = assetIdAt(index);
    if (!assetId.isEmpty())
      closeAssetTab(assetId);
  });
  connect(m_tabs, &QTabWidget::currentChanged, this, [this](int index) {
    if (index >= 0)
      focusWellIfNeeded(assetIdAt(index), m_tabs->widget(index));
    syncDetails();
  });
  // D7 最大化 affordance：右上角 checkable 钮，切换时只发意图信号——实际
  // 分栏尺寸由 shell 决定。空态时 tabs 隐藏，按钮随之隐藏。
  auto *maxBtn = new QToolButton(m_tabs);
  maxBtn->setObjectName(QStringLiteral("previewMaxButton"));
  maxBtn->setCheckable(true);
  maxBtn->setText(tr("最大化预览"));
  maxBtn->setAccessibleName(tr("最大化预览"));
  maxBtn->setToolTip(tr("暂时收起数据列表和属性面板，让预览占满工作区"));
  // QGIS 主题没有最大化/还原语义——PaleoIcons 自绘，随勾选态切换。
  maxBtn->setIcon(PaleoIcons::maximize());
  maxBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  connect(maxBtn, &QToolButton::toggled, this, [this, maxBtn](bool on) {
    maxBtn->setText(on ? tr("还原预览") : tr("最大化预览"));
    maxBtn->setIcon(on ? PaleoIcons::restore() : PaleoIcons::maximize());
    maxBtn->setToolTip(on ? tr("恢复最大化前的面板布局") : tr("暂时收起数据列表和属性面板，让预览占满工作区"));
    emit previewMaximizeToggled(on);
  });
  m_tabs->setCornerWidget(maxBtn, Qt::TopRightCorner);
  lay->addWidget(m_tabs);

  m_emptyLabel = stateLabel(tr("还没有打开的预览 — 从顶部导入数据，再在左侧列表选择一条数据"), this);
  m_emptyLabel->setObjectName(QStringLiteral("previewEmptyLabel"));
  lay->addWidget(m_emptyLabel);
  m_tabs->setVisible(false);
}

DataPreviewTabs::~DataPreviewTabs() = default;

void DataPreviewTabs::setDetailsHost(QWidget *host)
{
  m_detailsHost = host;
  syncDetails();
}

void DataPreviewTabs::clearDetails(const QString &assetId)
{
  if (auto old = m_detailsOfAsset.take(assetId)) {
    old->hide();
    old->setParent(nullptr);
    old->deleteLater();
  }
  syncDetails();
}

void DataPreviewTabs::syncDetails()
{
  const QString active = assetIdAt(m_tabs->currentIndex());
  bool any = false;
  for (auto it = m_detailsOfAsset.cbegin(); it != m_detailsOfAsset.cend(); ++it)
    if (it.value()) {
      const bool show = it.key() == active;
      it.value()->setVisible(show);
      any |= show;
    }
  if (m_detailsHost)
    m_detailsHost->setVisible(any);
}


void DataPreviewTabs::setImportService(DataImportService *svc)
{
  // 自建门面（测试/小环境）；壳共享实例经 setDocService。
  m_docOwned.reset(svc ? new PreviewDocService(svc) : nullptr);
  attachDoc(m_docOwned.get());
}

void DataPreviewTabs::setDocService(PreviewDocService *doc)
{
  m_docOwned.reset();
  attachDoc(doc);
}

void DataPreviewTabs::attachDoc(PreviewDocService *doc)
{
  if (m_doc)
    disconnect(m_doc, nullptr, this, nullptr);
  if (m_catalogForTitles)
    disconnect(m_catalogForTitles, nullptr, this, nullptr);
  m_catalogForTitles = nullptr;
  m_doc = doc;
  if (!m_doc)
    return;
  if (m_taskSvc)
    m_doc->setTaskService(m_taskSvc); // 接线顺序无关：后到的服务补进门面
  // 测线解码结果（D1/T23）：陈旧结果已在服务内按世代号丢弃。
  connect(m_doc, &PreviewDocService::seismicSectionReady, this,
          &DataPreviewTabs::onSectionReady);
  connect(m_doc, &PreviewDocService::seismicSectionFailed, this,
          &DataPreviewTabs::onSectionFailed);
  connect(m_doc, &PreviewDocService::seismicSectionCancelled, this,
          [this](const QString &assetId) {
            onSectionFailed(assetId, tr("已取消"));
          });
  // F1（goal/perf-systematize 簇2）：LAS 数据行异步填充结果（key=assetId；
  // 陈旧结果已在服务内按世代号丢弃）。
  connect(m_doc, &PreviewDocService::lasReady, this,
          &DataPreviewTabs::onLasReady);
  connect(m_doc, &PreviewDocService::lasFailed, this,
          &DataPreviewTabs::onLasFailed);
  connect(m_doc, &PreviewDocService::lasCancelled, this,
          [this](const QString &key) { onLasFailed(key, tr("已取消")); });
  // B 包 staleness-lite：stale 标记可能来自其它标签的 sha 复验或上游版本
  // 取代——catalog 任一变更后重算已开标签的「过时」徽标（GUI 线程直连，
  // 不必重建标签）。换绑服务时先断旧 catalog（上面已断）。
  m_catalogForTitles = m_doc->catalog();
  if (m_catalogForTitles)
    connect(m_catalogForTitles, &DataCatalog::changed, this, [this]() {
      for (auto it = m_pageOfAsset.constBegin(); it != m_pageOfAsset.constEnd(); ++it)
        updateTabTitle(it.key());
    });
}

void DataPreviewTabs::setTaskService(PaleoTaskService *svc)
{
  m_taskSvc = svc;
  if (m_doc)
    m_doc->setTaskService(svc);
}

void DataPreviewTabs::setProject(QgsProject *project)
{
  m_project = project;
}

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

int DataPreviewTabs::tabCount() const
{
  return m_tabs->count();
}

QString DataPreviewTabs::assetIdAt(int index) const
{
  QWidget *w = m_tabs->widget(index);
  if (!w)
    return QString();
  for (auto it = m_pageOfAsset.constBegin(); it != m_pageOfAsset.constEnd(); ++it)
    if (it.value() == w)
      return it.key();
  return QString();
}

void DataPreviewTabs::closeAssetTab(const QString &assetId)
{
  QWidget *page = m_pageOfAsset.value(assetId);
  if (!page)
    return;
  const int idx = m_tabs->indexOf(page);
  if (idx >= 0)
    m_tabs->removeTab(idx);
  m_pageOfAsset.remove(assetId);
  clearDetails(assetId);
  m_wellEntityOfAsset.remove(assetId);
  m_titleSuffixOfAsset.remove(assetId);
  m_chosenVersionOfAsset.remove(assetId);
  // D1：标签关掉即释放该资产的索引缓存（持有文件句柄级状态）与世代号；
  // 进行中的解码任务请求取消——结果没人等了。
  if (m_doc)
  {
    m_doc->releaseSection(assetId);
    m_doc->releaseLas(assetId); // F1：同口径释放 LAS 解析世代号/取消在途
  }
  m_pendingSection.remove(assetId);
  m_pendingLas.remove(assetId);
  page->setParent(nullptr); // 摘出子树再推迟删除，关闭后 findChild 不再命中
  page->deleteLater();
  if (m_tabs->count() == 0)
  {
    m_tabs->setVisible(false);
    m_emptyLabel->setVisible(true);
  }
}

void DataPreviewTabs::closeAllTabs()
{
  const QStringList ids = m_pageOfAsset.keys();
  for (const QString &id : ids)
    closeAssetTab(id);
  // 不在 m_pageOfAsset 里登记的页（理论上没有）一并摘掉，确保空态。
  while (m_tabs->count() > 0)
  {
    QWidget *w = m_tabs->widget(0);
    m_tabs->removeTab(0);
    if (w)
    {
      w->setParent(nullptr);
      w->deleteLater();
    }
  }
  m_pendingSection.clear();
  m_pendingLas.clear();
  m_tiledCanvas.clear();
  m_tiledSample = -1;
  m_tabs->setVisible(false);
  m_emptyLabel->setVisible(true);
}

bool DataPreviewTabs::isMissingSourceState(const QString &assetId) const
{
  QWidget *page = m_pageOfAsset.value(assetId);
  if (!page)
    return false;
  auto *lbl = page->findChild<QLabel *>(QStringLiteral("stateText"));
  return lbl && lbl->text().contains(tr("找不到源文件"));
}

bool DataPreviewTabs::relocateMissingSourceWith(const QString &assetId,
                                                const QString &versionId,
                                                const QString &pickedPath)
{
  // wave4：把死胡同接到 relocateVersionSource——内容一致才重接（服务层拒解
  // SHA 不一致的候选文件，不静默换源）。失败保留「找不到源文件」状态与按钮，
  // 错误就地可见，可换文件再试；成功清掉本会话的 SHA 已验缓存（新路径要在
  // 重建时重新过 §3 校验门）并重建标签加载真预览。
  if (!m_doc || assetId.isEmpty())
    return false;
  QString err;
  const QString newVer = m_doc->relocateVersionSource(versionId, pickedPath, &err);
  if (newVer.isEmpty())
  {
    QWidget *page = m_pageOfAsset.value(assetId);
    if (auto *lbl = page ? page->findChild<QLabel *>(QStringLiteral("stateText")) : nullptr)
      lbl->setText(tr("找不到源文件\n重新定位失败：%1").arg(err));
    return false;
  }
  if (m_doc)
    m_doc->resetSha(assetId);
  rebuildAssetTab(assetId);
  return true;
}

QLabel *DataPreviewTabs::loadingLabel(const QString &fileName, QWidget *parent)
{
  // §4 读取中态：「正在读取」+文件名。读取仍是同步的——标签先就位并立即
  // 重绘，文件读完后隐藏（钩子存在，但不引入线程）。
  auto *l = stateLabel(tr("正在读取\n%1").arg(fileName), parent);
  l->setObjectName(QStringLiteral("loadingText"));
  return l;
}

QWidget *DataPreviewTabs::failureState(const QString &assetId, const QString &reason,
                                       QWidget *parent)
{
  // §4 失败态：「读取失败」+原因+文件名+「重试」。重试 = 重建该标签。
  const QString name =
      m_doc ? m_doc->catalog()->assetById(assetId).displayName : assetId;
  auto *box = new QWidget(parent);
  auto *l = new QVBoxLayout(box);
  l->setContentsMargins(0, 0, 0, 0);
  l->setSpacing(PaleoTheme::tokens().spacingXs);
  l->addStretch(1);
  l->addWidget(stateLabel(tr("读取失败\n%1\n%2").arg(reason, name), box, true));
  auto *btn = new QPushButton(tr("重试"), box);
  btn->setObjectName(QStringLiteral("retryBtn"));
  connect(btn, &QPushButton::clicked, box,
          [this, assetId] { rebuildAssetTab(assetId); });
  l->addWidget(btn, 0, Qt::AlignHCenter);
  l->addStretch(1);
  return box;
}

void DataPreviewTabs::focusWellIfNeeded(const QString &assetId, QWidget *page)
{
  Q_UNUSED(page);
  if (!m_doc || assetId.isEmpty())
    return;
  // §4：well_head 标签的选中井在地图上高亮。多井标签只报该标签已选中的井
  // ——没有选中就不报，绝不拿第一条链接糊弄（m_wellEntityOfAsset 在
  // 唯一已决井/下拉框选择时写入）。
  const CatalogAsset asset = m_doc->catalog()->assetById(assetId);
  if (asset.type != QLatin1String("well_head"))
    return;
  const QString wellId = m_wellEntityOfAsset.value(assetId);
  if (!wellId.isEmpty())
    emit wellSelected(wellId);
}

void DataPreviewTabs::updateTabTitle(const QString &assetId)
{
  QWidget *page = m_pageOfAsset.value(assetId);
  if (!page)
    return;
  const int idx = m_tabs->indexOf(page);
  if (idx < 0)
    return;
  // §4：标题是「文件名 · 井名」/「文件名 · 测线」；无过滤时只有文件名。
  QString title =
      m_doc ? m_doc->catalog()->assetById(assetId).displayName : assetId;
  if (title.isEmpty())
    title = assetId;
  const QString suffix = m_titleSuffixOfAsset.value(assetId);
  if (!suffix.isEmpty())
    title += QStringLiteral(" · ") + suffix;
  // B 包 staleness-lite：实际预览版本被标 stale（上游 sha 失配/被取代）→
  // 标题带「过时」徽标——下游产物过期在数据页如实可见。
  const CatalogVersion shown = !m_doc ? CatalogVersion()
      : m_chosenVersionOfAsset.value(assetId).isEmpty() ? m_doc->catalog()->currentVersion(assetId)
      : m_doc->versionForPreview(m_chosenVersionOfAsset.value(assetId));
  if (shown.extra.value(QStringLiteral("stale")).toBool())
    title += QStringLiteral(" · ") + tr("过时");
  m_tabs->setTabText(idx, title);
}

void DataPreviewTabs::drainDeferredRebuilds()
{
  while (!m_rebuildDeferred.isEmpty())
  {
    const QList<QString> ids = m_rebuildDeferred.values();
    m_rebuildDeferred.clear();
    for (const QString &id : ids)
      rebuildAssetTab(id); // m_buildingContent 已复位——正常重建
  }
}

void DataPreviewTabs::rebuildAssetTab(const QString &assetId)
{
  QWidget *page = m_pageOfAsset.value(assetId);
  if (!page || !m_doc)
    return;
  if (m_buildingContent)
  {
    // 嵌套重建（转换失败信号在 ensure* 内同步发射所致）：外层
    // buildContent 继续走完会看到服务层已写就的最终态，这里只记顺延。
    m_rebuildDeferred.insert(assetId);
    return;
  }
  auto *pageLay = qobject_cast<QVBoxLayout *>(page->layout());
  if (!pageLay)
    return;
  clearDetails(assetId);
  while (QLayoutItem *it = pageLay->takeAt(0))
  {
    if (QWidget *w = it->widget())
    {
      // 信号发送者（如「重试」钮）可能就在被清的子树里——不能就地 delete，
      // 但先摘出父子树，deleteLater 后 findChild 不再碰到陈旧控件。
      w->setParent(nullptr);
      w->deleteLater();
    }
    delete it;
  }
  const QString name = m_doc->catalog()->assetById(assetId).displayName;
  QLabel *loading = loadingLabel(name.isEmpty() ? assetId : name, page);
  pageLay->addWidget(loading, 1);
  loading->repaint(); // 「正在读取」先可见，随后同步读
  m_buildingContent = true;
  QWidget *content = buildContent(assetId, page);
  m_buildingContent = false;
  loading->setVisible(false); // 保留在树里，便于测试/诊断读取中态
  pageLay->addWidget(content ? content : stateLabel(tr("无法生成预览"), page, true), 1);
  updateTabTitle(assetId);
  drainDeferredRebuilds();
}

void DataPreviewTabs::openAsset(const QString &assetId)
{
  if (!m_doc || assetId.isEmpty())
    return;
  if (QWidget *existing = m_pageOfAsset.value(assetId))
  {
    m_tabs->setCurrentIndex(m_tabs->indexOf(existing)); // 重选聚焦（§4）
    focusWellIfNeeded(assetId, existing);
    return;
  }

  const QString displayName = m_doc->catalog()->assetById(assetId).displayName;
  QWidget *page = new QWidget(this);
  auto *pageLay = new QVBoxLayout(page);
  pageLay->setContentsMargins(PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm);

  QLabel *loading = loadingLabel(displayName.isEmpty() ? assetId : displayName, page);
  pageLay->addWidget(loading, 1);

  const int idx = m_tabs->addTab(page, displayName.isEmpty() ? assetId : displayName);
  m_pageOfAsset.insert(assetId, page);
  m_tabs->setVisible(true);
  m_emptyLabel->setVisible(false);
  m_tabs->setCurrentIndex(idx);
  loading->repaint(); // 「正在读取」+文件名在同步读取前先可见（§4）

  m_buildingContent = true;
  QWidget *content = buildContent(assetId, page);
  m_buildingContent = false;
  loading->setVisible(false); // 读取完成；隐藏但保留节点便于测试断言该状态
  pageLay->addWidget(content ? content : stateLabel(tr("无法生成预览"), page, true), 1);
  updateTabTitle(assetId);
  focusWellIfNeeded(assetId, page);
  drainDeferredRebuilds();
}

QString DataPreviewTabs::versionIdAt(int index) const
{
  QWidget *page = index >= 0 && index < m_tabs->count() ? m_tabs->widget(index) : nullptr;
  return page ? page->property("previewVersionId").toString() : QString();
}

void DataPreviewTabs::openVersion(const QString &versionId)
{
  if (!m_doc) return;
  const CatalogVersion v = m_doc->versionForPreview(versionId);
  if (v.id.isEmpty()) return;
  const bool existing = m_pageOfAsset.contains(v.assetId);
  // 解码与 SHA 留底缓存按资产键控；换版本必须取消旧世代并释放旧句柄。
  if (m_chosenVersionOfAsset.value(v.assetId) != v.id)
  {
    m_doc->releaseSection(v.assetId);
    m_doc->releaseLas(v.assetId);
    m_pendingSection.remove(v.assetId); m_pendingLas.remove(v.assetId);
  }
  m_chosenVersionOfAsset[v.assetId] = v.id;
  // 页签切换时壳不先覆盖图内选中态，最终 versionContextChanged 统一定位。
  setProperty("paleo.versionNavigation", true);
  openAsset(v.assetId);
  if (existing) rebuildAssetTab(v.assetId);
  setProperty("paleo.versionNavigation", false);
  emit versionContextChanged(v.assetId, v.id);
}

void DataPreviewTabs::openAssetForWell(const QString &assetId, const QString &wellId)
{
  if (!m_doc || assetId.isEmpty())
    return;
  if (!wellId.isEmpty())
  {
    m_wellEntityOfAsset[assetId] = wellId;
    if (m_doc->catalog())
      m_titleSuffixOfAsset[assetId] = m_doc->catalog()->entityById(wellId).name;
  }
  openAsset(assetId);
  QWidget *page = m_pageOfAsset.value(assetId);
  if (page)
  {
    if (auto *combo = page->findChild<QComboBox *>(QStringLiteral("wellCombo")))
    {
      const int idx = combo->findData(wellId);
      if (idx >= 0 && combo->currentIndex() != idx)
        combo->setCurrentIndex(idx);
    }
    updateTabTitle(assetId);
    focusWellIfNeeded(assetId, page);
  }
}

void DataPreviewTabs::openSeismicLine(const QString &assetId, const QString &kind,
                                      int line, double timeMs)
{
  openAsset(assetId); // §4 重选语义：已有标签聚焦，否则新开
  QWidget *page = m_pageOfAsset.value(assetId);
  if (!page)
    return;
  auto *mode = page->findChild<QComboBox *>(QStringLiteral("lineMode"));
  auto *no = page->findChild<QSpinBox *>(QStringLiteral("lineSpin"));
  if (!mode || !no)
    return; // 非地震标签（或地震正文未建出来）——不造假测线控件
  const int want = mode->findData(
      kind == QLatin1String("crossline") ? QStringLiteral("crossline")
                                         : QStringLiteral("inline"));
  if (want >= 0 && mode->currentIndex() != want)
    mode->setCurrentIndex(want); // currentIndexChanged → 该控件链路上的 decode
  if (no->value() != line)
    no->setValue(line); // valueChanged → decode 目标测线
  if (auto *modeTabs = page->findChild<QTabWidget *>(QStringLiteral("seismicSubTabs")))
    modeTabs->setCurrentIndex(0); // 聚焦到二维测线剖面页签
  Q_UNUSED(timeMs); // 目标时间的标注由剖面自身的 D61 标定线承担（§4/阶段B）
}

QWidget *DataPreviewTabs::buildContent(const QString &assetId, QWidget *page)
{
  DataCatalog *cat = m_doc->catalog();
  const CatalogAsset asset = cat->assetById(assetId);
  if (asset.id.isEmpty())
    return nullptr;
  const QString chosenId = m_chosenVersionOfAsset.value(assetId);
  const CatalogVersion v = chosenId.isEmpty() ? cat->currentVersion(assetId) : m_doc->versionForPreview(chosenId);
  if (v.id.isEmpty() || v.assetId != assetId) return stateLabel(tr("所选版本不在目录中"), page, true);
  page->setProperty("previewVersionId", v.id);
  CatalogVersion sourceVersion = v; // abs 实际对应的版本（文档标签锚回 RAW 原件）
  QString abs = m_doc->absolutePathForVersion(v);
  // 文档/工作簿资产：RAW 原件是规范来源——currentVersion 可能已指向
  // DERIVED 转换件，缺失检查与「用系统程序打开」必须锚在原件上；
  // Office 原件交给本机编辑页。
  if ((asset.type == QLatin1String("document") ||
       asset.type == QLatin1String("outsource_workbook")) &&
      chosenId.isEmpty())
    for (const CatalogVersion &cv : cat->versionsForAsset(assetId))
      if (cv.stage == QLatin1String("RAW"))
      {
        sourceVersion = cv;
        abs = m_doc->absolutePathForVersion(cv);
        break;
      }

  const auto links = cat->linksForAsset(assetId);
  // 已决井链接 → 多井标签的「井」下拉框数据源（未决链接不进列表，§4）。
  QVector<QPair<QString, QString>> wells; // (entityId, 井名)
  QString linkedBoundary;
  bool hasResolvedNonAux = false;
  bool hasAuxLink = false;
  for (const EntityAssetLink &l : links)
  {
    if (l.unresolved || l.entityId.isEmpty())
      continue;
    if (l.entityType == QLatin1String("well"))
    {
      const CatalogEntity w = cat->entityById(l.entityId);
      wells.append({l.entityId, w.name.isEmpty() ? l.entityId : w.name});
      hasResolvedNonAux = true;
      continue;
    }
    if (l.entityType == QLatin1String("sequence_boundary") && linkedBoundary.isEmpty())
      linkedBoundary = l.entityId;
    if (l.entityType == QLatin1String("auxiliary"))
      hasAuxLink = true;
    else
      hasResolvedNonAux = true;
  }
  // 固定辅助参考（§4 阶段 D）：XML 被内容判成井类但按规则钉在辅助实体上
  // （如 参考资料/ 下的 HZ28-6-1）——链接全部是 auxiliary 时一律走参考面板，
  // 绝不拿 well_head/well_log 类型去解析。
  const bool auxOnly = hasAuxLink && !hasResolvedNonAux;

  QWidget *host = new QWidget(this);
  auto *lay = new QVBoxLayout(host);
  lay->setContentsMargins(0, 0, 0, 0);
  lay->setSpacing(PaleoTheme::tokens().spacingSm);

  // 外链/受管缺失态（§4：「找不到源文件」+路径）。外链版本（wave4）多给一个
  // 「重新定位文件…」出口——服务层流式 SHA-256 复验，内容一致才重接，不一致
  // 如实拒绝；受管文件缺失不是这条恢复路径能解的，不给按钮、只留文案。
  if (abs.isEmpty() || !QFile::exists(abs))
  {
    lay->addWidget(stateLabel(tr("找不到源文件\n%1").arg(abs.isEmpty() ? v.path : abs), host, true), 1);
    if (!sourceVersion.managed && m_doc)
    {
      auto *btn = new QPushButton(tr("重新定位文件…"), host);
      btn->setObjectName(QStringLiteral("relocateBtn"));
      const QString versionId = sourceVersion.id;
      connect(btn, &QPushButton::clicked, host, [this, assetId, versionId] {
        const QString picked = QFileDialog::getOpenFileName(
            this, tr("重新定位源文件"), QString(), QString());
        if (!picked.isEmpty())
          relocateMissingSourceWith(assetId, versionId, picked);
      });
      lay->addWidget(btn, 0, Qt::AlignHCenter);
    }
    return host;
  }

  // 外链完整性（§3）：入库时留过 SHA-256 的源文件被改过就不再解码——
  // 正文如实写「源文件与入库时的 SHA-256 不一致」。
  // D1：地震资产接了任务服务时把这道哈希移交异步解码任务——体量大不该堵
  // 住建标签；辅助 XML 的校验同样交后台解析会话。
  // 托管/无指纹/本会话已验的短路、失配后的下游标过时都在门面里。
  const bool deferShaToTask =
      OfficePreviewSession::supports(abs) ||
      (abs.endsWith(QLatin1String(".xml"), Qt::CaseInsensitive) &&
       (auxOnly || asset.type == QLatin1String("unknown") || asset.type == QLatin1String("auxiliary") ||
        asset.type == QLatin1String("outsource_workbook"))) ||
      (m_doc->taskService() && asset.type == QLatin1String("seismic"));
  if (!deferShaToTask)
  {
    QString verr;
    if (!m_doc->verifyExternalSha(assetId, sourceVersion, &verr))
    {
      lay->addWidget(stateLabel(verr, host, true), 1);
      return host;
    }
  }

  // Office 原件交给本机编辑页。SHA 在后台核对。编辑结果另存为
  // DERIVED 版本，不覆盖 RAW，也不生成 PDF。
  if (OfficePreviewSession::supports(abs))
  {
    auto *office = new OfficePreviewWidget(abs, sourceVersion.managed ? QString() : sourceVersion.sha256, host);
    const QString editAsset = assetId;
    const QString editParent = sourceVersion.id;
    connect(office, &OfficePreviewWidget::editSaved, host, [this, office, editAsset, editParent](const QString &saved) {
      if (!m_doc || !m_doc->catalog()) return;
      QString error;
      if (OfficePreviewSession::commitEdit(m_doc->catalog(), editAsset, editParent, saved, &error))
        office->showMessage(tr("已另存为工程中的新版本，原件未改"));
      else
        office->showMessage(error, true);
    });
    lay->addWidget(office, 1);
    return host;
  }

  if (asset.type == QLatin1String("well_log") && !auxOnly)
    return buildWellLogContent(cat, asset, v, abs, assetId, wells, auxOnly,
                              links, host, lay);

  const bool wellFilterable = asset.type == QLatin1String("well_head") ||
                              asset.type == QLatin1String("well_stratification") ||
                              asset.type == QLatin1String("time_depth");
  if (wellFilterable && !auxOnly)
  {
    if (wells.size() == 1)
    {
      // 恰好一口已决井：直接按它过滤（§4 autoplan：唯一解析时过滤即它）。
      const QString wellId = wells.front().first;
      m_wellEntityOfAsset[assetId] = wellId;
      m_titleSuffixOfAsset[assetId] = wells.front().second;
      lay->addWidget(buildWellBody(asset, abs, wellId, wells.front().second, host), 1);
      return host;
    }
    // T31 死胡同文案：多井 tab 无井可挂（下拉会是空的）时不留空白页——
    // 工程没井指向导入；资产未决指向数据页「挂到这口井」入口。
    if (wells.isEmpty())
    {
      auto *deadEnd = stateLabel(
          cat->entities(QStringLiteral("well")).isEmpty()
              ? tr("工程里还没有井 — 先导入工区文件夹（井位表会建立井）")
              : tr("这个资产还没有挂到任何井 — 在数据页资产表的「未决」行，"
                   "用「挂到这口井」把它挂上"),
          host);
      deadEnd->setObjectName(QStringLiteral("deadEndText"));
      lay->addWidget(deadEnd, 1);
      return host;
    }

    // 多井文件（井口表、DC.dat、多井 TD）或未决资产：每标签自带「井」下拉框，
    // 只列已决链接的井；默认未选 → 正文「先选择一口井」。
    auto *bar = new QWidget(host);
    auto *barLay = new QHBoxLayout(bar);
    barLay->setContentsMargins(0, 0, 0, 0);
    barLay->addWidget(caption8(tr("井"), bar));
    auto *combo = new QComboBox(bar);
    combo->setObjectName(QStringLiteral("wellCombo"));
    combo->setAccessibleName(tr("井"));
    for (const auto &w : wells)
      combo->addItem(w.second, w.first);
    combo->setCurrentIndex(-1); // 默认未选（§4）
    barLay->addWidget(combo);
    barLay->addStretch(1);
    lay->addWidget(bar);

    auto *bodyHost = new QWidget(host);
    auto *bodyLay = new QVBoxLayout(bodyHost);
    bodyLay->setContentsMargins(0, 0, 0, 0);
    lay->addWidget(bodyHost, 1);

    const auto applyWell = [this, assetId, asset, abs, cat, combo, bodyLay, bodyHost,
                            host](const QString &wellId) {
      // 本标签自己的选择：不动其他标签（§4）。
      if (wellId.isEmpty())
        m_wellEntityOfAsset.remove(assetId);
      else
        m_wellEntityOfAsset[assetId] = wellId;
      const QString wname =
          wellId.isEmpty() ? QString() : cat->entityById(wellId).name;
      m_titleSuffixOfAsset[assetId] = wname;
      updateTabTitle(assetId);
      while (QLayoutItem *it = bodyLay->takeAt(0))
      {
        if (QWidget *w = it->widget())
          delete w; // 直接删：发送者（下拉框）不在正文子树里，陈旧控件立刻出树
        delete it;
      }
      if (wellId.isEmpty())
        bodyLay->addWidget(stateLabel(tr("先选择一口井"), bodyHost), 1);
      else
      {
        bodyLay->addWidget(buildWellBody(asset, abs, wellId, wname, bodyHost), 1);
        if (asset.type == QLatin1String("well_head"))
          emit wellSelected(wellId); // §4：选中时地图同时高亮该井
      }
    };
    connect(combo, &QComboBox::currentIndexChanged, host,
            [applyWell, combo](int idx) {
              applyWell(idx >= 0 ? combo->itemData(idx).toString() : QString());
            });
    // 重建时恢复本标签之前选中的井；否则保持未选。
    const QString prev = m_wellEntityOfAsset.value(assetId);
    const int prevIdx = prev.isEmpty() ? -1 : combo->findData(prev);
    if (prevIdx >= 0)
      combo->setCurrentIndex(prevIdx); // 触发 applyWell → 正文按该井渲染
    else
      applyWell(QString());
    return host;
  }

  if (asset.type == QLatin1String("horizon"))
    return buildHorizonContent(cat, asset, v, assetId, linkedBoundary, host, lay);

  if (asset.type == QLatin1String("seismic"))
    return buildSeismicContent(cat, asset, v, abs, assetId, links, host, lay);

  if (asset.type == QLatin1String("image_reference"))
    return buildImageReferenceContent(cat, asset, v, abs, assetId, host, lay);

  if (asset.type == QLatin1String("document"))
  {
    // PDF 原件仍走 QtPdf；Office 后缀已在上方接入 Calligra，绝不转换。
    if (QFileInfo(abs).suffix().compare(QLatin1String("pdf"), Qt::CaseInsensitive) == 0)
    {
      auto *doc = new QPdfDocument(host);
      if (doc->load(abs) == QPdfDocument::Error::None)
      {
        auto *view = new QPdfView(host);
        view->setObjectName(QStringLiteral("pdfView"));
        view->setDocument(doc);
        view->setPageMode(QPdfView::PageMode::MultiPage);
        lay->addWidget(view, 1);
      }
      else
        lay->addWidget(stateLabel(tr("PDF 原件无法加载\n%1").arg(abs), host, true), 1);
    }
    else
    {
      lay->addWidget(stateLabel(tr("该格式暂无内嵌预览"), host), 1);
      lay->addWidget(makeOpenExternalRow(abs, host));
    }
    return host;
  }

  // SpreadsheetML XML 保留原生数据表；xls/xlsx 已由 Calligra 直接预览。
  if (asset.type == QLatin1String("outsource_workbook")) {
    if (abs.endsWith(QLatin1String(".xml"), Qt::CaseInsensitive))
      return buildAuxiliaryXmlContent(abs, assetId, host, lay, sourceVersion.managed ? QString() : sourceVersion.sha256);
    return buildOutsourceWorkbookContent(abs, host, lay);
  }

  if (asset.type == QLatin1String("geojson") ||
      (asset.type == QLatin1String("boundary") &&
       asset.displayName.endsWith(QLatin1String(".geojson"), Qt::CaseInsensitive)))
    return buildGeoJsonContent(cat, asset, v, abs, assetId, host, lay);

  if (isMapProductAsset(asset, v))
    return buildMapProductContent(cat, asset, v, abs, assetId, host, lay);

  // ---- 辅助/参考与未知类型（§4 阶段 D）：预览内容为主，文件名/类型等属性
  // 信息由右侧属性面板承担（不再重复占空间）；「未配准，不加入地图」警告照旧；
  // HZ28-6-1 XML 额外写「不对应 A1–A20」；无内嵌预览时留「用系统程序打开」
  // 兜底出口。----
  {
    QString auxName;
    for (const EntityAssetLink &l : links)
      if (l.entityType == QLatin1String("auxiliary") && !l.entityId.isEmpty())
      {
        auxName = cat->entityById(l.entityId).name;
        break;
      }
    lay->addWidget(warnLabel(tr("未配准，不加入地图"), host));
    // 参考资料/ 下 HZ28-6-1 的 XML：不按内容挂井、不并进 A1–A20（§3 固定规则）。
    if (asset.displayName.contains(QStringLiteral("HZ28-6-1")) ||
        auxName.contains(QStringLiteral("HZ28-6-1")))
      lay->addWidget(warnLabel(tr("不对应 A1–A20"), host));

    if (abs.endsWith(QLatin1String(".xml"), Qt::CaseInsensitive))
    {
      return buildAuxiliaryXmlContent(abs, assetId, host, lay, sourceVersion.managed ? QString() : sourceVersion.sha256);
    }
    // P2 D2.12 未知类型：统一「不支持预览」态 + 可支持类型清单（不再留白）。
    static const QStringList kKnownTypes = {
        QStringLiteral("well_log"),      QStringLiteral("well_head"),
        QStringLiteral("well_stratification"), QStringLiteral("time_depth"),
        QStringLiteral("horizon"),       QStringLiteral("seismic"),
        QStringLiteral("image_reference"), QStringLiteral("document"),
        QStringLiteral("outsource_workbook"),
        QStringLiteral("geojson"),       QStringLiteral("boundary"),
        QStringLiteral("seismic_prediction"), QStringLiteral("wells_prediction"),
        QStringLiteral("composed_facies"), QStringLiteral("facies_polygons"),
        QStringLiteral("edited_facies"), QStringLiteral("single_factor_raster"),
        QStringLiteral("contour_lines"), QStringLiteral("facies_fusion_raster")};
    if (!kKnownTypes.contains(asset.type))
      lay->addWidget(PreviewMapStates::buildUnsupportedPage(asset.type, host), 1);
    else
      lay->addStretch(1);
    lay->addWidget(makeOpenExternalRow(abs, host), 0, Qt::AlignLeft);
    return host;
  }
}

QWidget *DataPreviewTabs::buildWellBody(const CatalogAsset &asset, const QString &absPath,
                                        const QString &wellEntityId,
                                        const QString &wellName, QWidget *parent)
{
  const QString normWell = DataCatalog::normalizeWellName(wellName);
  const auto matchWell = [&normWell](const QString &rowName) {
    return normWell.isEmpty() ||
           DataCatalog::normalizeWellName(rowName) == normWell;
  };

  if (asset.type == QLatin1String("well_stratification"))
  {
    QVector<WellTopRecord> tops;
    QString werr;
    if (!m_doc->wellTopsAt(absPath, &tops, &werr))
      return failureState(asset.id, werr, parent);
    auto *holder = new QWidget(parent);
    auto *hl = new QVBoxLayout(holder);
    hl->setContentsMargins(0, 0, 0, 0);
    hl->setSpacing(PaleoTheme::tokens().spacingSm);
    // §4：层名、MD、TVD、X、Y；Time 列为空就显示空，不填 -99999，也不填假时间。
    auto *table = new QTableWidget(0, 6, holder);
    table->setObjectName(QStringLiteral("topsTable"));
    table->setHorizontalHeaderLabels(
        {tr("层名"), tr("MD"), tr("TVD"), tr("X"), tr("Y"), tr("Time(ms)")});
    table->verticalHeader()->setVisible(false);
    for (const WellTopRecord &t : tops)
    {
      if (!matchWell(t.wellName))
        continue; // 多井文件按当前井过滤，不拆文件（§3）
      const int r = table->rowCount();
      table->insertRow(r);
      table->setItem(r, 0, new QTableWidgetItem(t.topName));
      auto *md = new QTableWidgetItem(t.hasMd ? QString::number(t.md, 'f', 1) : QString());
      auto *tvd = new QTableWidgetItem(t.hasTvd ? QString::number(t.tvd, 'f', 1) : QString());
      auto *x = new QTableWidgetItem(t.hasX ? QString::number(t.x, 'f', 2) : QString());
      auto *y = new QTableWidgetItem(t.hasY ? QString::number(t.y, 'f', 2) : QString());
      auto *tm = new QTableWidgetItem(t.hasTime ? QString::number(t.timeMs, 'f', 1) : QString());
      for (QTableWidgetItem *it : {md, tvd, x, y, tm})
        setNumericItem(it); // JetBrains Mono 9pt 右对齐（§4/DESIGN.md）
      table->setItem(r, 1, md);
      table->setItem(r, 2, tvd);
      table->setItem(r, 3, x);
      table->setItem(r, 4, y);
      table->setItem(r, 5, tm); // Time 空（-99999）就显示空，不填假时间
    }
    table->horizontalHeader()->setStretchLastSection(true);
    hl->addWidget(caption8(wellName.isEmpty() ? tr("分层表")
                                              : tr("%1 的分层表").arg(wellName),
                           holder));
    hl->addWidget(table, 1);

    // P2 D2.8 井位落图：分层行带坐标（X/Y）时把层位顶点打上图。
    bool anyCoords = false;
    for (const WellTopRecord &t : tops)
      if (matchWell(t.wellName) && t.hasX && t.hasY)
      {
        anyCoords = true;
        break;
      }
    if (anyCoords)
    {
      auto *topsVl = makeMemoryPointLayer(
          wellName.isEmpty() ? tr("分层顶点") : tr("%1 分层顶点").arg(wellName), holder);
      for (const WellTopRecord &t : tops)
        if (matchWell(t.wellName) && t.hasX && t.hasY)
          addMemoryPoint(topsVl, t.x, t.y, t.topName, QStringLiteral("well"));
      stylePointLayer(topsVl, true);
      auto *mapPage = new PreviewMapPage(holder);
      mapPage->setObjectName(QStringLiteral("topsPreviewPage"));
      mapPage->mapCanvas()->canvas()->setObjectName(QStringLiteral("topsMapCanvas"));
      mapPage->setProfileEnabled(false);
      mapPage->addMapLayer(topsVl, tr("分层顶点"), absPath);
      auto *labelRow = new QWidget(holder);
      auto *labelLay = new QHBoxLayout(labelRow);
      labelLay->setContentsMargins(0, 0, 0, 0);
      auto *labelToggle = new QCheckBox(tr("名称标注"), labelRow);
      labelToggle->setObjectName(QStringLiteral("topsLabelToggle"));
      labelToggle->setChecked(true);
      labelLay->addWidget(labelToggle);
      labelLay->addStretch(1);
      QObject::connect(labelToggle, &QCheckBox::toggled, mapPage,
                       [topsVl, mapPage](bool on) {
                         topsVl->setLabelsEnabled(on);
                         mapPage->mapCanvas()->canvas()->refresh();
                       });
      hl->addWidget(labelRow);
      hl->addWidget(mapPage, 1);
      QTimer::singleShot(0, holder, [mapPage, topsVl]() {
        mapPage->mapCanvas()->zoomToLayer(topsVl);
      });
    }
    return holder;
  }

  if (asset.type == QLatin1String("time_depth"))
  {
    TimeDepthTable td;
    QString terr;
    if (!m_doc->timeDepthAt(absPath, &td, &terr))
      return failureState(asset.id, terr, parent);
    QVector<double> tvds, times;
    for (const TdRow &r : td.rows)
    {
      if (!r.hasTvd)
        continue;
      tvds.append(r.tvd);
      times.append(r.timeMs);
    }
    // §4：time_depth 没有可用样点时写「无时深表」，不画假线。
    if (tvds.isEmpty())
      return stateLabel(tr("无时深表"), parent);
    auto *panel = new CurvePanel(parent);
    panel->setEmptyText(tr("无时深表")); // 双保险：NaN 过滤后仍空的兜底文案
    panel->setCurve(tr("TIME–TVD"), QStringLiteral("ms"), times, tvds);
    return panel;
  }

  if (asset.type == QLatin1String("well_head"))
  {
    QVector<WellHeadRecord> rows;
    QString herr;
    if (!m_doc->wellHeadsAt(absPath, &rows, &herr))
      return failureState(asset.id, herr, parent);
    auto *holder = new QWidget(parent);
    auto *hl = new QVBoxLayout(holder);
    hl->setContentsMargins(0, 0, 0, 0);
    hl->setSpacing(PaleoTheme::tokens().spacingSm);
    // 预览按当前井过滤（多井井位文件；井名规范化后比较）
    const WellHeadRecord *rec = nullptr;
    for (const WellHeadRecord &r : rows)
      if (matchWell(r.name))
        rec = &r;
    if (!rec && !wellName.isEmpty())
    {
      hl->addWidget(stateLabel(tr("井 %1 不在该井位文件中").arg(wellName), holder), 1);
      return holder;
    }
    // §4 字段不变（井名/X/Y/KB/TD/BottomX/BottomY/WellType/坐标状态），
    // 收为单行字段条——纵列信息卡占高约十行，压缩后纵向让给地图；
    // 数字仍 JetBrains Mono（DESIGN mono 约定）。
    auto *info = new QWidget(holder);
    auto *grid = new QHBoxLayout(info);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(PaleoTheme::tokens().spacingSm * 2);
    const auto addField = [&](const QString &k, const QString &val, bool mono = false,
                              bool muted = false) {
      grid->addWidget(caption8(k, info));
      auto *v = valueLabel(val, info, mono);
      if (muted)
        PaleoTheme::applyThemedStyleSheet(
            v, [] { return PaleoTheme::mutedCaptionStyleSheet(); }); // text-muted
      grid->addWidget(v);
    };
    if (rec)
    {
      addField(tr("井名"), rec->name);
      addField(tr("X"), QString::number(rec->x, 'f', 2), true);
      addField(tr("Y"), QString::number(rec->y, 'f', 2), true);
      addField(tr("KB"), QString::number(rec->kb, 'f', 2), true);
      addField(tr("TD"), QString::number(rec->td, 'f', 2), true);
      addField(tr("BottomX"),
               rec->hasBottomX ? QString::number(rec->bottomX, 'f', 2) : QString(), true);
      addField(tr("BottomY"),
               rec->hasBottomY ? QString::number(rec->bottomY, 'f', 2) : QString(), true);
      addField(tr("WellType"), rec->wellType);
    }
    const QString status =
        wellEntityId.isEmpty()
            ? QString()
            : m_doc->catalog()->entityById(wellEntityId).coordinateStatus;
    // T27：坐标状态中文化 + text-muted（计划 §4：这些状态仍用 #5D6E80）。
    addField(tr("坐标状态"), coordinateStatusText(status), false, true);
    grid->addStretch(1);
    hl->addWidget(info);

    // P2 D2.6 井位地图预览：全部井位打点 + 当前井高亮 + 名称标注开关。
    auto *wellsVl = makeMemoryPointLayer(tr("井位"), holder);
    for (const WellHeadRecord &r : rows)
      if (r.x > -99990.0 && r.y > -99990.0) // 坐标哨兵过滤
        addMemoryPoint(wellsVl, r.x, r.y, r.name,
                       (rec && matchWell(r.name)) ? QStringLiteral("highlight")
                                                   : QStringLiteral("well"));
    stylePointLayer(wellsVl, true);
    auto *mapPage = new PreviewMapPage(holder);
    mapPage->setObjectName(QStringLiteral("wellHeadPreviewPage"));
    mapPage->mapCanvas()->canvas()->setObjectName(QStringLiteral("wellHeadMapCanvas"));
    mapPage->setProfileEnabled(false);
    QgsMapCanvas *canvas = mapPage->mapCanvas()->canvas();
    // 与测区全景同一口径：有效配准的工程把离线底图克隆进预览画布（独立实例
    // 随标签释放），overrideCrs 让画布跟随工程地图坐标系。
    if (m_project)
    {
      canvas->setProject(m_project);
      canvas->mapSettings().setTransformContext(m_project->transformContext());
      mapPage->mapCanvas()->setOverrideCrs(m_project->crs());
      const auto baseLayers = m_project->layerTreeRoot()->findLayers();
      for (auto it = baseLayers.crbegin(); it != baseLayers.crend(); ++it)
      {
        auto *layer = (*it)->layer();
        if (!layer || !layer->customProperty("paleoBasemap").toBool())
          continue;
        if (auto *base = paleo::mapreference::offlineBasemap(
                layer->customProperty("paleoBasemapPath").toString(), layer->name(),
                canvas))
          mapPage->addMapLayer(base, base->name(), QString());
      }
    }
    // 井位预览是跟随内容缩放的交互画布，不出鹰眼（同测区全景页口径）。
    if (auto *ovAction =
            mapPage->findChild<QAction *>(QStringLiteral("previewOverviewAction")))
    {
      ovAction->setChecked(false); // toggled → setOverviewVisible(false)
      ovAction->setVisible(false);
    }
    else
    {
      mapPage->setOverviewVisible(false);
    }
    mapPage->addMapLayer(wellsVl, tr("井位"), absPath);
    auto *labelRow = new QWidget(holder);
    auto *labelLay = new QHBoxLayout(labelRow);
    labelLay->setContentsMargins(0, 0, 0, 0);
    auto *labelToggle = new QCheckBox(tr("名称标注"), labelRow);
    labelToggle->setObjectName(QStringLiteral("wellLabelToggle"));
    labelToggle->setChecked(true);
    labelLay->addWidget(labelToggle);
    labelLay->addStretch(1);
    // 底图版权方标注 + 高亮提示收进同一行（不占独立行）。
    if (m_project)
    {
      const QString attr = paleo::mapreference::attribution(canvas);
      if (!attr.isEmpty())
      {
        auto *src = caption8(attr, labelRow);
        src->setObjectName(QStringLiteral("wellHeadBasemapAttribution"));
        labelLay->addWidget(src);
      }
    }
    labelLay->addWidget(caption8(tr("选中时地图同时高亮该井"), labelRow));
    QObject::connect(labelToggle, &QCheckBox::toggled, mapPage,
                     [wellsVl, mapPage](bool on) {
                       wellsVl->setLabelsEnabled(on);
                       mapPage->mapCanvas()->canvas()->refresh();
                     });
    hl->addWidget(labelRow);
    hl->addWidget(mapPage, 1);
    QTimer::singleShot(0, holder, [mapPage, wellsVl]() {
      mapPage->mapCanvas()->zoomToLayer(wellsVl);
    });
    return holder;
  }

  return stateLabel(tr("先选择一口井"), parent); // 兜底（不可达）
}

// ---- D2.10 同目录组图：叠一层同目录可地图化资产（geojson/带配准图片/
// 层位栅格）。不支持的类型如实跳过，不造假层。----
void DataPreviewTabs::addSiblingOverlayButton(DataCatalog *catalog, const QString &sourcePath,
                                               const QString &assetId, PreviewMapPage *page, QWidget *owner)
{
  const auto siblings = PreviewMapStates::siblingMappableAssets(
      catalog, sourcePath, [this](const CatalogVersion &v) { return m_doc->absolutePathForVersion(v); });
  QVector<QPair<QString, QString>> others;
  for (const auto &sib : siblings)
    if (sib.first != assetId)
      others.append(sib);
  if (others.isEmpty())
    return;
  auto *overlayBtn = new QToolButton(page);
  overlayBtn->setObjectName(QStringLiteral("siblingOverlayButton"));
  overlayBtn->setText(tr("同目录叠加"));
  overlayBtn->setToolTip(tr("把同目录下的相图/配准图片/层位栅格叠加到本预览"));
  overlayBtn->setPopupMode(QToolButton::InstantPopup);
  auto *menu = new QMenu(overlayBtn);
  for (const auto &sib : others)
  {
    QAction *act = menu->addAction(sib.second);
    QObject::connect(act, &QAction::triggered, owner, [this, page, sib, owner]() {
      addSiblingOverlayLayer(page, sib.first, sib.second, owner);
    });
  }
  overlayBtn->setMenu(menu);
  page->addToolBarWidget(overlayBtn);
}

void DataPreviewTabs::addSiblingOverlayLayer(PreviewMapPage *page, const QString &sibAssetId,
                                             const QString &sibName, QWidget *owner)
{
  if (!page || !m_doc || sibAssetId.isEmpty())
    return;
  DataCatalog *cat = m_doc->catalog();
  const CatalogAsset asset = cat->assetById(sibAssetId);
  if (asset.id.isEmpty())
    return;
  const CatalogVersion v = cat->currentVersion(sibAssetId);
  const QString abs = m_doc->absolutePathForVersion(v);
  if (abs.isEmpty() || !QFile::exists(abs))
    return;

  if (asset.type == QLatin1String("geojson") ||
      (asset.type == QLatin1String("boundary") &&
       abs.endsWith(QLatin1String(".geojson"), Qt::CaseInsensitive)))
  {
    auto *vl = new QgsVectorLayer(abs, sibName, QStringLiteral("ogr"));
    vl->setParent(owner);
    if (!vl->isValid())
    {
      vl->deleteLater();
      return;
    }
    QString field;
    for (const QgsField &f : vl->fields())
    {
      const QString n = f.name();
      if (n == QLatin1String("相") || n.contains(QLatin1String("相")) ||
          n.compare(QLatin1String("facies"), Qt::CaseInsensitive) == 0)
      {
        field = n;
        break;
      }
    }
    if (!field.isEmpty())
      applyFaciesRendererToLayer(vl, field);
    page->addMapLayer(vl, sibName, abs);
    return;
  }
  if (asset.type == QLatin1String("image_reference"))
  {
    if (PreviewMapStates::detectWorldFile(abs).isEmpty())
      return; // 未配准图片不进地图（D2.7 语义）
    auto *rl = new QgsRasterLayer(abs, sibName, QStringLiteral("gdal"));
    rl->setParent(owner);
    if (!rl->isValid() || rl->extent().isEmpty())
    {
      rl->deleteLater();
      return;
    }
    page->addMapLayer(rl, sibName, abs);
    return;
  }
  if (asset.type == QLatin1String("horizon"))
  {
    CatalogVersion best;
    for (const CatalogVersion &cv : cat->versionsForAsset(sibAssetId))
      if (cv.stage == QLatin1String("DERIVED") && cv.versionNumber >= best.versionNumber)
        best = cv;
    if (best.id.isEmpty())
      return;
    const QString tif = m_doc->absolutePathForVersion(best);
    auto *rl = new QgsRasterLayer(tif, sibName, QStringLiteral("gdal"));
    rl->setParent(owner);
    if (!rl->isValid() || rl->extent().isEmpty())
    {
      rl->deleteLater();
      return;
    }
    const auto sum = PreviewRasterAnalysis::summarize(rl);
    if (sum.valid)
      PreviewRasterAnalysis::applyPseudoColorRenderer(
          rl, 1, sum.min, sum.max,
          *PreviewRasterAnalysis::rampPreset(QStringLiteral("terrain")), false,
          PreviewRasterAnalysis::Classification::Continuous);
    page->addMapLayer(rl, sibName, tif);
    return;
  }
}

// ---- 测线解码结果应用（PreviewDocService 信号 → 挂起控件组）----
// 陈旧结果与 SHA 标过时都在服务内做完；这里只把最新一代贴上控件。
void DataPreviewTabs::onSectionReady(const QString &assetId,
                                     const PreviewDocService::SectionDoc &doc)
{
  const SectionPending pend = m_pendingSection.value(assetId);
  if (!pend.panel)
    return;
  // SectionPanel 是本 cpp 内聚的预览控件——挂起时存的是它。
  auto *sp = static_cast<SectionPanel *>(pend.panel.data());
  sp->setTraces(doc.traces, doc.sampleIntervalUs, doc.startTimeMs, doc.readReport.message);
  if (pend.hasTie)
    sp->setTieMarker(pend.tieText, pend.tieMs);
  // 标题后缀：「文件名 · IL1315」/「文件名 · XL4165」（§4）。
  m_titleSuffixOfAsset[assetId] =
      (doc.isInline ? QStringLiteral("IL") : QStringLiteral("XL")) +
      QString::number(doc.lineNo);
  updateTabTitle(assetId);
  if (pend.mode)
  {
    pend.mode->setEnabled(true);
    pend.mode->setToolTip(doc.readReport.message);
  }
  if (pend.spin)
  {
    pend.spin->setEnabled(true);
    pend.spin->setToolTip(doc.readReport.message);
  }
}

void DataPreviewTabs::onSectionFailed(const QString &assetId,
                                      const QString &reason)
{
  const SectionPending pend = m_pendingSection.value(assetId);
  if (auto *sp = static_cast<SectionPanel *>(
          pend.panel ? pend.panel.data() : nullptr))
    sp->setError(reason.isEmpty() ? tr("无法解码测线") : reason); // §4：如实写，不装灰图
  if (pend.mode)
  {
    pend.mode->setEnabled(true);
    pend.mode->setToolTip(QString());
  }
  if (pend.spin)
  {
    pend.spin->setEnabled(true);
    pend.spin->setToolTip(QString());
  }
}

void DataPreviewTabs::onLasReady(const QString &key, const QStringList &, const QList<LasCurve> &curves)
{
  // F1（goal/perf-systematize 簇2）：数据行到达——fill 闭包内自带 QPointer
  // 护栏（页没了就不装）；服务侧世代号已保证这是最新一代。兄弟文件文档
  // 在 lasReady 之前已写入门面，失败的不在表里。
  if (!m_pendingLas.contains(key))
    return;
  const LasPending pend = m_pendingLas.take(key);
  const QHash<QString, LasDoc> siblings =
      m_doc ? m_doc->lasSiblingDocs(key) : QHash<QString, LasDoc>();
  if (pend.fill)
    pend.fill(curves, siblings);
}

void DataPreviewTabs::onLasFailed(const QString &key, const QString &reason)
{
  // 头部能解但整份解析失败（截断/坏行）：页面骨架已建好——把视图栈内容
  // 换成「读取失败」面（带重试=重建标签重走两段式），与其它失败态同一
  // 形态（§4）。呈现切换条保留（重试成功后仍有用）。
  if (!m_pendingLas.contains(key))
    return;
  const LasPending pend = m_pendingLas.take(key);
  if (auto *stack = qobject_cast<QStackedWidget *>(pend.page.data()))
  {
    while (stack->count() > 0)
    {
      QWidget *w = stack->widget(0);
      stack->removeWidget(w);
      w->deleteLater();
    }
    stack->addWidget(failureState(key, reason.isEmpty() ? tr("无法解析 LAS 文件") : reason, stack));
  }
}
