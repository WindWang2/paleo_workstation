// 层：视图
#include "datapreviewtabs.h"
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
  lay->setSpacing(4);

  m_tabs = new QTabWidget(this);
  m_tabs->setObjectName(QStringLiteral("dataPreviewTabs"));
  m_tabs->setTabsClosable(true);
  m_tabs->setUsesScrollButtons(true); // T32：标签超宽滚动，不挤压
  m_tabs->setAccessibleName(tr("预览"));
  // dock 面板样式（DESIGN.md）：无工作流蓝下划线，安静边框；活体跟随主题。
  PaleoTheme::applyThemedStyleSheet(m_tabs, [] {
    const PaleoTheme::ThemeTokens &t = PaleoTheme::tokens();
    return QStringLiteral(
        "QTabWidget::pane { border: 1px solid %1; background: %2; top: -1px; }"
        "QTabBar::tab { padding: 4px 10px; color: %3; border: 1px solid %1;"
        " border-bottom: none; background: %2; }"
        "QTabBar::tab:selected { color: %4; font-weight: 600; }")
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
  // 文档 PDF 转换完成/失败 → 重建该资产标签（「转换中」→ 预览或降级面）。
  connect(m_doc, &PreviewDocService::documentPdfReady, this,
          [this](const QString &assetId) { rebuildAssetTab(assetId); });
  connect(m_doc, &PreviewDocService::documentPdfFailed, this,
          [this](const QString &assetId, const QString &) { rebuildAssetTab(assetId); });
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
  pageLay->addWidget(content ? content : stateLabel(tr("无法生成测区地图"), page), 1);

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
  tbLay->setContentsMargins(8, 4, 8, 4);
  tbLay->setSpacing(8);
  stylePreviewToolBar(topBar);

  auto *lblTitle = new QLabel(tr("测区全景地图 (QGIS 画布)"), topBar);
  PaleoTheme::applyThemedStyleSheet(lblTitle, [] {
    return QStringLiteral("font-weight: 600; color: %1; font-size: 9pt;")
        .arg(qssHex(PaleoTheme::tokens().text));
  });
  tbLay->addWidget(lblTitle);

  tbLay->addSpacing(8);

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
    QgsPolylineXY ring;
    for (const auto &c : survey.corners)
      ring.append(QgsPointXY(c.first, c.second));
    if (!ring.isEmpty() && ring.first() != ring.last())
      ring.append(ring.first());
    surveyGeom = QgsGeometry::fromPolygonXY(QgsPolygonXY{ring});
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
    boundaryBand->setToGeometry(surveyGeom, nullptr);
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
    extentStr = tr("工区范围: %1 km × %2 km · 局部工程坐标系统 (米)")
                    .arg(QString::number(wKm, 'f', 1), QString::number(hKm, 'f', 1));
  }
  else
  {
    extentStr = tr("局部工程坐标系统 (米)");
  }
  auto *crsLabel = new QLabel(extentStr, topBar);
  crsLabel->setObjectName(QStringLiteral("surveyAreaExtentLabel"));
  QFont crsFont = PaleoTheme::monoFont();
  crsFont.setPointSize(PaleoTheme::kLabelPt);
  crsLabel->setFont(crsFont);
  PaleoTheme::applyThemedStyleSheet(crsLabel,
                                    [] { return PaleoTheme::mutedCaptionStyleSheet(); });
  tbLay->addWidget(crsLabel);

  lay->addWidget(new PaleoToolRow(topBar, w));
  lay->addWidget(mapPage, 1);

  auto zoomFull = [canvas, surveyGeom]() {
    if (!surveyGeom.isNull() && !surveyGeom.boundingBox().isEmpty())
    {
      QgsRectangle ext = surveyGeom.boundingBox();
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
  l->setSpacing(4);
  l->addStretch(1);
  l->addWidget(stateLabel(tr("读取失败\n%1\n%2").arg(reason, name), box));
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
  // B 包 staleness-lite：资产当前版本被标 stale（上游 sha 失配/被取代）→
  // 标题带「过时」徽标——下游产物过期在数据页如实可见。
  if (m_doc && m_doc->catalog()->currentVersion(assetId)
                   .extra.value(QStringLiteral("stale"))
                   .toBool())
    title += QStringLiteral(" · ") + tr("过时");
  m_tabs->setTabText(idx, title);
}

void DataPreviewTabs::rebuildAssetTab(const QString &assetId)
{
  QWidget *page = m_pageOfAsset.value(assetId);
  if (!page || !m_doc)
    return;
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
  QWidget *content = buildContent(assetId, page);
  loading->setVisible(false); // 保留在树里，便于测试/诊断读取中态
  pageLay->addWidget(content ? content : stateLabel(tr("无法生成预览"), page), 1);
  updateTabTitle(assetId);
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
  pageLay->setContentsMargins(8, 8, 8, 8);

  QLabel *loading = loadingLabel(displayName.isEmpty() ? assetId : displayName, page);
  pageLay->addWidget(loading, 1);

  const int idx = m_tabs->addTab(page, displayName.isEmpty() ? assetId : displayName);
  m_pageOfAsset.insert(assetId, page);
  m_tabs->setVisible(true);
  m_emptyLabel->setVisible(false);
  m_tabs->setCurrentIndex(idx);
  loading->repaint(); // 「正在读取」+文件名在同步读取前先可见（§4）

  QWidget *content = buildContent(assetId, page);
  loading->setVisible(false); // 读取完成；隐藏但保留节点便于测试断言该状态
  pageLay->addWidget(content ? content : stateLabel(tr("无法生成预览"), page), 1);
  updateTabTitle(assetId);
  focusWellIfNeeded(assetId, page);
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
  Q_UNUSED(page);
  DataCatalog *cat = m_doc->catalog();
  const CatalogAsset asset = cat->assetById(assetId);
  if (asset.id.isEmpty())
    return nullptr;
  const CatalogVersion v = cat->currentVersion(assetId);
  CatalogVersion sourceVersion = v; // abs 实际对应的版本（文档标签锚回 RAW 原件）
  QString abs = m_doc->absolutePathForVersion(v);
  // 文档资产：RAW 原件是规范来源——currentVersion 可能已指向 DERIVED
  // PDF 转换件，缺失检查与「用系统程序打开」必须锚在原件上。
  if (asset.type == QLatin1String("document"))
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
  lay->setSpacing(8);

  // 外链/受管缺失态（§4：「找不到源文件」+路径）。外链版本（wave4）多给一个
  // 「重新定位文件…」出口——服务层流式 SHA-256 复验，内容一致才重接，不一致
  // 如实拒绝；受管文件缺失不是这条恢复路径能解的，不给按钮、只留文案。
  if (abs.isEmpty() || !QFile::exists(abs))
  {
    lay->addWidget(stateLabel(tr("找不到源文件\n%1").arg(abs.isEmpty() ? v.path : abs), host), 1);
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
  // 住建标签；其它资产类型文件小，保留同步门（会话已验过的资产直接跳过）。
  // 托管/无指纹/本会话已验的短路、失配后的下游标过时都在门面里。
  const bool deferShaToTask =
      m_doc->taskService() && asset.type == QLatin1String("seismic");
  if (!deferShaToTask)
  {
    QString verr;
    if (!m_doc->verifyExternalSha(assetId, sourceVersion, &verr))
    {
      lay->addWidget(stateLabel(verr, host), 1);
      return host;
    }
  }

  if (asset.type == QLatin1String("well_log") && !auxOnly)
  {
    // 单井曲线：按已决链接过滤（LAS 本就是单井文件），标题带井名。
    QString linkedWell;
    if (!wells.isEmpty())
      linkedWell = wells.front().first;
    if (!linkedWell.isEmpty())
    {
      m_wellEntityOfAsset[assetId] = linkedWell;
      m_titleSuffixOfAsset[assetId] = wells.front().second;
    }
    // F1 两段式（goal/perf-systematize 簇2）：lasHeaderAt 只读 ~V/~W/~C 到
    // ~A 段头（代价与头部行数成正比、与数据行数无关）——曲线名秒出，整页
    // 控件骨架同步铺完；数据行 requestLas 池内解析（大文件冷解析曾 400ms+
    // 阻塞 UI），lasReady 到达后补数据与单位。无任务服务时 requestLas 同步
    // 执行、返回前信号已发——测试环境行为与旧路径一致。
    LasHeaderInfo header;
    QString perr;
    if (!m_doc->lasHeaderAt(abs, &header, &perr))
    {
      lay->addWidget(failureState(assetId, perr, host), 1);
      return host;
    }
    const QStringList names = header.curveNames;
    auto *singlePage = new QWidget(host);
    auto *singleLay = new QVBoxLayout(singlePage);
    singleLay->setContentsMargins(0, 0, 0, 0);
    singleLay->setSpacing(8);

    auto *panel = new CurvePanel(singlePage);
    panel->setObjectName(QStringLiteral("curvePanel"));
    panel->setEmptyText(tr("这条曲线没有有效样点")); // §4：整条 -99999 → 不绘制

    // 1. 顶部控制栏（主选曲线 + 预设 + 缩放控制）
    auto *topBar = new QWidget(singlePage);
    auto *topLay = new QHBoxLayout(topBar);
    topLay->setContentsMargins(0, 0, 0, 0);
    topLay->setSpacing(8);

    auto *combo = new QComboBox(topBar);
    combo->setObjectName(QStringLiteral("curveCombo"));
    combo->setAccessibleName(tr("曲线"));
    for (int i = 1; i < names.size(); ++i) // curves[0] 是深度道
      combo->addItem(names.at(i), i); // userData = curves 下标（禁用项不受序号偏移影响）

    // §4：约定的 GR/AC/DEN 缺了就给禁用项，tooltip 写「这条曲线不在文件里」。
    static const QStringList kExpected{QStringLiteral("GR"), QStringLiteral("AC"),
                                       QStringLiteral("DEN")};
    for (const QString &cn : kExpected)
      if (combo->findText(cn) < 0)
      {
        const int j = combo->count();
        combo->addItem(cn, -1);
        combo->setItemData(j, tr("这条曲线不在文件里"), Qt::ToolTipRole);
        auto *model = qobject_cast<QStandardItemModel *>(combo->model());
        if (model && model->item(j))
          model->item(j)->setEnabled(false);
      }

    const int def = combo->findText(QStringLiteral("GR"));
    if (def >= 0 && combo->itemData(def).toInt() > 0) // 禁用项不当作默认曲线
      combo->setCurrentIndex(def);

    // 深度缩放按钮组
    auto *btnZoomOut = new QToolButton(topBar);
    btnZoomOut->setText(QStringLiteral("−"));
    btnZoomOut->setToolTip(tr("缩小深度 (Ctrl+滚轮向下)"));
    btnZoomOut->setStyleSheet(QStringLiteral("QToolButton { font-weight: bold; min-width: 24px; min-height: 22px; }"));

    auto *lblZoom = new QLabel(QStringLiteral("100%"), topBar);
    lblZoom->setFont(monoFont());
    PaleoTheme::applyThemedStyleSheet(lblZoom, [] {
      return QStringLiteral("color: %1; min-width: 44px;")
          .arg(qssHex(PaleoTheme::tokens().textMuted));
    });
    lblZoom->setAlignment(Qt::AlignCenter);

    auto *btnZoomIn = new QToolButton(topBar);
    btnZoomIn->setText(QStringLiteral("+"));
    btnZoomIn->setToolTip(tr("放大深度 (Ctrl+滚轮向上)"));
    btnZoomIn->setStyleSheet(QStringLiteral("QToolButton { font-weight: bold; min-width: 24px; min-height: 22px; }"));

    auto *btnZoomReset = new QToolButton(topBar);
    btnZoomReset->setText(tr("1:1 适应"));
    btnZoomReset->setToolTip(tr("重置为全井深 (双击图道重置)"));
    btnZoomReset->setStyleSheet(QStringLiteral("QToolButton { min-height: 22px; padding: 0 6px; }"));

    // 曲线快速预设按钮
    auto *btnSelectDefault = new QToolButton(topBar);
    btnSelectDefault->setText(tr("常规(GR/AC/DEN)"));
    btnSelectDefault->setToolTip(tr("显示三孔隙/常规测井曲线"));
    btnSelectDefault->setStyleSheet(QStringLiteral("QToolButton { min-height: 22px; padding: 0 6px; }"));

    auto *btnSelectAll = new QToolButton(topBar);
    btnSelectAll->setText(tr("全选"));
    btnSelectAll->setToolTip(tr("同时显示所有曲线"));
    btnSelectAll->setStyleSheet(QStringLiteral("QToolButton { min-height: 22px; padding: 0 6px; }"));

    auto *btnClear = new QToolButton(topBar);
    btnClear->setText(tr("仅主选"));
    btnClear->setToolTip(tr("仅显示当前下拉框选中的单根曲线"));
    btnClear->setStyleSheet(QStringLiteral("QToolButton { min-height: 22px; padding: 0 6px; }"));

    topLay->addWidget(caption8(tr("主选曲线:"), topBar));
    topLay->addWidget(combo);
    topLay->addSpacing(8);
    topLay->addWidget(btnSelectDefault);
    topLay->addWidget(btnSelectAll);
    topLay->addWidget(btnClear);
    topLay->addStretch(1);
    topLay->addWidget(caption8(tr("深度缩放:"), topBar));
    topLay->addWidget(btnZoomOut);
    topLay->addWidget(lblZoom);
    topLay->addWidget(btnZoomIn);
    topLay->addWidget(btnZoomReset);

    // 初始显示曲线集合（默认优先显示 GR/AC/DEN 常规三孔隙）
    QSet<QString> defaultShown;
    if (combo->findText(QStringLiteral("GR")) >= 0 && combo->itemData(combo->findText(QStringLiteral("GR"))).toInt() > 0)
      defaultShown.insert(QStringLiteral("GR"));
    if (combo->findText(QStringLiteral("AC")) >= 0 && combo->itemData(combo->findText(QStringLiteral("AC"))).toInt() > 0)
      defaultShown.insert(QStringLiteral("AC"));
    if (combo->findText(QStringLiteral("DEN")) >= 0 && combo->itemData(combo->findText(QStringLiteral("DEN"))).toInt() > 0)
      defaultShown.insert(QStringLiteral("DEN"));
    if (defaultShown.isEmpty() && names.size() > 1)
      defaultShown.insert(names.at(1));

    // 曲线数据本体（values/unit）两段式第二段到达后经 fill 回调补装——
    // 此处只建骨架（chips/下拉/缩放/呈现切换），不碰数据行。

    // 2. 曲线多选 Chips 栏（横向滚动条，支持单击自由切换各曲线可见性）
    auto *chipScroll = new QScrollArea(singlePage);
    chipScroll->setWidgetResizable(true);
    chipScroll->setFixedHeight(32);
    chipScroll->setFrameShape(QFrame::NoFrame);
    chipScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    chipScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    chipScroll->setStyleSheet(QStringLiteral("QScrollArea { background: transparent; border: none; }"));

    auto *chipContainer = new QWidget(chipScroll);
    chipContainer->setStyleSheet(QStringLiteral("background: transparent;"));
    auto *chipLay = new QHBoxLayout(chipContainer);
    chipLay->setContentsMargins(0, 0, 0, 0);
    chipLay->setSpacing(8);
    chipLay->addWidget(caption8(tr("多曲线叠合:"), chipContainer));

    // 曲线 chip：描边/字色用曲线数据色（数据符号，豁免）；底/边/悬停走 chrome
    // token。切换时重算当前主题样式，活体注册保证运行中换主题跟随。
    const auto chipStyle = [](const QColor &col, bool on) {
      const PaleoTheme::ThemeTokens &t = PaleoTheme::tokens();
      if (on)
        return QStringLiteral(
            "QToolButton { background: %1; border: 1.5px solid %2; border-radius: 8px; "
            "color: %2; font-weight: bold; padding: 1px 7px; font-size: 8pt; }"
            "QToolButton:hover { background: %3; }")
            .arg(qssHex(t.surface), col.name(), qssHex(t.surfaceAltRaised));
      return QStringLiteral(
          "QToolButton { background: %1; border: 1px solid %2; border-radius: 8px; "
          "color: %3; padding: 1px 7px; font-size: 8pt; }"
          "QToolButton:hover { background: %4; border-color: %5; }")
          .arg(qssHex(t.surface), qssHex(t.border), qssHex(t.textMuted),
               qssHex(t.surfaceAltRaised), qssHex(t.textDisabled));
    };

    auto chipMap = std::make_shared<QHash<QString, QToolButton *>>();
    for (int i = 1; i < names.size(); ++i)
    {
      const QString &cname = names.at(i);
      const QColor col = pickCurveColor(cname, i - 1);
      auto *chip = new QToolButton(chipContainer);
      chip->setText(cname);
      chip->setCheckable(true);
      const bool isChecked = defaultShown.contains(cname);
      chip->setChecked(isChecked);
      chip->setToolTip(cname); // 单位两段式第二段（lasReady）随数据补写

      PaleoTheme::applyThemedStyleSheet(chip, [chipStyle, chip, col] {
        return chipStyle(col, chip->isChecked());
      });

      connect(chip, &QToolButton::toggled, host, [panel, chip, cname, chipStyle, col](bool on) {
        panel->setCurveVisible(cname, on);
        chip->setStyleSheet(chipStyle(col, on));
      });

      (*chipMap)[cname] = chip;
      chipLay->addWidget(chip);
    }
    chipLay->addStretch(1);
    chipScroll->setWidget(chipContainer);

    // 缩放接线
    connect(btnZoomIn, &QToolButton::clicked, host, [panel]() { panel->zoomIn(); });
    connect(btnZoomOut, &QToolButton::clicked, host, [panel]() { panel->zoomOut(); });
    connect(btnZoomReset, &QToolButton::clicked, host, [panel]() { panel->resetZoom(); });
    panel->onZoomChanged = [lblZoom](double z) {
      lblZoom->setText(QStringLiteral("%1%").arg(qRound(z * 100)));
    };

    // 主选下拉框变更时，自动确保该曲线被勾选显示
    connect(combo, &QComboBox::currentIndexChanged, host, [panel, combo, names, chipMap]() {
      const int ci = combo->currentData().toInt();
      if (ci <= 0 || ci >= names.size())
        return;
      const QString &selName = names.at(ci);
      if (chipMap->contains(selName))
      {
        auto *btn = chipMap->value(selName);
        if (!btn->isChecked())
          btn->setChecked(true);
      }
    });

    // 预设按钮事件
    connect(btnSelectAll, &QToolButton::clicked, host, [chipMap]() {
      for (auto *b : *chipMap)
        if (!b->isChecked()) b->setChecked(true);
    });

    connect(btnSelectDefault, &QToolButton::clicked, host, [chipMap, defaultShown]() {
      for (auto it = chipMap->begin(); it != chipMap->end(); ++it)
      {
        const bool on = defaultShown.contains(it.key());
        if (it.value()->isChecked() != on)
          it.value()->setChecked(on);
      }
    });

    connect(btnClear, &QToolButton::clicked, host, [combo, names, chipMap]() {
      const int ci = combo->currentData().toInt();
      const QString activeName = (ci > 0 && ci < names.size()) ? names.at(ci) : QString();
      for (auto it = chipMap->begin(); it != chipMap->end(); ++it)
      {
        const bool on = (it.key() == activeName);
        if (it.value()->isChecked() != on)
          it.value()->setChecked(on);
      }
    });

    singleLay->addWidget(new PaleoToolRow(topBar, singlePage));
    singleLay->addWidget(chipScroll);
    singleLay->addWidget(panel, 1);

    // ResFormStar 多井道综合柱状图总装（骨架即建；曲线数据两段式第二段补装）
    auto *compPanel = new WellComposite::WellCompositePanel(host);
    compPanel->setObjectName(QStringLiteral("wellCompositePanel"));

    // 查询该井是否有关联分层数据 (DC.dat)
    QVector<WellComposite::FormationInterval> formationIntervals;
    if (!linkedWell.isEmpty())
    {
      static const QVector<QColor> kFormColors = {
          QColor(QStringLiteral("#FFE082")), QColor(QStringLiteral("#FFF59D")),
          QColor(QStringLiteral("#C8E6C9")), QColor(QStringLiteral("#A5D6A7")),
          QColor(QStringLiteral("#80CBC4")), QColor(QStringLiteral("#80DEEA")),
          QColor(QStringLiteral("#90CAF9")), QColor(QStringLiteral("#B39DDB"))};

      const auto wLinks = cat->linksForEntity(linkedWell);
      for (const auto &lk : wLinks)
      {
        if (lk.role == QLatin1String("tops"))
        {
          CatalogAsset topsAsset = cat->assetById(lk.assetId);
          CatalogVersion topsVer = cat->currentVersion(lk.assetId);
          QString topsPath = m_doc->absolutePathForVersion(topsVer);
          {
            QVector<WellTopRecord> tops;
            if (!topsPath.isEmpty() && m_doc->wellTopsAt(topsPath, &tops))
            {
              const QString normWell = DataCatalog::normalizeWellName(wells.isEmpty() ? QString() : wells.front().second);
              QVector<WellTopRecord> wellTops;
              for (const auto &tr : tops)
              {
                if (normWell.isEmpty() || DataCatalog::normalizeWellName(tr.wellName) == normWell)
                  wellTops.append(tr);
              }
              std::sort(wellTops.begin(), wellTops.end(), [](const WellTopRecord &a, const WellTopRecord &b) {
                return a.md < b.md;
              });
              for (int ti = 0; ti < wellTops.size(); ++ti)
              {
                WellComposite::FormationInterval fi;
                fi.name = wellTops.at(ti).topName;
                fi.topDepth = static_cast<float>(wellTops.at(ti).md);
                fi.bottomDepth = static_cast<float>((ti + 1 < wellTops.size()) ? wellTops.at(ti + 1).md : (wellTops.at(ti).md + 50.0));
                fi.color = kFormColors.at(ti % kFormColors.size());
                formationIntervals.append(fi);
              }
            }
          }
          break;
        }
      }
    }

    const QString wellTitle = wells.isEmpty() ? asset.displayName : wells.front().second;

    // 两段式期间如实占位：数据行池内解析中（DESIGN.md 诚实状态；秒级内
    // 换装真实曲线，无骨架闪空）。
    auto *lasPendingHint = new QLabel(tr("正在后台解析数据行…"), host);
    lasPendingHint->setObjectName(QStringLiteral("lasPendingHint"));
    PaleoTheme::applyThemedStyleSheet(lasPendingHint, [] {
      return PaleoTheme::mutedCaptionStyleSheet();
    });
    const QPointer<QLabel> hintFill(lasPendingHint);

    // 已决 well_log：综合图走井曲线并集。这里只读 ~C 头；兄弟文件数据体
    // 放进下面同一次 requestLas，不在 GUI 线程 parse。
    QString wellLogEntityId;
    for (const EntityAssetLink &l : links)
    {
      if (!l.unresolved && l.role == QLatin1String("well_log") && !l.entityId.isEmpty())
      {
        wellLogEntityId = l.entityId;
        break;
      }
    }
    const bool compositeFromWell = !wellLogEntityId.isEmpty();
    QVector<WellCurveRef> wellCurves;
    QStringList siblingPaths;
    if (compositeFromWell)
    {
      wellCurves = WellLogSet::wellCurveIndex(cat, catalogProjectDir(cat), wellLogEntityId);
      const QString currentAbs = QFileInfo(abs).absoluteFilePath();
      QSet<QString> seen;
      for (const WellCurveRef &ref : wellCurves)
      {
        const QString p = QFileInfo(ref.path).absoluteFilePath();
        if (p.isEmpty() || p == currentAbs || seen.contains(p))
          continue;
        seen.insert(p);
        siblingPaths.append(ref.path);
      }
    }

    // ---- F1 两段式第二段挂起：数据到达后一次装齐两个消费方 ----
    // 单道检视仍只用当前文件。无已决 well_log 时综合图与旧路径一致；
    // 有已决链接时综合图按 wellCurveIndex，每条曲线用自己文件的深度列。
    const QPointer<CurvePanel> panelFill(panel);
    const QPointer<WellComposite::WellCompositePanel> compFill(compPanel);
    const std::function<void(const QList<LasCurve> &, const QHash<QString, LasDoc> &)> fillCurves =
        [panelFill, compFill, chipMapFill = chipMap, hintFill, names, defaultShown,
         wellTitle, formationIntervals, wellCurves, compositeFromWell, abs](
            const QList<LasCurve> &curves, const QHash<QString, LasDoc> &siblings) {
          if (hintFill)
            hintFill->hide(); // 数据到齐，占位提示退场
          if (curves.size() != names.size())
            return; // 头/整份契约：lasHeaderAt 与 lasAt 曲线名逐项一致——不符不装
          if (panelFill)
          {
            for (int i = 1; i < names.size(); ++i)
            {
              const QString &cname = names.at(i);
              panelFill->addCurve(cname, curves.at(i).unit, curves.at(i).values,
                                  curves.at(0).values, pickCurveColor(cname, i - 1),
                                  defaultShown.contains(cname));
              if (auto *chip = chipMapFill->value(cname))
                chip->setToolTip(QStringLiteral("%1 (%2)").arg(cname, curves.at(i).unit));
            }
          }
          if (compFill)
          {
            QVector<WellComposite::CurveData> compCurves;
            if (!compositeFromWell)
            {
              const auto &depList = curves.at(0).values;
              QVector<float> depVec;
              depVec.reserve(depList.size());
              for (double d : depList)
                depVec.append(static_cast<float>(d));

              for (int i = 1; i < names.size(); ++i)
              {
                const auto &src = curves.at(i);
                WellComposite::CurveData cd;
                cd.name = names.at(i);
                cd.unit = src.unit;
                cd.color = pickCurveColor(cd.name, i - 1);
                cd.depths = depVec;
                cd.values.reserve(src.values.size());
                float valMin = 1e9f, valMax = -1e9f;
                for (double v : src.values)
                {
                  if (v <= -999.0 || v >= 99999.0)
                  {
                    cd.values.append(-9999.0f);
                    continue;
                  }
                  float fv = static_cast<float>(v);
                  cd.values.append(fv);
                  if (fv < valMin) valMin = fv;
                  if (fv > valMax) valMax = fv;
                }
                if (valMin < valMax)
                {
                  cd.minScale = valMin;
                  cd.maxScale = valMax;
                }
                else
                {
                  cd.minScale = 0.0f;
                  cd.maxScale = 100.0f;
                }
                compCurves.append(cd);
              }
            }
            else
            {
              int colorIndex = 0;
              for (const WellCurveRef &ref : wellCurves)
              {
                const QList<LasCurve> *body = lasBodyFor(ref.path, abs, curves, siblings);
                if (!body || body->isEmpty() || ref.column <= 0 || ref.column >= body->size())
                  continue; // 兄弟文件解析失败：跳过，不让整页失败
                const LasCurve &src = body->at(ref.column);
                compCurves.append(compositeCurve(ref.mnemonic, src.unit, body->at(0).values,
                                                 src.values, colorIndex));
                ++colorIndex;
              }
            }
            compFill->loadLasCurves(wellTitle, compCurves, formationIntervals);
          }
        };

    // 视图模式切换条与堆叠容器：选中 = chip 语义（primary 描边 + 浮起面底，
    // 同 ribbonStyleSheet checked 范式）；样式挂切换条一份，:checked 自动生效。
    auto *viewSwitchBar = new QWidget(host);
    auto *switchLay = new QHBoxLayout(viewSwitchBar);
    switchLay->setContentsMargins(0, 0, 0, 0);
    switchLay->setSpacing(8);
    PaleoTheme::applyThemedStyleSheet(viewSwitchBar, [] {
      const PaleoTheme::ThemeTokens &t = PaleoTheme::tokens();
      return QStringLiteral(
          "QToolButton { background: %1; border: 1px solid %2; border-radius: 4px;"
          " padding: 3px 10px; font-size: 8pt; color: %3; }"
          "QToolButton:hover { background: %4; }"
          "QToolButton:checked { background: %4; border-color: %5; color: %6;"
          " font-weight: 600; }")
          .arg(qssHex(t.surface), qssHex(t.border), qssHex(t.text),
               qssHex(t.surfaceAltRaised), qssHex(t.primary), qssHex(t.primaryText));
    });

    auto *btnResForm = new QToolButton(viewSwitchBar);
    btnResForm->setObjectName(QStringLiteral("btnResFormView"));
    btnResForm->setText(tr("ResFormStar 综合多井道柱状图 (推荐)"));
    btnResForm->setCheckable(true);
    btnResForm->setChecked(true);

    auto *btnSingle = new QToolButton(viewSwitchBar);
    btnSingle->setObjectName(QStringLiteral("btnSingleView"));
    btnSingle->setText(tr("单道叠合检视"));
    btnSingle->setCheckable(true);
    btnSingle->setChecked(false);

    auto *viewStack = new QStackedWidget(host);
    viewStack->setObjectName(QStringLiteral("logViewStack"));
    viewStack->addWidget(compPanel);   // 0: ResForm 多井道柱状图（默认）
    viewStack->addWidget(singlePage);  // 1: 单道快速检视

    connect(btnResForm, &QToolButton::clicked, host, [btnResForm, btnSingle, viewStack] {
      btnResForm->setChecked(true);
      btnSingle->setChecked(false);
      viewStack->setCurrentIndex(0);
    });

    connect(btnSingle, &QToolButton::clicked, host, [btnResForm, btnSingle, viewStack] {
      btnSingle->setChecked(true);
      btnResForm->setChecked(false);
      viewStack->setCurrentIndex(1);
    });

    switchLay->addWidget(caption8(tr("呈现模式:"), viewSwitchBar));
    switchLay->addWidget(btnResForm);
    switchLay->addWidget(btnSingle);
    switchLay->addStretch(1);

    lay->addWidget(new PaleoToolRow(viewSwitchBar, host));
    lay->addWidget(lasPendingHint);
    lay->addWidget(viewStack, 1);

    // F1 二段挂起注册 + 数据请求（页根 = viewStack：整份解析失败时栈内容
    // 换成「读取失败」面，重试=重建标签再走一遍两段式）。无任务服务时
    // requestLas 同步执行、返回前信号已发——测试环境行为与旧路径一致。
    m_pendingLas.insert(assetId, {viewStack, fillCurves});
    m_doc->requestLas(assetId, abs, siblingPaths);
    return host;
  }

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
    detailLayout->setSpacing(4);
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
                }
              });
      verLay->addWidget(verCombo);
      verLay->addStretch(1);
      detailLayout->addWidget(verBar);
    }

    const bool chosenIsDerived = chosen.stage == QLatin1String("DERIVED");
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
    const QString bigHint = PreviewRasterAnalysis::bigRasterHint(raster.get());
    if (!bigHint.isEmpty())
    {
      lay->addWidget(PreviewMapStates::buildBigRasterHintBar(bigHint, host));
      // B3（wave/deepen-perf）：消费侧预热——quiet 任务后台建瓦片金字塔 +
      // GDAL 外部 .ovr 概览；完成后重载层 + 刷新画布，本会话后续渲染走概览。
      if (m_doc)
      {
        QPointer<QgsRasterLayer> rasterGuard(raster.get());
        connect(m_doc, &PreviewDocService::rasterPyramidFinished, host,
                [this, rasterGuard, assetId](const QString &doneId, bool ok) {
                  if (doneId != assetId || !ok || !rasterGuard)
                    return;
                  rasterGuard->reload(); // 重开数据源——让 provider 发现 .ovr
                  rasterGuard->triggerRepaint();
                });
        m_doc->ensureRasterPyramidVersion(assetId);
      }
    }

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
      histLay->setContentsMargins(4, 4, 4, 4);
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
    {
      const auto siblings = PreviewMapStates::siblingMappableAssets(
          cat, tifPath, [this](const CatalogVersion &v) { return m_doc->absolutePathForVersion(v); });
      // 排除自身。
      QVector<QPair<QString, QString>> others;
      for (const auto &sib : siblings)
        if (sib.first != assetId)
          others.append(sib);
      if (!others.isEmpty())
      {
        auto *overlayBtn = new QToolButton(page);
        overlayBtn->setObjectName(QStringLiteral("siblingOverlayButton"));
        overlayBtn->setText(tr("同目录叠加"));
        overlayBtn->setToolTip(tr("把同目录下的相图/配准图片/层位栅格叠加到本预览"));
        overlayBtn->setPopupMode(QToolButton::InstantPopup);
        auto *menu = new QMenu(overlayBtn);
        for (const auto &sib : others)
        {
          const QString sibAssetId = sib.first;
          const QString sibName = sib.second;
          QAction *act = menu->addAction(sibName);
          QObject::connect(act, &QAction::triggered, host, [this, page, sibAssetId, sibName, host]() {
            addSiblingOverlayLayer(page, sibAssetId, sibName, host);
          });
        }
        overlayBtn->setMenu(menu);
        page->addToolBarWidget(overlayBtn);
      }
    }

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

    return buildSeismicContent(cat, asset, v, abs, assetId, links, host, lay);

  if (asset.type == QLatin1String("image_reference"))
  {
    // D2.7：有 world file/配准边车 → 栅格上图；未配准 → 图片查看器 + 引导。
    // 托管副本身旁没有边车、源目录有 → 成对搬进临时目录再上图（GDAL 只认
    // 数据文件旁的边车）。
    const auto georefPair =
        PreviewMapStates::stageGeorefPairIfNeeded(abs, v.sourceUri, host);
    const QString worldFile = georefPair.second;
    if (!worldFile.isEmpty())
    {
      auto raster = std::make_unique<QgsRasterLayer>(georefPair.first, asset.displayName,
                                                     QStringLiteral("gdal"));
      if (raster->isValid() && !raster->extent().isEmpty())
      {
        auto *page = new PreviewMapPage(host);
        page->setObjectName(QStringLiteral("imagePreviewPage"));
        page->setAssetKey(assetId);
        page->setRenderCacheIdentity(assetId, v.id);
        page->mapCanvas()->canvas()->setObjectName(QStringLiteral("imageMapCanvas"));
        page->decorations()->setObjectName(QStringLiteral("imageDecorManager"));
        page->setProfileEnabled(false); // 影像非连续值面——剖面采样无意义
        QgsRasterLayer *rasterRaw = raster.release();
        rasterRaw->setParent(host);
        page->addMapLayer(rasterRaw, asset.displayName, abs);
        // D2.11 大图（>50MB 无金字塔）提示：降级仍可用。
        const QString bigHint = PreviewRasterAnalysis::bigRasterHint(rasterRaw);
        if (!bigHint.isEmpty())
        {
          lay->addWidget(PreviewMapStates::buildBigRasterHintBar(bigHint, host));
          // B3：消费侧预热（同层位栅格页口径——.ovr 完成后重载层刷新）。
          if (m_doc)
          {
            QPointer<QgsRasterLayer> rasterGuard(rasterRaw);
            connect(m_doc, &PreviewDocService::rasterPyramidFinished, host,
                    [rasterGuard, assetId](const QString &doneId, bool ok) {
                      if (doneId != assetId || !ok || !rasterGuard)
                        return;
                      rasterGuard->reload();
                      rasterGuard->triggerRepaint();
                    });
            m_doc->ensureRasterPyramidVersion(assetId);
          }
        }
        lay->addWidget(page, 1);
        lay->addWidget(caption8(tr("已按配准边车 %1 上图（RGB 影像原色）")
                                    .arg(QFileInfo(worldFile).fileName()),
                                host));
        QTimer::singleShot(0, host, [page, rasterRaw]() {
          page->mapCanvas()->zoomToLayer(rasterRaw);
          page->primeRenderCache();
          if (!page->mapCanvas()->overlayVisible())
            page->showLowResSnapshot();
        });
        return host;
      }
    }
    // 未配准 → 图片查看器（原行为）+ 「去配准」引导入口（D2.7）。
    auto *scroll = new QScrollArea(host);
    scroll->setWidgetResizable(true);
    auto *imgLabel = new QLabel(scroll);
    QPixmap pm(abs);
    if (pm.isNull())
    {
      lay->addWidget(failureState(assetId, tr("无法解析图片"), host), 1);
      return host;
    }
    imgLabel->setPixmap(pm.scaledToWidth(560, Qt::SmoothTransformation)); // 按面板宽缩放
    scroll->setWidget(imgLabel);
    lay->addWidget(scroll, 1);
    auto *regGuideBtn = new QPushButton(tr("去配准…"), host);
    regGuideBtn->setObjectName(QStringLiteral("goRegisterGuideBtn"));
    regGuideBtn->setToolTip(tr("把这张平面相图配准到工程测网"));
    connect(regGuideBtn, &QPushButton::clicked, host, [this, host]() {
      QMessageBox::information(
          host, tr("去配准"),
          tr("配准两条路：\n"
             "· 在图片旁放同名 world file（.wld/.pgw/.jgw，六参数文本）——"
             "重新打开预览即按栅格上图；\n"
             "· 或把相图界线转为 GeoJSON，用 D11「临时配准（手工仿射）」"
             "登记为 DERIVED 版本。"));
    });
    lay->addWidget(regGuideBtn, 0, Qt::AlignLeft);
    lay->addWidget(warnLabel(tr("未配准，不加入地图"), host)); // §4
    return host;
  }

  if (asset.type == QLatin1String("document"))
  {
    // 文件名/类型等属性信息由右侧属性面板承担，预览页不再重复占空间；
    // 「用系统程序打开」只在无内嵌预览（转换失败/无门面）或需要打开
    // office 原件时作兜底出口。
    QString pdfAbs;
    if (asset.format == QLatin1String("pdf"))
      pdfAbs = abs;
    else if (m_doc)
    {
      m_doc->ensureDocumentPdf(assetId);
      switch (m_doc->documentPdfState(assetId))
      {
        case PreviewDocService::DocPdfState::Ready:
          pdfAbs = m_doc->documentPdfPath(assetId);
          break;
        case PreviewDocService::DocPdfState::Failed:
          lay->addWidget(
              stateLabel(tr("无 PDF 预览：%1").arg(m_doc->documentPdfError(assetId)),
                         host),
              1);
          lay->addWidget(makeOpenExternalRow(abs, host));
          break;
        default: // Pending（None 不可达——ensure 刚入队或已记失败）
          lay->addWidget(stateLabel(tr("正在转换为 PDF 预览…"), host), 1);
          break;
      }
    }
    else
    {
      lay->addWidget(stateLabel(tr("无法生成 PDF 预览"), host), 1);
      lay->addWidget(makeOpenExternalRow(abs, host));
    }

    if (!pdfAbs.isEmpty())
    {
      auto *doc = new QPdfDocument(host);
      if (doc->load(pdfAbs) == QPdfDocument::Error::None)
      {
        auto *view = new QPdfView(host);
        view->setObjectName(QStringLiteral("pdfView"));
        view->setDocument(doc);
        view->setPageMode(QPdfView::PageMode::MultiPage);
        lay->addWidget(view, 1);
        if (asset.format != QLatin1String("pdf"))
        {
          lay->addWidget(
              caption8(tr("预览为 PDF 转换件；原件经「用系统程序打开」"), host));
          lay->addWidget(makeOpenExternalRow(abs, host), 0, Qt::AlignLeft);
        }
      }
      else
      {
        lay->addWidget(
            stateLabel(tr("PDF 转换件无法加载\n%1").arg(pdfAbs), host), 1);
        lay->addWidget(makeOpenExternalRow(abs, host));
      }
    }
    lay->addWidget(warnLabel(tr("未配准，不加入地图"), host)); // §4
    return host;
  }

  if (asset.type == QLatin1String("geojson") ||
      (asset.type == QLatin1String("boundary") && asset.displayName.endsWith(QLatin1String(".geojson"), Qt::CaseInsensitive)))
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
    topLay->setContentsMargins(8, 4, 8, 4);
    topLay->setSpacing(8);
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

    topLay->addSpacing(6);

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
        return QStringLiteral(
            "QComboBox { background: %1; border: 1px solid %2; border-radius: 4px;"
            " padding: 2px 6px; font-size: 8pt; color: %3; }"
            "QComboBox:hover { border-color: %4; }")
            .arg(qssHex(t.surface), qssHex(t.border), qssHex(t.text),
                 qssHex(t.textDisabled));
      });
      topLay->addWidget(fieldCombo);
    }

    topLay->addSpacing(8);

    // D11 临时配准入口
    auto *regBtn = new QPushButton(tr("临时配准（手工仿射）…"), host);
    regBtn->setObjectName(QStringLiteral("provisionalRegisterButton"));
    regBtn->setAccessibleName(tr("临时配准"));
    regBtn->setToolTip(tr("手工输入仿射参数，把 GeoJSON 变换到工程局部测网"));
    PaleoTheme::applyThemedStyleSheet(regBtn, [] {
      const PaleoTheme::ThemeTokens &t = PaleoTheme::tokens();
      return QStringLiteral(
          "QPushButton { background: %1; border: 1px solid %2; border-radius: 4px;"
          " padding: 4px 8px; font-size: 8pt; color: %3; }"
          "QPushButton:hover { background: %4; border-color: %5; }")
          .arg(qssHex(t.surface), qssHex(t.border), qssHex(t.text),
               qssHex(t.surfaceAltRaised), qssHex(t.textDisabled));
    });
    topLay->addWidget(regBtn);

    topLay->addStretch(1);

    // 经纬度与工程测网不同空间：阻断级提示用 error token（原 #D32F2F + 11px）。
    auto *warnLbl = new QLabel(tr("经纬度，与本测网不是同一空间"), host);
    warnLbl->setWordWrap(true);
    PaleoTheme::applyThemedStyleSheet(warnLbl, [] {
      return QStringLiteral("color: %1; font-size: 8pt; font-weight: 500;")
          .arg(qssHex(PaleoTheme::tokens().error));
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
      {
        const auto siblings = PreviewMapStates::siblingMappableAssets(
            cat, abs, [this](const CatalogVersion &v) { return m_doc->absolutePathForVersion(v); });
        QVector<QPair<QString, QString>> others;
        for (const auto &sib : siblings)
          if (sib.first != assetId)
            others.append(sib);
        if (!others.isEmpty())
        {
          auto *overlayBtn = new QToolButton(page);
          overlayBtn->setObjectName(QStringLiteral("siblingOverlayButton"));
          overlayBtn->setText(tr("同目录叠加"));
          overlayBtn->setToolTip(tr("把同目录下的相图/配准图片/层位栅格叠加到本预览"));
          overlayBtn->setPopupMode(QToolButton::InstantPopup);
          auto *menu = new QMenu(overlayBtn);
          for (const auto &sib : others)
          {
            QAction *act = menu->addAction(sib.second);
            QObject::connect(act, &QAction::triggered, host, [this, page, sib, host]() {
              addSiblingOverlayLayer(page, sib.first, sib.second, host);
            });
          }
          overlayBtn->setMenu(menu);
          page->addToolBarWidget(overlayBtn);
        }
      }

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
      return QStringLiteral(
          "QTableWidget { background-color: %1; gridline-color: %2; border: 1px solid %2;"
          " font-size: 9pt; }"
          "QHeaderView::section { background-color: %3; color: %4; border: none;"
          " border-bottom: 1px solid %2; border-right: 1px solid %2; padding: 4px 8px;"
          " font-weight: 500; font-size: 8pt; }")
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
    tablePageLay->setSpacing(4);
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
      // F2 两段式（goal/perf-systematize 簇2）：XML 解析进任务池（综合图可
      // 含数 MB 曲线数据，同步解析阻塞 UI 线程），面板骨架 + 「解析中」提示
      // 即时上屏；无任务服务时 loadComprehensiveXmlAsync 同步执行——失败
      // 返回 false 走原有不支持预览 fall-through（测试环境行为不变）。
      auto *compositePanel = new WellComposite::WellCompositePanel(host);
      compositePanel->setObjectName(QStringLiteral("wellCompositePanel"));
      auto *xmlPendingHint = new QLabel(tr("正在后台解析综合柱状图…"), host);
      xmlPendingHint->setObjectName(QStringLiteral("xmlPendingHint"));
      PaleoTheme::applyThemedStyleSheet(xmlPendingHint, [] {
        return PaleoTheme::mutedCaptionStyleSheet();
      });
      const QPointer<QLabel> xmlHintG(xmlPendingHint);
      const QPointer<WellComposite::WellCompositePanel> compG(compositePanel);
      connect(compositePanel, &WellComposite::WellCompositePanel::comprehensiveXmlLoaded,
              host, [this, assetId, host, lay, xmlHintG, compG](bool ok) {
                if (xmlHintG)
                  xmlHintG->hide();
                // 异步失败（截断/坏 XML）：换成「读取失败」面（重试=重建标签）。
                if (!ok && compG)
                {
                  lay->removeWidget(compG);
                  compG->deleteLater();
                  lay->addWidget(failureState(assetId, tr("综合柱状图 XML 无法解析"), host), 1);
                }
              });
      lay->addWidget(xmlPendingHint);
      if (compositePanel->loadComprehensiveXmlAsync(
              abs, m_doc ? m_doc->taskService() : nullptr))
      {
        lay->addWidget(compositePanel, 1);
        return host;
      }
      delete compositePanel;
      delete xmlPendingHint;
    }
    // P2 D2.12 未知类型：统一「不支持预览」态 + 可支持类型清单（不再留白）。
    static const QStringList kKnownTypes = {
        QStringLiteral("well_log"),      QStringLiteral("well_head"),
        QStringLiteral("well_stratification"), QStringLiteral("time_depth"),
        QStringLiteral("horizon"),       QStringLiteral("seismic"),
        QStringLiteral("image_reference"), QStringLiteral("document"),
        QStringLiteral("geojson"),       QStringLiteral("boundary")};
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
    hl->setSpacing(8);
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
    hl->setSpacing(8);
    auto *info = new QWidget(holder);
    auto *grid = new QVBoxLayout(info);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(4);
    const auto addRow = [&](const QString &k, const QString &val, bool mono = false,
                            bool muted = false) {
      auto *row = new QWidget(info);
      auto *rl = new QHBoxLayout(row);
      rl->setContentsMargins(0, 0, 0, 0);
      rl->addWidget(caption8(k, row));
      auto *v = valueLabel(val, row, mono);
      if (muted)
        PaleoTheme::applyThemedStyleSheet(
            v, [] { return PaleoTheme::mutedCaptionStyleSheet(); }); // text-muted
      rl->addWidget(v, 1);
      grid->addWidget(row);
    };
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
    if (rec)
    {
      // §4：井名、X、Y、KB、TD、BottomX、BottomY、WellType、coordinate_status；
      // 数字 JetBrains Mono 9pt 右对齐。
      addRow(tr("井名"), rec->name);
      addRow(tr("X"), QString::number(rec->x, 'f', 2), true);
      addRow(tr("Y"), QString::number(rec->y, 'f', 2), true);
      addRow(tr("KB"), QString::number(rec->kb, 'f', 2), true);
      addRow(tr("TD"), QString::number(rec->td, 'f', 2), true);
      addRow(tr("BottomX"),
             rec->hasBottomX ? QString::number(rec->bottomX, 'f', 2) : QString(), true);
      addRow(tr("BottomY"),
             rec->hasBottomY ? QString::number(rec->bottomY, 'f', 2) : QString(), true);
      addRow(tr("WellType"), rec->wellType);
    }
    const QString status =
        wellEntityId.isEmpty()
            ? QString()
            : m_doc->catalog()->entityById(wellEntityId).coordinateStatus;
    // T27：坐标状态行中文化 + text-muted（计划 §4：这些状态仍用 #5D6E80）。
    addRow(tr("坐标状态"), coordinateStatusText(status), false, true);
    hl->addWidget(info);
    hl->addWidget(caption8(tr("选中时地图同时高亮该井"), holder));

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
    mapPage->addMapLayer(wellsVl, tr("井位"), absPath);
    auto *labelRow = new QWidget(holder);
    auto *labelLay = new QHBoxLayout(labelRow);
    labelLay->setContentsMargins(0, 0, 0, 0);
    auto *labelToggle = new QCheckBox(tr("名称标注"), labelRow);
    labelToggle->setObjectName(QStringLiteral("wellLabelToggle"));
    labelToggle->setChecked(true);
    labelLay->addWidget(labelToggle);
    labelLay->addStretch(1);
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
  sp->setTraces(doc.traces, doc.sampleIntervalUs, doc.startTimeMs);
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
    pend.mode->setToolTip(QString());
  }
  if (pend.spin)
  {
    pend.spin->setEnabled(true);
    pend.spin->setToolTip(QString());
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
