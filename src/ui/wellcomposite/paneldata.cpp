// 层：视图
// 综合图面板·数据装配（XML 同步/异步、LAS 曲线、综合数据→道布局）——自 wellcompositepanel.cpp 拆出（方向 66，行为零变更）
#include "wellcompositepanel.h"
#include "../../domain/wellcompositemodel.h" // ComprehensiveWellData（方向 59：previewdoc.h 瘦身后出参类型直取）
#include "../../services/previewdoc.h"
#include "../../services/paleotaskservice.h"
#include "editsession.h"
#include "wellpositionlegendwidget.h"
#include <QFile>
#include <QFileInfo>

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

  // 1. 地层系统组组合道 (系 | 统 | 组) —— 仅当分层名能映射出系/统时才展示，
  //    否则不摆一个大量留空的组合道（地层单位道已覆盖真实分层）。
  if (!formations.isEmpty())
  {
    auto stratTrack = std::make_shared<StratigraphyCompoundTrack>(QStringLiteral("地层"), 145.0);
    stratTrack->autoDeriveStratigraphy(formations, minD, maxD);
    // intervals() 按值返回：先绑定到局部再取迭代器（两次调用 = 两个不同临时对象）。
    const auto derived = stratTrack->intervals();
    const bool anySystem = std::any_of(derived.cbegin(), derived.cend(),
                                       [](const StratigraphyInterval &si) { return !si.system.isEmpty(); });
    if (anySystem)
      m_canvas->addTrack(stratTrack);
  }

  // 2. 深度标尺道 (DepthScaleTrack)
  auto scaleTrack = std::make_shared<DepthScaleTrack>(68.0);
  scaleTrack->setScaleRatio(m_scaleCombo->currentText());
  m_canvas->addTrack(scaleTrack);

  // 3. 地层道 (FormationTrack) —— 若有分层数据
  if (!formations.isEmpty())
  {
    auto formTrack = std::make_shared<FormationTrack>(QStringLiteral("地层"), 80.0);
    formTrack->setIntervals(formations);
    m_canvas->addTrack(formTrack);
  }

  // 4. 曲线道 —— 按助记名语义分道（H3：经 TrackSpec/注册表装配，配置对话框可改）
  const auto isLitho = [](const QString &name) {
    const QString n = name.toUpper();
    return n.startsWith(QStringLiteral("GR")) || n.startsWith(QStringLiteral("CAL")) ||
           n.startsWith(QStringLiteral("SP")) || n.startsWith(QStringLiteral("BS")) ||
           n.startsWith(QStringLiteral("AZIM"));
  };
  const auto isPorosity = [](const QString &name) {
    const QString n = name.toUpper();
    return n.startsWith(QStringLiteral("AC")) || n.startsWith(QStringLiteral("DEN")) ||
           n.startsWith(QStringLiteral("CNL")) || n.startsWith(QStringLiteral("POR")) ||
           n.startsWith(QStringLiteral("CPOR")) || n.startsWith(QStringLiteral("PHIF"));
  };

  QVector<CurveData> lithoCurves;
  QVector<CurveData> poroCurves;
  QVector<CurveData> resCurves;
  QVector<CurveData> otherCurves;

  for (const auto &c : curves)
  {
    if (isLitho(c.name)) lithoCurves.append(c);
    else if (isPorosity(c.name)) poroCurves.append(c);
    else if (c.name.toUpper().startsWith(QStringLiteral("RT"))) resCurves.append(c);
    else otherCurves.append(c);
  }

  const auto addTrackGroup = [this](const QString &baseTitle, const QVector<CurveData> &group) {
    for (int i = 0; i < group.size(); i += 4)
    {
      QString title = baseTitle;
      if (group.size() > 4)
        title += QStringLiteral(" (%1)").arg(i / 4 + 1);
      auto track = std::make_shared<CurveTrack>(title, 180.0);
      for (int j = 0; j < 4 && (i + j) < group.size(); ++j)
        track->addCurve(group.at(i + j));
      m_canvas->addTrack(track);
    }
  };

  if (!lithoCurves.isEmpty())
    addTrackGroup(QStringLiteral("岩性测井"), lithoCurves);
  if (!poroCurves.isEmpty())
    addTrackGroup(QStringLiteral("三孔隙测井"), poroCurves);
  if (!resCurves.isEmpty())
    addTrackGroup(QStringLiteral("电阻率测井"), resCurves);
  if (!otherCurves.isEmpty())
    addTrackGroup(QStringLiteral("辅助曲线"), otherCurves);

  if (lithoCurves.isEmpty() && poroCurves.isEmpty() && resCurves.isEmpty() && otherCurves.isEmpty())
  {
    for (int i = 0; i < curves.size(); i += 4)
    {
      auto track = std::make_shared<CurveTrack>(
          i == 0 ? QStringLiteral("常规测井") : QStringLiteral("辅助曲线"), 180.0);
      for (int j = 0; j < 4 && (i + j) < curves.size(); ++j)
        track->addCurve(curves.at(i + j));
      m_canvas->addTrack(track);
    }
  }

  // 沉积相道：无真实相数据时不展示（不臆造相序）。
  m_data.wellName = wellName;
  m_data.minDepth = minD;
  m_data.maxDepth = maxD;
  m_data.continuousCurves = curves;
  m_data.formationIntervals = formations;
  if (m_legendWidget)
    m_legendWidget->setWellData(m_data);

  // LAS 路径无 sidecar（曲线为主）；编辑会话仍可建（层位编辑）
  m_editSession = std::make_unique<EditSession>(m_data, this);
  connect(m_editSession.get(), &EditSession::documentChanged, this, [this]() {
    syncSessionToTracks();
  });

  const QList<TrackSpec> mem = WellCompositeStore::loadSessionTracks(m_projectName, m_wellName);
  restoreSessionState();
  m_canvas->setScaleRatio(m_scaleCombo->currentText());
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
  auto track = std::make_shared<LithologyTrack>(tr("岩性道"), 80.0);
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
}

