// 层：视图
// 综合图面板·数据装配（XML 同步/异步、LAS 曲线、综合数据→道布局）——自 wellcompositepanel.cpp 拆出（方向 66，行为零变更）
#include "wellcompositepanel.h"
#include "../../domain/wellcompositemodel.h" // ComprehensiveWellData（方向 59：previewdoc.h 瘦身后出参类型直取）
#include "../../services/previewdoc.h"
#include "../../services/paleotaskservice.h"
#include "editsession.h"
#include "trackops.h"
#include "wellpositionlegendwidget.h"
#include <QFile>
#include <QFileInfo>
#include <QToolButton>

namespace WellComposite
{
// ----------------------------------------------------------------------------
// 数据装配
// ----------------------------------------------------------------------------
bool WellCompositePanel::loadComprehensiveXml(const QString &xmlPath)
{
  ++m_xmlLoadSeq; // 同步换源同样使在途异步结果作废
  ComprehensiveWellData data;
  QString err;
  if (!PreviewDocService::wellCompositeAt(xmlPath, &data, &err))
    return false; // 同步失败语义保持：返回值即终态，不发信号（调用方直取）
  applyComprehensiveData(data, xmlPath);
  return true;
}

void WellCompositePanel::applyComprehensiveData(const ComprehensiveWellData &data,
                                                const QString &xmlPath, bool reference)
{
  m_data = data;
  setWellName(data.wellName, reference);
  setupTracksFromData(data);
  if (m_legendWidget)
    m_legendWidget->setWellData(data);
  clearDepthTables(); // D1：换源复位井斜/时深（wellLoaded 后 sink 按新源重喂）

  // D3.x 编辑会话：工作副本 + 源 mtime 跟踪（D3.10）
  m_editSession = std::make_unique<EditSession>(data, this);
  connect(m_editSession.get(), &EditSession::documentChanged, this, [this]() {
    syncSessionToTracks();
  });
  setSourceDataPath(xmlPath);

  // D2.x sidecar + D1.8 会话记忆
  loadSidecar();
  restoreSessionState();
  updateDefaultLayoutButton();
  emit wellLoaded(data.wellName);
  emit faciesDataChanged(m_data);
  emit comprehensiveXmlLoaded(true);
}

bool WellCompositePanel::loadWellData(const ComprehensiveWellData &data, const QString &sourcePath, bool reference)
{
  ++m_xmlLoadSeq;
  if (data.isEmpty()) return false;
  applyComprehensiveData(data, sourcePath, reference);
  return true;
}

bool WellCompositePanel::loadComprehensiveXmlAsync(const QString &xmlPath,
                                                   PaleoTaskService *svc)
{
  // 快速失败留在同步侧（与旧路径一致：文件不存在 false，不拉任务）。
  if (xmlPath.isEmpty() || !QFile::exists(xmlPath))
    return false;
  if (!svc)
    return loadComprehensiveXml(xmlPath); // 无任务服务：同步旧路径（测试）

  const int seq = ++m_xmlLoadSeq;
  // ComprehensiveWellData 值语义（QVector 底）——池线程产出、GUI 线程装配，
  // shared_ptr 交接（与 WellCorrelationPanel::submitLasLoad 同一纪律）。
  auto out = std::make_shared<ComprehensiveWellData>();
  auto *task = svc->start(
      tr("解析综合柱状图 %1").arg(QFileInfo(xmlPath).fileName()),
      [xmlPath, out](PaleoTask *) -> QString {
        QString err;
        if (!PreviewDocService::wellCompositeAt(xmlPath, out.get(), &err))
          return err.isEmpty() ? QObject::tr("无法解析综合柱状图 XML") : err;
        return QString();
      },
      QString(), /*quiet=*/true); // 交互内嵌取数——不拉起任务中心
  connect(task, &PaleoTask::finished, this,
          [this, seq, out, xmlPath, task]() {
            if (seq != m_xmlLoadSeq)
              return; // 陈旧结果丢弃：换源/重入已接管
            if (task->state() == PaleoTask::State::Succeeded)
              applyComprehensiveData(*out, xmlPath);
            else
              emit comprehensiveXmlLoaded(false);
          });
  return true;
}

bool WellCompositePanel::loadLasCurves(const QString &wellName, const QVector<CurveData> &curves,
                                       const QVector<FormationInterval> &formations)
{
  ++m_xmlLoadSeq;
  m_data = {};
  m_editSession.reset();
  clearFaciesPrediction();
  m_canvas->clearTracks();
  setWellName(wellName);
  clearDepthTables(); // D1：换井复位（LAS 路径无井斜/时深表，保持禁用态）

  if (curves.isEmpty() && formations.isEmpty())
  {
    emit faciesDataChanged(m_data);
    return false;
  }

  // 计算深度跨度
  double minD = 1e9, maxD = -1e9;
  for (const auto &c : curves)
  {
    if (!c.depths.isEmpty())
    {
      if (c.depths.first() < minD) minD = c.depths.first();
      if (c.depths.last() > maxD) maxD = c.depths.last();
    }
  }
  for (const auto &f : formations)
  {
    if (f.topDepth < minD) minD = f.topDepth;
    if (f.bottomDepth > maxD) maxD = f.bottomDepth;
  }
  if (minD >= maxD)
  {
    minD = 0.0;
    maxD = 1000.0;
  }

  m_canvas->setDepthRange(minD, maxD);

  // LAS 与综合 XML 共用默认测井查看模板；全部曲线仍可从隐藏道恢复。
  // 沉积相道：无真实相数据时不展示（不臆造相序）。
  m_data.wellName = wellName;
  m_data.minDepth = minD;
  m_data.maxDepth = maxD;
  m_data.continuousCurves = curves;
  m_data.formationIntervals = formations;
  m_canvas->setTracks(TrackOps::defaultLayoutTracks(m_data, m_scaleCombo->currentText()));
  if (m_legendWidget)
    m_legendWidget->setWellData(m_data);

  // LAS 路径无 sidecar（曲线为主）；编辑会话仍可建（层位编辑）
  m_editSession = std::make_unique<EditSession>(m_data, this);
  connect(m_editSession.get(), &EditSession::documentChanged, this, [this]() {
    syncSessionToTracks();
  });

  restoreSessionState();
  m_canvas->setScaleRatio(m_scaleCombo->currentText());
  updateDefaultLayoutButton();
  emit faciesDataChanged(m_data);
  return true;
}

void WellCompositePanel::setLithologyIntervals(const QVector<LithologyInterval> &items)
{
  if (items.isEmpty() || !m_canvas)
    return;
  m_data.lithologyIntervals = items;
  double lo = m_canvas->minDepth();
  double hi = m_canvas->maxDepth();
  for (const LithologyInterval &interval : items)
  {
    lo = qMin(lo, double(interval.topDepth));
    hi = qMax(hi, double(interval.bottomDepth));
  }
  m_canvas->setDepthRange(lo, hi);
  m_data.minDepth = lo;
  m_data.maxDepth = hi;
  for (const auto &existing : m_canvas->tracks())
    if (existing && existing->type() == TrackType::Lithology)
    {
      std::static_pointer_cast<LithologyTrack>(existing)->setIntervals(items);
      if (m_legendWidget)
        m_legendWidget->setWellData(m_data);
      m_canvas->updateAll();
      updateDefaultLayoutButton();
      emit faciesDataChanged(m_data);
      return;
    }
  auto track = std::make_shared<LithologyTrack>(tr("岩性"), 90.0);
  track->setIntervals(items);
  int insertAt = m_canvas->tracks().size();
  const auto tracks = m_canvas->tracks();
  for (int i = 0; i < tracks.size(); ++i)
  {
    if (tracks.at(i)->type() == TrackType::Curve)
    {
      insertAt = i;
      break;
    }
  }
  m_canvas->insertTrack(insertAt, track);
  if (m_legendWidget)
    m_legendWidget->setWellData(m_data);
  updateDefaultLayoutButton();
  emit faciesDataChanged(m_data);
}

void WellCompositePanel::setCoreImages(const QVector<ImageDepthItem> &items)
{
  if (items.isEmpty())
    return;
  m_data.images = items;
  double lo = m_canvas->minDepth();
  double hi = m_canvas->maxDepth();
  for (const ImageDepthItem &im : items)
  {
    lo = qMin(lo, double(im.topDepth));
    hi = qMax(hi, double(im.bottomDepth));
  }
  m_canvas->setDepthRange(lo, hi);
  m_data.minDepth = lo;
  m_data.maxDepth = hi;
  auto track = std::make_shared<ImageTrack>(tr("岩心照片"), 110.0);
  track->setItems(items);
  m_canvas->addTrack(track);
  if (m_legendWidget)
    m_legendWidget->setWellData(m_data);
  updateDefaultLayoutButton();
}

void WellCompositePanel::setupTracksFromData(const ComprehensiveWellData &data)
{
  clearFaciesPrediction();
  m_canvas->clearTracks();
  m_canvas->setDepthRange(data.minDepth, data.maxDepth);

  // D2.x 标志层线（渲染/吸附/读数/gap 数据面）
  m_canvas->setMarkerLines(data.standardHorizons);

  // 默认测井查看模板：深度 → 地层 → 岩性 → 测井曲线×3 → 沉积相 → 层序；其余数据道隐藏保留。
  m_canvas->setTracks(TrackOps::defaultLayoutTracks(data, m_scaleCombo->currentText()));

  m_canvas->setScaleRatio(m_scaleCombo->currentText());
}

} // namespace WellComposite
