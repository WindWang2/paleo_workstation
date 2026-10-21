// 层：组装根
// 工区井/轨迹/测区图层刷新簇 + 工程重开分拍恢复。从 appcontext.cpp 析出
//（goal/core-deepening A-R2 行为守恒拆分）：这些是被 projectOpened/catalog
// changed 尾部调用的叶子段，内部无跨段顺序耦合；projectOpened 巨 lambda 的
// 顺序敏感段（锁接管/journal/manifest/catalog 九连绑等）仍留在主 TU。
#include "appcontext.h"
#include "../io/dataimportservice.h"
#include "../linkage/wellmaplink.h"
#include "../metadata/projectlock.h"
#include "../qgis/qgiscanvascontroller.h"
#include "../qgis/qgislayerorganizer.h"
#include "../qgis/qgislayerservice.h"
#include "../qgis/qgisprojectservice.h"
#include "../qgis/qgisstyleservice.h"
#include "../qgis/wellattributestore.h"
#include "../workflow/projectlayerrefresh.h"
#include "../workflow/welltrajectorylayer.h"
#include <QDir>
#include <QTimer>
#include <qgsfillsymbol.h>
#include <qgsrectangle.h>
#include <qgssinglesymbolrenderer.h>
#include <qgsvectorlayer.h>

void AppContext::restoreProjectLayers(const QString &directory, quint64 session, bool wellsPrepared,
                                     bool trajectoriesPrepared, int phase)
{
  if (directory != m_projectDir || session != m_projectSvc->sessionId() ||
      m_projectSvc->isOpening() || m_projectSvc->projectPath().isEmpty() ||
      !m_import->catalog()->isOpen()) return;
  // QGIS 的图层接管/样式只在 GUI；各层分拍恢复，避免与目录面板及地震体
  // 就绪结果挤成一次长回调。切工程后定时回调按会话身份失效。
  if (phase == 0) m_layerSvc->refreshBasemaps();
  else if (phase == 1) refreshSurveyLayer();
  else if (phase == 2) refreshWellsLayer(false, wellsPrepared);
  else {
    refreshWellTrajectoriesLayer(trajectoriesPrepared);
    if (m_layerOrganizer) m_layerOrganizer->reorganize();
    return;
  }
  QTimer::singleShot(16, this, [this, directory, session, wellsPrepared, trajectoriesPrepared, phase] {
    restoreProjectLayers(directory, session, wellsPrepared, trajectoriesPrepared, phase + 1);
  });
}

void AppContext::refreshWellTrajectoriesLayer(bool dataPrepared)
{
  if (m_projectDir.isEmpty() || !m_import || !m_layerSvc || !m_projectData)
    return;
  DataCatalog *cat = m_import->catalog();
  if (!cat)
    return;
  const bool readOnly = m_projectLock && !m_projectLock->isHeld();

  const QString trajPath = QDir(m_projectDir).filePath(
      QStringLiteral("artifacts/layers/well_trajectories.geojson"));
  if (!readOnly && !dataPrepared) {
    m_projectLayerRefresh->request(cat, m_projectDir);
    return;
  }
  if (!QFile::exists(trajPath)) return;
  if (!readOnly && dataPrepared && QFile::exists(trajPath)) {
    LayerDeclaration decl;
    decl.layerId = QStringLiteral("well_trajectories");
    decl.type = QStringLiteral("vector"); decl.source = trajPath;
    decl.group = QStringLiteral("00_Data"); decl.title = tr("井轨迹投影");
    QString error;
    if (!m_layerSvc->declare(decl, &error)) qWarning() << error;
  }
  QString ierr;
  auto *layer = qobject_cast<QgsVectorLayer *>(
      m_layerSvc->instantiate(QStringLiteral("well_trajectories"), &ierr));
  if (!layer)
  {
    // 声明缺位（可写实例首跑前）或文件为空——不算失败，仅无层可上图。
    return;
  }
  layer->reload();
  QgisStyleService::applyTrajectoryLayerStyle(layer);
  layer->triggerRepaint();
}