void WellCompositePanel::setupTracksFromData(const ComprehensiveWellData &data)
{
  m_canvas->clearTracks();
  m_canvas->setDepthRange(data.minDepth, data.maxDepth);

  // D2.x 标志层线（渲染/吸附/读数/gap 数据面）
  m_canvas->setMarkerLines(data.standardHorizons);

  // 1. 地层系统组组合道：文档真实地层系统数据，或分层名能映射出系/统时展示。
  if (!data.stratigraphyIntervals.isEmpty() || !data.formationIntervals.isEmpty())
  {
    auto stratTrack = std::make_shared<StratigraphyCompoundTrack>(QStringLiteral("地层"), 145.0);
    if (!data.stratigraphyIntervals.isEmpty())
    {
      stratTrack->setIntervals(data.stratigraphyIntervals);
      m_canvas->addTrack(stratTrack);
    }
    else
    {
      stratTrack->autoDeriveStratigraphy(data.formationIntervals, data.minDepth, data.maxDepth);
      const auto derived = stratTrack->intervals(); // 按值返回，绑定局部后再迭代
      const bool anySystem = std::any_of(derived.cbegin(), derived.cend(),
                                         [](const StratigraphyInterval &si) { return !si.system.isEmpty(); });
      if (anySystem)
        m_canvas->addTrack(stratTrack);
    }
  }

  // 2. 地层单位道
  if (!data.formationIntervals.isEmpty())
  {
    auto formTrack = std::make_shared<FormationTrack>(QStringLiteral("地层单位"), 75.0);
    formTrack->setIntervals(data.formationIntervals);
    m_canvas->addTrack(formTrack);
  }

  // 3. 砂层组道（细分层道）
  if (!data.sandIntervals.isEmpty())
  {
    auto sandTrack = std::make_shared<FormationTrack>(QStringLiteral("砂层组"), 60.0);
    sandTrack->setIntervals(data.sandIntervals);
    m_canvas->addTrack(sandTrack);
  }

  // 4. 沉积旋回与符号道
  if (!data.symbolItems.isEmpty())
  {
    auto symTrack = std::make_shared<SymbolTrack>(QStringLiteral("沉积旋回"), 50.0);
    symTrack->setItems(data.symbolItems);
    m_canvas->addTrack(symTrack);
  }

  // 5. 岩性道（标准地质岩性花纹填充）
  if (!data.lithologyIntervals.isEmpty())
  {
    auto lithoTrack = std::make_shared<LithologyTrack>(QStringLiteral("岩性分析"), 80.0);
    lithoTrack->setIntervals(data.lithologyIntervals);
    m_canvas->addTrack(lithoTrack);
  }

  // 6. 深度标尺道（居中基准）
  auto scaleTrack = std::make_shared<DepthScaleTrack>(64.0);
  scaleTrack->setScaleRatio(m_scaleCombo->currentText());
  m_canvas->addTrack(scaleTrack);

  // 7. 取芯道
  if (!data.coreBarrels.isEmpty())
  {
    auto coreTrack = std::make_shared<CoreTrack>(QStringLiteral("取心数据"), 65.0);
    coreTrack->setBarrels(data.coreBarrels);
    m_canvas->addTrack(coreTrack);
  }

  // 8. 曲线道（连续物理曲线：4 根合并）
  if (!data.continuousCurves.isEmpty())
  {
    for (int i = 0; i < data.continuousCurves.size(); i += 4)
    {
      bool hasGR = false, hasNeutronDensity = false, hasGas = false, hasRes = false, hasInterp = false;
      for (int j = 0; j < 4 && (i + j) < data.continuousCurves.size(); ++j)
      {
        const QString &cn = data.continuousCurves.at(i + j).name.toUpper();
        if (cn.contains(QStringLiteral("GR")) || cn.contains(QStringLiteral("CALI")) || cn.contains(QStringLiteral("SP"))) hasGR = true;
        if (cn.contains(QStringLiteral("CNCF")) || cn.contains(QStringLiteral("ZDEN")) || cn.contains(QStringLiteral("AC")) || cn.contains(QStringLiteral("PE"))) hasNeutronDensity = true;
        if (cn.contains(QStringLiteral("RPC")) || cn.contains(QStringLiteral("RAC")) || cn.contains(QStringLiteral("RT")) || cn.contains(QStringLiteral("RXO"))) hasRes = true;
        if ((cn.startsWith(QLatin1Char('C')) && cn.length() <= 3) || cn.contains(QStringLiteral("TG")) || cn.contains(QStringLiteral("CO2"))) hasGas = true;
        if (cn.contains(QStringLiteral("PIGN")) || cn.contains(QStringLiteral("KINT")) || cn.contains(QStringLiteral("SUWI"))) hasInterp = true;
      }

      QString title;
      if (hasInterp)
        title = QStringLiteral("储层物性解释");
      else if (hasGas)
        title = QStringLiteral("气测录井烃类");
      else if (hasGR && hasRes)
        title = QStringLiteral("常规/电阻率");
      else if (hasNeutronDensity)
        title = QStringLiteral("三孔隙度/密度");
      else if (hasRes)
        title = QStringLiteral("电阻率测井");
      else
        title = QStringLiteral("测井道 %1").arg(i / 4 + 1);

      auto curveTrack = std::make_shared<CurveTrack>(title, 180.0);
      for (int j = 0; j < 4 && (i + j) < data.continuousCurves.size(); ++j)
        curveTrack->addCurve(data.continuousCurves.at(i + j));
      m_canvas->addTrack(curveTrack);
    }
  }

  // 9. 离散曲线道
  if (!data.discreteCurves.isEmpty())
  {
    for (int i = 0; i < data.discreteCurves.size(); i += 4)
    {
      const QString title = (i == 0) ? QStringLiteral("实测物性分析") : QStringLiteral("地化/生烃潜量");
      auto discTrack = std::make_shared<CurveTrack>(title, 160.0);
      for (int j = 0; j < 4 && (i + j) < data.discreteCurves.size(); ++j)
        discTrack->addCurve(data.discreteCurves.at(i + j));
      m_canvas->addTrack(discTrack);
    }
  }

  // 10. 文本道
  if (!data.textIntervals.isEmpty())
  {
    auto textTrack = std::make_shared<TextTrack>(QStringLiteral("解释结论/取样"), 120.0);
    textTrack->setIntervals(data.textIntervals);
    m_canvas->addTrack(textTrack);
  }

  // 11. 沉积相组合道（规范放置在最右侧）
  if (!data.faciesIntervals.isEmpty())
  {
    auto faciesTrack = std::make_shared<FaciesCompoundTrack>(QStringLiteral("沉积相"), 180.0);
    faciesTrack->setIntervals(data.faciesIntervals);
    m_canvas->addTrack(faciesTrack);
  }

  m_canvas->setScaleRatio(m_scaleCombo->currentText());
}

} // namespace WellComposite