void AppContext::refreshWellsLayer(bool zoomOnGrowth, bool dataPrepared)
{
  if (m_projectDir.isEmpty() || !m_import || !m_layerSvc)
    return;
  DataCatalog *cat = m_import->catalog();
  if (!cat)
    return;

  // 只读实例（T4）：不写 wells.geojson、不 upsert manifest——第一实例的
  // 产物已在位时直接按既有声明实例化；声明缺失（从未有写实例跑过）则
  // 如实无 wells 图层，静默降级。
  const bool readOnly = m_projectLock && !m_projectLock->isHeld();

  // catalog 井实体（有 surface 坐标者）→ artifacts/layers/wells.geojson。
  // 坐标是工程网格局部米（writeWellsGeoJson 写同一 WKT，不投 4326）。
  const QString wellsPath = QDir(m_projectDir).filePath(
      QStringLiteral("artifacts/layers/wells.geojson"));
  if (!readOnly)
  {
    if (!dataPrepared) {
      m_projectLayerRefresh->request(cat, m_projectDir);
      return;
    }
    if (!QFile::exists(wellsPath))
      return; // 没有可定位的井——不声明空图层（与 writeWellsGeoJson 同一约定）

    LayerDeclaration decl;
    decl.layerId = QStringLiteral("wells");
    decl.type = QStringLiteral("vector");
    decl.source = wellsPath;
    decl.group = QStringLiteral("00_Data");
    decl.title = tr("井位");
    QString derr;
    if (!m_layerSvc->declare(decl, &derr)) // upsert——重复声明安全
    {
      qWarning() << "AppContext: wells layer declare failed:" << derr;
      return;
    }
  }
  else if (!QFile::exists(wellsPath))
  {
    return; // 只读且无可复用产物——不造数据，也不算失败
  }
  QString ierr;
  auto *layer = qobject_cast<QgsVectorLayer *>(
      m_layerSvc->instantiate(QStringLiteral("wells"), &ierr));
  if (!layer)
  {
    qWarning() << "AppContext: wells layer instantiate failed:" << ierr;
    return;
  }
  layer->reload();         // 文件可能刚被重写——数据源重读要素
  QgisStyleService::applyWellLayerStyle(layer);
  // C3（wave/deepen-perf）：井头带类别字段时按 Q/HS 1011—2016 表 K.1 十二类
  // 探井分类渲染；无字段保持通用「探井」符号——不造假类别。
  for (const QString &field : {QStringLiteral("well_class"),
                               QStringLiteral("well_category"),
                               QStringLiteral("category"),
                               QStringLiteral("类别")})
  {
    if (layer->fields().lookupField(field) >= 0)
    {
      QgisStyleService::applyWellCategoryStyle(layer, field);
      break;
    }
  }
  layer->triggerRepaint();
  if (m_wellLink)
    m_wellLink->setWellLayer(layer, QStringLiteral("id"));

  // D6 zoom-to-content：井点范围实际变化（导入/补挂出新井）才把视野拉到
  // wells 层——无关 catalog 变更重写同一 geojson，extent 不变不打扰用户。
  const QgsRectangle ext = layer->extent();
  if (ext != m_lastWellsExtent)
  {
    m_lastWellsExtent = ext;
    if (zoomOnGrowth && !ext.isEmpty() && m_canvasCtl)
      m_canvasCtl->zoomToLayer(QStringLiteral("wells"));
  }
}

void AppContext::refreshSurveyLayer()
{
  if (m_projectDir.isEmpty() || !m_import || !m_import->catalog() || !m_layerSvc) return;
  // 无有效测区（角点 ≥3）不落测区图层——空工程的地图/图层树保持空态
  //（T31 契约）；测区被移除时把此前的占位层一并撤下。
  bool hasSurvey = false;
  for (const auto &area : m_import->catalog()->entities(QStringLiteral("study_area")))
    if (area.corners.size() >= 3) { hasSurvey = true; break; }
  for (const auto &survey : m_import->catalog()->entities(QStringLiteral("seismic_survey")))
    if (survey.corners.size() >= 3) { hasSurvey = true; break; }
  if (!hasSurvey) {
    QString error;
    m_layerSvc->removeDeclaration(QStringLiteral("survey.area"), &error);
    return;
  }
  const QString path = QDir(m_projectDir).filePath(QStringLiteral("artifacts/layers/survey_area.geojson"));
  QString error;
  if (!isProjectReadOnly()) {
    if (!m_import->catalog()->writeSurveyGeoJson(path, &error)) {
      qWarning() << "Survey layer:" << error; return;
    }
    LayerDeclaration declaration;
    declaration.layerId = QStringLiteral("survey.area");
    declaration.type = QStringLiteral("vector");
    declaration.source = path;
    declaration.group = QStringLiteral("00_Data");
    declaration.title = tr("测区范围");
    if (!m_layerSvc->declare(declaration, &error)) {
      qWarning() << "Survey declaration:" << error; return;
    }
  }
  if (auto *layer = qobject_cast<QgsVectorLayer *>(m_layerSvc->instantiate(QStringLiteral("survey.area"), &error))) {
    layer->reload();
    // 原生符号保持透明内部，矩形范围和底图同时可读。
    auto symbol = QgsFillSymbol::createSimple({{"color", "transparent"}, {"outline_color", "27,115,208,255"},
                                               {"outline_style", "dash"}, {"outline_width", "0.5"}});
    layer->setRenderer(new QgsSingleSymbolRenderer(symbol.release()));
    layer->triggerRepaint();
  }
}
