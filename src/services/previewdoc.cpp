// 层：数据
#include "previewdoc.h"

#include "../catalog/datacatalog.h"
#include "../domain/arearules.h"
#include "../domain/wellcompositemodel.h" // ComprehensiveWellData（wellCompositeAt 出参；方向 59 起显式）
#include "../domain/wellrecords.h"        // WellHeadRecord 等（wellHeadsAt 出参；方向 59 起显式）
#include "../io/dataimportservice.h"
#include "../io/geojsonaffine.h"
#include "../io/lascache.h"
#include "../io/lasdoc.h" // LasCurve/LasDoc/LasHeaderInfo（契约类型；解析入口不再直触）
#include "../io/welllogread.h" // 方向44：格式分派
#include "../io/segyreader.h"
#include "../io/streaming.h" // F3：GeoJSON 流式统计（无 DOM 增量扫描）
#include "../io/timedeptool.h"
#include "../io/wellcompositexml.h"
#include "../io/wellfileparsers.h"
#include "paleotaskservice.h"
#include "sectiondoc.h" // SectionDoc 完整定义（信号载荷构造/metatype 注册）
#include "seismictaskservice.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QSet>

namespace
{

// §3：外链源入库时留过 SHA-256——解码前照它再验一遍（文件可能在标签打开后
// 被改动）。worker 侧逐 64KB 分块哈希 + 进度上报/协作取消（原 UI 内静态函数，
// 分层收口时随解码编排一起下沉 services）。
bool externalShaMatches(const QString &absPath, const QString &expected,
                        PaleoTask *task, QString *error, bool *mismatched = nullptr)
{
  QFile f(absPath);
  if (!f.open(QIODevice::ReadOnly))
  {
    if (error)
      *error = QStringLiteral("无法读取源文件：%1").arg(absPath);
    return false;
  }
  const qint64 total = f.size();
  QCryptographicHash hash(QCryptographicHash::Sha256);
  // 64KB chunks: a 1MB stack buffer overflows the default Windows thread
  // stack when the preview runs on the QTest main thread.
  char buf[64 << 10];
  qint64 done = 0;
  int sinceReport = 0;
  for (;;)
  {
    const qint64 n = f.read(buf, sizeof(buf));
    if (n < 0)
    {
      if (error)
        *error = QStringLiteral("读取源文件失败：%1").arg(absPath);
      return false;
    }
    if (n == 0)
      break;
    hash.addData(QByteArrayView(buf, static_cast<qsizetype>(n)));
    done += n;
    if (task && ++sinceReport >= 16) // ~16MB 粒度上报
    {
      sinceReport = 0;
      task->reportBytes(done, total);
      if (task->cancelRequested())
      {
        if (error)
          *error = QStringLiteral("cancelled");
        return false;
      }
    }
  }
  if (QString::fromLatin1(hash.result().toHex())
          .compare(expected, Qt::CaseInsensitive) != 0)
  {
    if (error)
      *error = QStringLiteral("源文件与入库时的 SHA-256 不一致");
    if (mismatched)
      *mismatched = true;
    return false;
  }
  return true;
}

} // namespace

PreviewDocService::~PreviewDocService() = default;

PreviewDocService::PreviewDocService(DataImportService *svc, QObject *parent)
  : QObject(parent)
  , m_svc(svc)
{
  qRegisterMetaType<PreviewDocService::SectionDoc>(
      "PreviewDocService::SectionDoc");
  // LAS 曲线表经信号跨线程交接（任务池路径）。
  qRegisterMetaType<QList<LasCurve>>("QList<LasCurve>");
  if (m_svc)
  {
    // 文档 PDF 转换信号转发——UI 只订阅本门面，不直连 io 服务。
    connect(m_svc, &DataImportService::documentPdfReady, this,
            [this](const QString &assetId) { emit documentPdfReady(assetId); });
    connect(m_svc, &DataImportService::documentPdfFailed, this,
            [this](const QString &assetId, const QString &error) {
              emit documentPdfFailed(assetId, error);
            });
    // 壳侧转发：catalog 打开失败 / .bak 恢复告警 / 资产入库（主窗不直连
    // io 服务）。
    connect(m_svc, &DataImportService::catalogOpenFailed, this,
            [this](const QString &error) { emit catalogOpenFailed(error); });
    connect(m_svc, &DataImportService::catalogRecoveredFromBackup, this,
            [this](const QString &reason) { emit catalogRecoveredFromBackup(reason); });
    connect(m_svc, &DataImportService::imported, this,
            [this](const QString &kind, const QString &assetId,
                   const QString &layerId) {
              emit assetImported(kind, assetId, layerId);
            });
  }
}

QString PreviewDocService::catalogOpenError() const
{
  return m_svc ? m_svc->catalogOpenError() : QString();
}

QStringList PreviewDocService::assetIds(const QString &type) const
{
  return m_svc ? m_svc->assets(type) : QStringList();
}

QString PreviewDocService::assetSource(const QString &assetId) const
{
  return m_svc ? m_svc->assetSource(assetId) : QString();
}

DataCatalog *PreviewDocService::catalog() const
{
  return m_svc ? m_svc->catalog() : nullptr;
}

CatalogVersion PreviewDocService::versionForPreview(const QString &versionId) const
{
  DataCatalog *cat = catalog();
  return cat && cat->isOpen() ? cat->versionById(versionId) : CatalogVersion();
}

QString PreviewDocService::entityIdForAsset(const QString &assetId) const
{
  DataCatalog *cat = catalog();
  if (cat && cat->isOpen())
    for (const EntityAssetLink &link : cat->linksForAsset(assetId))
      if (!link.unresolved && !link.entityId.isEmpty()) return link.entityId;
  return QString();
}

QString PreviewDocService::absolutePathForVersion(const CatalogVersion &version) const
{
  return m_svc ? m_svc->absolutePathForVersion(version) : QString();
}

QString PreviewDocService::relocateVersionSource(const QString &versionId,
                                                 const QString &pickedPath,
                                                 QString *error)
{
  return m_svc ? m_svc->relocateVersionSource(versionId, pickedPath, error)
               : QString();
}

QString PreviewDocService::importSingleFile(const QString &kind,
                                            const QString &sourcePath,
                                            QString *error)
{
  // B2：直通 io 层单文件导入（同步，只能在 catalog 所属线程调用——后台
  // 导入走 prepareSingleFileImport，见 dataimportservice.h 线程规则）。
  return m_svc ? m_svc->importFile(kind, sourcePath, error) : QString();
}

std::shared_ptr<PreviewDocService::SingleFileImportJob>
PreviewDocService::prepareSingleFileImport(const QString &kind, const QString &sourcePath)
{
  Q_UNUSED(kind); // 同 importFile：kind 只是旧签名，信号带真实分类类型
  if (!m_svc)
    return nullptr;
  std::shared_ptr<ImportSession> session = m_svc->beginImport();
  if (!session)
    return nullptr;
  auto job = std::make_shared<SingleFileImportJob>();
  job->produce = [session, sourcePath] {
    DataImportService::produceFile(*session, sourcePath, DataImportService::ImportOptions{});
  };
  QPointer<DataImportService> svc = m_svc;
  job->commit = [session, svc](QString *error) -> QString {
    if (!svc)
    {
      if (error)
        *error = QStringLiteral("导入服务未就绪");
      return QString();
    }
    QString cerr;
    svc->commitImport(*session, &cerr, /*allowConflict=*/false);
    const QString id = session->fileResult.assetId;
    if (error)
      *error = id.isEmpty() ? session->error : QString();
    return id;
  };
  return job;
}

void PreviewDocService::setTaskService(PaleoTaskService *svc)
{
  m_taskSvc = svc;
  if (m_taskSvc)
  {
    if (!m_seismicTaskSvc)
      m_seismicTaskSvc =
          std::make_unique<seismic::SeismicTaskService>(m_taskSvc, 256, this);
    else
      m_seismicTaskSvc->setTaskService(m_taskSvc);
  }
}

QString PreviewDocService::targetHorizon()
{
  return AreaRules::active().targetHorizon;
}

int PreviewDocService::onnxGridRows()
{
  return AreaRules::active().onnxGrid.rows;
}

int PreviewDocService::onnxGridCols()
{
  return AreaRules::active().onnxGrid.cols;
}

bool PreviewDocService::lasAt(const QString &absPath, QStringList *names,
                              QList<LasCurve> *curves, QString *error)
{
  // D1.1：走 LasCache（内存 LRU + 磁盘缓存 + 并发合并）——纯同步语义不变，
  // 二次打开 <5ms。文件不存在/解析失败的 error 文本与 parseDoc 口径一致。
  const LasDoc doc = LasCache::shared().load(absPath);
  if (!doc.ok)
  {
    if (error)
      *error = doc.error;
    return false;
  }
  *names = doc.curveNames;
  *curves = doc.curves;
  return true;
}

bool PreviewDocService::lasHeaderAt(const QString &absPath, LasHeaderInfo *out,
                                    QString *error)
{
  LasHeaderInfo info;
  if (!WellLogRead::parseHeader(absPath, info, error))
    return false;
  if (out)
    *out = info;
  return true;
}

bool PreviewDocService::wellHeadsAt(const QString &absPath,
                                    QVector<WellHeadRecord> *out,
                                    QString *error)
{
  QFile f(absPath);
  if (!f.open(QIODevice::ReadOnly))
  {
    if (error)
      *error = f.errorString();
    return false;
  }
  if (out)
    *out = parseWellHeadText(f.readAll());
  return true;
}

bool PreviewDocService::wellTopsAt(const QString &absPath,
                                   QVector<WellTopRecord> *out,
                                   QString *error)
{
  QFile f(absPath);
  if (!f.open(QIODevice::ReadOnly))
  {
    if (error)
      *error = f.errorString();
    return false;
  }
  if (out)
    *out = parseWellTopsText(f.readAll());
  return true;
}

bool PreviewDocService::timeDepthAt(const QString &absPath, TimeDepthTable *out,
                                    QString *error)
{
  QFile f(absPath);
  if (!f.open(QIODevice::ReadOnly))
  {
    if (error)
      *error = f.errorString();
    return false;
  }
  if (out)
    *out = parseTimeDepthText(f.readAll());
  return true;
}

bool PreviewDocService::geoJsonBounds(const QString &absPath, double bounds[4],
                                      QString *error)
{
  return ::geoJsonBounds(absPath, bounds, error);
}

bool PreviewDocService::geoJsonSummaryAt(const QString &absPath, GeoJsonSummary *out,
                                         QString *error)
{
  // F3（goal/perf-systematize 簇2）：属性面板/详情的统计出口——bounds 与
  // 要素计数/属性键各一遍流式增量扫描（io/streaming，无 DOM 不整读），
  // 代替视图侧「geoJsonBounds DOM 解 + readAll 再 DOM 解」的双重整读。
  // bounds 与计数独立成败（与旧视图口径一致：缺坐标 ≠ 计数失败）。
  if (!out)
    return false;
  *out = GeoJsonSummary{};
  QString berr;
  double b[4] = {0, 0, 0, 0};
  out->hasBounds = Streaming::geoJsonBoundsStreaming(absPath, b, nullptr, &berr);
  if (out->hasBounds)
    for (int i = 0; i < 4; ++i)
      out->bounds[i] = b[i];
  QString cerr;
  out->featureCount =
      Streaming::geoJsonFeatureCountStreaming(absPath, &out->propKeys, &cerr);
  if (!out->hasBounds && out->featureCount < 0)
  {
    if (error)
      *error = !berr.isEmpty() ? berr : cerr;
    return false;
  }
  return true;
}

bool PreviewDocService::geoJsonDocumentAt(const QString &absPath,
                                        QJsonDocument *out,
                                        QString *error)
{
  QFile f(absPath);
  if (!f.open(QIODevice::ReadOnly))
  {
    if (error)
      *error = f.errorString();
    return false;
  }
  const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
  if (!doc.isObject())
  {
    if (error)
      *error = QStringLiteral("GeoJSON 解析失败");
    return false;
  }
  if (out)
    *out = doc;
  return true;
}

bool PreviewDocService::wellCompositeAt(const QString &absPath,
                                        WellComposite::ComprehensiveWellData *out,
                                        QString *error)
{
  if (!out)
    return false;
  return WellComposite::parseComprehensiveWellXml(absPath, *out, error);
}

void PreviewDocService::affinePreviewBounds(const double src[4],
                                            const QVariantMap &params,
                                            double lo[2], double hi[2])
{
  GeoAffineParams p{params.value(QStringLiteral("tx")).toDouble(),
                    params.value(QStringLiteral("ty")).toDouble(),
                    params.value(QStringLiteral("sx")).toDouble(),
                    params.value(QStringLiteral("sy")).toDouble(),
                    params.value(QStringLiteral("rotDeg")).toDouble()};
  lo[0] = lo[1] = 1e30;
  hi[0] = hi[1] = -1e30;
  for (int i = 0; i < 4; ++i)
  {
    double ox = 0.0, oy = 0.0;
    geoAffineApply(p, src[i % 2 == 0 ? 0 : 2], src[i < 2 ? 1 : 3], &ox, &oy);
    lo[0] = qMin(lo[0], ox);
    hi[0] = qMax(hi[0], ox);
    lo[1] = qMin(lo[1], oy);
    hi[1] = qMax(hi[1], oy);
  }
}

bool PreviewDocService::verifyExternalSha(const QString &assetId,
                                          const CatalogVersion &version,
                                          QString *error) const
{
  if (version.managed || version.sha256.isEmpty() || m_shaVerified.value(assetId))
    return true; // 托管副本/无指纹/本会话已验 → 放行
  DataCatalog *cat = catalog();
  if (!cat)
    return true;
  if (!cat->verifyExternalVersionSha(version, error))
  {
    // B 包 staleness-lite：外链源字节与入库时不一致 ⇒ 下游闭包里的 DERIVED
    // 产物输入失效，如实标过时（幂等；GUI 线程直调，与 catalog 线程纪律
    // 一致）。标记失败只记日志——复验拒解码的门不受影响。
    QString markErr;
    if (!cat->markDownstreamStale(version.id,
                                  QStringLiteral("上游外链版本 sha 校验失败"),
                                  &markErr))
      qWarning() << "markDownstreamStale:" << markErr;
    return false;
  }
  m_shaVerified.insert(assetId, true);
  return true;
}

void PreviewDocService::resetSha(const QString &assetId)
{
  m_shaVerified.remove(assetId);
}

bool PreviewDocService::markDownstreamStale(const QString &versionId,
                                            const QString &reason, QString *error)
{
  DataCatalog *cat = catalog();
  if (!cat)
  {
    if (error)
      *error = QStringLiteral("无 catalog");
    return false;
  }
  return cat->markDownstreamStale(versionId, reason, error);
}

void PreviewDocService::ensureDocumentPdf(const QString &assetId)
{
  if (m_svc)
    m_svc->ensureDocumentPdf(assetId);
}

PreviewDocService::DocPdfState
PreviewDocService::documentPdfState(const QString &assetId) const
{
  if (!m_svc)
    return DocPdfState::None;
  switch (m_svc->documentPdfState(assetId))
  {
    case DataImportService::DocPdfState::Pending:
      return DocPdfState::Pending;
    case DataImportService::DocPdfState::Ready:
      return DocPdfState::Ready;
    case DataImportService::DocPdfState::Failed:
      return DocPdfState::Failed;
    default:
      return DocPdfState::None;
  }
}

QString PreviewDocService::documentPdfPath(const QString &assetId) const
{
  return m_svc ? m_svc->documentPdfPath(assetId) : QString();
}

QString PreviewDocService::documentPdfError(const QString &assetId) const
{
  return m_svc ? m_svc->documentPdfError(assetId) : QString();
}

PreviewDocService::TieMarker
PreviewDocService::seismicTieMarker(const QString &assetId) const
{
  TieMarker tie;
  tie.horizon = targetHorizon();
  DataCatalog *cat = catalog();
  if (!cat || !m_svc)
    return tie;

  // survey：该资产的 seismic_volume 已决链接实体（无则角点内插跳过）。
  QString surveyId;
  for (const EntityAssetLink &l : cat->linksForAsset(assetId))
    if (l.role == QLatin1String("seismic_volume"))
      surveyId = l.entityId;
  const CatalogEntity survey =
      surveyId.isEmpty() ? CatalogEntity() : cat->entityById(surveyId);

  // ---- 标定井：catalog 序第一口有目标层位分层的井（§3/阶段 B）。----
  // 分层点有坐标用分层点，没有退回井口；TD 表取该井已决主 time_depth。
  WellTopRecord tieTop;
  {
    // 先按资产把各井的目标层位行收集起来（多井文件解析一次就好）。
    QHash<QString, WellTopRecord> topsByNorm;
    for (const CatalogAsset &a : cat->assets())
    {
      if (a.type != QLatin1String("well_stratification"))
        continue;
      const CatalogVersion tv = cat->currentVersion(a.id);
      // T8 staleness 补漏：外链版本带指纹先复验——源字节变了就不再当标定
      // 依据（失配时 verifyExternalSha 已如实标下游过时，这里跳过该文件）。
      QString shaErr;
      if (!verifyExternalSha(a.id, tv, &shaErr))
        continue;
      const QString p = tv.id.isEmpty() ? QString() : m_svc->absolutePathForVersion(tv);
      if (p.isEmpty() || !QFile::exists(p))
        continue;
      QVector<WellTopRecord> rows;
      if (!wellTopsAt(p, &rows))
        continue;
      for (const WellTopRecord &t : rows)
      {
        if (t.topName != tie.horizon)
          continue;
        const QString norm = DataCatalog::normalizeWellName(t.wellName);
        if (!norm.isEmpty() && !topsByNorm.contains(norm))
          topsByNorm.insert(norm, t);
      }
    }
    for (const CatalogEntity &w : cat->entities(QStringLiteral("well")))
    {
      const auto it = topsByNorm.constFind(DataCatalog::normalizeWellName(w.name));
      if (it == topsByNorm.constEnd())
        continue;
      tie.wellId = w.id;
      tie.wellName = w.name;
      tieTop = *it;
      tie.haveTop = true;
      if (it->hasX && it->hasY)
      {
        tie.x = it->x;
        tie.y = it->y;
        tie.hasCoords = true;
      }
      else if (w.hasSurface)
      {
        tie.x = w.surfaceX;
        tie.y = w.surfaceY;
        tie.hasCoords = true;
      }
      break;
    }
  }

  TimeDepthTable tieTd;
  if (tie.haveTop)
    for (const EntityAssetLink &l : cat->linksForEntity(tie.wellId))
    {
      if (l.role != QLatin1String("time_depth") || l.unresolved || !l.isPrimary)
        continue;
      const CatalogVersion tv = cat->currentVersion(l.assetId);
      // T8：同上——外链时深表先复验再作插值依据。
      QString shaErr;
      if (tv.id.isEmpty() || !verifyExternalSha(tv.assetId, tv, &shaErr))
        break; // 主关联只有一条（失配即不取该表——TD 留空如实降级）
      const QString p = m_svc->absolutePathForVersion(tv);
      if (!p.isEmpty())
        timeDepthAt(p, &tieTd);
      break; // 主关联只有一条
    }

  // 标定线：目标层位分层深度经 TD 表换成 ms；失败原因如实写，绝不造时间。
  // 分层点 TVD 空时改用 MD（§3）；两者皆空 → 留默认 NoTable「无时深表」。
  TimeDepthTool::TdResult td;
  if (tie.haveTop && (tieTop.hasTvd || tieTop.hasMd))
  {
    const bool useMd = !tieTop.hasTvd;
    const double depth = tieTop.hasTvd ? tieTop.tvd : tieTop.md;
    td = TimeDepthTool::interpolateTimeMs(tieTd, depth, useMd);
  }
  tie.ok = td.ok();
  if (tie.ok)
    tie.timeMs = td.timeMs;
  else
    tie.statusText = TimeDepthTool::reasonText(td.status);

  // 初始测线：标定井所在 inline（survey 角点线性内插；判不出回 -1，§4/§7）。
  if (tie.hasCoords && survey.corners.size() == 4 &&
      survey.inlineMax > survey.inlineMin)
  {
    // corners 序：(inlMin,xlMin) (inlMin,xlMax) (inlMax,xlMax) (inlMax,xlMin)。
    const double yAtMin =
        (survey.corners.at(0).second + survey.corners.at(1).second) * 0.5;
    const double yAtMax =
        (survey.corners.at(2).second + survey.corners.at(3).second) * 0.5;
    if (yAtMax != yAtMin)
    {
      const double f = (tie.y - yAtMin) / (yAtMax - yAtMin);
      const int inl =
          qRound(survey.inlineMin + f * (survey.inlineMax - survey.inlineMin));
      if (inl >= survey.inlineMin && inl <= survey.inlineMax)
        tie.initialInline = inl;
    }
  }
  return tie;
}

void PreviewDocService::requestSection(const QString &assetId,
                                       const QString &versionId,
                                       const QString &absPath, bool managed,
                                       const QString &sha256, bool isInline,
                                       int lineNo)
{
  // D1：新解码请求取消同资产仍在跑的任务——其结果反正按世代号丢弃。
  const int seq = ++m_decodeSeq[assetId];
  if (auto *old = m_decodeTask.value(assetId).data(); old && old->running())
  {
    old->requestCancel();
  }

  // worker 产出（跨线程交接，GUI 只读 finished 后的快照）。
  struct DecodeOut
  {
    std::shared_ptr<SegyReader> opened; // worker 新建索引时填（回填缓存）
    SectionDoc doc;
  };
  auto out = std::make_shared<DecodeOut>();
  out->doc.isInline = isInline;
  out->doc.lineNo = lineNo;

  const std::shared_ptr<SegyReader> cachedReader = m_segyReaders.value(assetId);
  const bool needSha =
      !managed && !sha256.isEmpty() && !m_shaVerified.value(assetId);
  // B 包 staleness-lite：sha 是否失配由 worker 判定（bool 经 shared_ptr
  // 交接），回 GUI 线程再碰 catalog——下游标记的线程纪律与库内一致。
  const auto shaMismatch = std::make_shared<bool>(false);

  // T23+D1：索引/SHA 每资产一次（缓存命中即跳过）；道索引与测线解码在
  // 任务池执行并回报字节进度/ETA；任务可协作取消。
  // P4 D2：磁盘索引目录在 GUI 线程取（worker 只值捕获）。
  const QString idxDir = m_svc ? m_svc->indexCacheDir() : QString();
  const auto work = [absPath, isInline, lineNo, needSha, sha256, cachedReader, idxDir,
                     out, shaMismatch](PaleoTask *t) -> QString {
    std::shared_ptr<SegyReader> reader = cachedReader;
    SegyOptions opts;
    if (t)
    {
      opts.progress = [t](qint64 d, qint64 tot) { t->reportBytes(d, tot); };
      opts.cancel = [t]() { return t->cancelRequested(); };
    }
    if (!reader)
    {
      if (needSha)
      {
        QString serr;
        if (!externalShaMatches(absPath, sha256, t, &serr, shaMismatch.get()))
          return (t && t->cancelRequested()) ? QString() : serr;
      }
      reader = std::make_shared<SegyReader>();
      QString err;
      // P4 D2：磁盘索引层——会话内重开免扫全文件道头（大 survey 分钟级 →
      // 亚秒）。索引目录取导入服务的工程级 artifacts/index/segy；无工程
      // （测试/外链未接线）时退回 open() 旧路径。
      const bool opened = idxDir.isEmpty() ? reader->open(absPath, &err, &opts)
                                           : reader->openCached(absPath, idxDir, &err, &opts);
      if (!opened)
        return (t && t->cancelRequested())
                   ? QString()
                   : (err.isEmpty() ? QStringLiteral("无法打开文件") : err);
      out->opened = reader;
    }
    QString err;
    const bool ok =
        isInline ? reader->readInline(lineNo, &out->doc.traces, &err, &opts, &out->doc.readReport)
                 : reader->readCrossline(lineNo, &out->doc.traces, &err, &opts, &out->doc.readReport);
    if (!ok && !(t && t->cancelRequested()))
      return err.isEmpty() ? QStringLiteral("无法解码测线") : err;
    out->doc.sampleIntervalUs = reader->sampleIntervalUs();
    out->doc.startTimeMs = reader->geometry().startTimeMs;
    return QString();
  };

  const auto apply = [this, assetId, seq, out, shaMismatch,
                      versionId](PaleoTask::State st, const QString &errText) {
    if (seq != m_decodeSeq.value(assetId))
      return; // 陈旧结果丢弃：更新一代 decode 已接管（发射前压制）
    if (st == PaleoTask::State::Succeeded)
    {
      if (out->opened)
        m_segyReaders.insert(assetId, out->opened);
      m_shaVerified.insert(assetId, true);
      emit seismicSectionReady(assetId, out->doc);
    }
    else if (st == PaleoTask::State::Failed)
    {
      // sha 失配（worker 判定）：下游 DERIVED 如实标过时——GUI 线程直调、
      // 幂等；读取失败/取消/纯解码失败不产标（非源被改的证据）。
      if (*shaMismatch && !versionId.isEmpty())
      {
        QString markErr;
        if (!markDownstreamStale(versionId,
                                 QStringLiteral("上游外链版本 sha 校验失败"),
                                 &markErr))
          qWarning() << "markDownstreamStale:" << markErr;
      }
      emit seismicSectionFailed(
          assetId, errText.isEmpty() ? QStringLiteral("无法解码测线") : errText);
    }
    else
      emit seismicSectionCancelled(assetId);
  };

  if (m_taskSvc)
  {
    // quiet：换测线是交互内嵌解码（控件组自带「正在建立道索引」挂起态），
    // 不拉起任务中心。
    auto *task = m_taskSvc->start(
        QStringLiteral("解码剖面 %1").arg(QFileInfo(absPath).fileName()), work,
        QString(), /*quiet=*/true);
    m_decodeTask[assetId] = task;
    connect(task, &PaleoTask::finished, this,
            [apply, task]() { apply(task->state(), task->errorText()); });
  }
  else
  {
    // 无任务服务（测试/小环境）：同步旧路径，行为与接线前一致。
    const QString err = work(nullptr);
    apply(err.isEmpty() ? PaleoTask::State::Succeeded : PaleoTask::State::Failed,
          err);
  }
}

void PreviewDocService::releaseSection(const QString &assetId)
{
  // 标签关掉即释放该资产的索引缓存（持有文件句柄级状态）与世代号；进行
  // 中的解码任务请求取消——结果没人等了。
  m_segyReaders.remove(assetId);
  // 世代号 +1 而不是移除：移除后同 assetId 的下一次请求会从 1 重新计数，
  // 与刚取消、尚未落地的旧代撞号（#154：换工程后 ast-N 复用时尤甚）。
  ++m_decodeSeq[assetId];
  m_shaVerified.remove(assetId);
  if (auto *t = m_decodeTask.value(assetId).data(); t && t->running())
    t->requestCancel();
  m_decodeTask.remove(assetId);
}

void PreviewDocService::requestLas(const QString &key, const QString &absPath,
                                    const QStringList &siblingPaths)
{
  // 与 requestSection 同一世代号纪律：同 key 新请求作废旧代（旧任务协作
  // 取消，其结果按世代号在发射前丢弃）。
  const int seq = ++m_lasSeq[key];
  m_lasSiblings.remove(key);
  if (auto *old = m_lasTask.value(key).data(); old && old->running())
    old->requestCancel();

  // worker 产出（跨线程交接，GUI 只读 finished 后的快照）。
  // 当前文件失败 = 整次失败；兄弟文件失败只跳过该文件。
  auto names = std::make_shared<QStringList>();
  auto curves = std::make_shared<QList<LasCurve>>();
  auto siblings = std::make_shared<QHash<QString, LasDoc>>();
  const auto work = [absPath, siblingPaths, names, curves,
                     siblings](PaleoTask *t) -> QString {
    // D1.1/D4.7：LasCache::load——同文件并发请求（不同 key 的两个标签、
    // 预取 + 点击）只解析一份，其余等 future 拷贝。
    const LasDoc doc = LasCache::shared().load(absPath);
    if (!doc.ok)
      return (t && t->cancelRequested()) ? QString() : doc.error;
    *names = doc.curveNames;
    *curves = doc.curves;
    const QString current = QFileInfo(absPath).absoluteFilePath();
    QSet<QString> seen;
    for (const QString &sp : siblingPaths)
    {
      if (t && t->cancelRequested())
        return QString();
      if (sp.isEmpty())
        continue;
      const QString abs = QFileInfo(sp).absoluteFilePath();
      if (abs.isEmpty() || abs == current || seen.contains(abs))
        continue;
      seen.insert(abs);
      const LasDoc sib = LasCache::shared().load(sp);
      if (!sib.ok)
        continue;
      siblings->insert(sp, sib);
      if (abs != sp)
        siblings->insert(abs, sib);
    }
    return QString();
  };
  const auto apply = [this, key, seq, names, curves,
                      siblings](PaleoTask::State st, const QString &errText) {
    if (seq != m_lasSeq.value(key))
      return; // 陈旧结果丢弃：更新一代请求已接管（发射前压制）
    if (st == PaleoTask::State::Succeeded)
    {
      m_lasSiblings.insert(key, *siblings);
      emit lasReady(key, *names, *curves);
    }
    else if (st == PaleoTask::State::Failed)
    {
      m_lasSiblings.remove(key);
      emit lasFailed(key, errText.isEmpty()
                              ? QStringLiteral("无法解析 LAS 文件") : errText);
    }
    else
    {
      m_lasSiblings.remove(key);
      emit lasCancelled(key);
    }
  };

  if (m_taskSvc)
  {
    // quiet：点开曲线页是交互内嵌解析，不拉起任务中心。
    auto *task = m_taskSvc->start(
        QStringLiteral("解析测井 %1").arg(QFileInfo(absPath).fileName()), work,
        QString(), /*quiet=*/true);
    m_lasTask[key] = task;
    connect(task, &PaleoTask::finished, this,
            [apply, task]() { apply(task->state(), task->errorText()); });
  }
  else
  {
    // 无任务服务（测试/小环境）：同步旧路径，行为与接线前一致。
    const QString err = work(nullptr);
    apply(err.isEmpty() ? PaleoTask::State::Succeeded : PaleoTask::State::Failed,
          err);
  }
}

QHash<QString, LasDoc> PreviewDocService::lasSiblingDocs(const QString &key) const
{
  return m_lasSiblings.value(key);
}

void PreviewDocService::resetProjectState()
{
  for (auto it = m_decodeTask.begin(); it != m_decodeTask.end(); ++it)
    if (PaleoTask *t = it.value().data(); t && t->running())
      t->requestCancel();
  for (auto it = m_lasTask.begin(); it != m_lasTask.end(); ++it)
    if (PaleoTask *t = it.value().data(); t && t->running())
      t->requestCancel();
  for (auto it = m_pyramidTask.begin(); it != m_pyramidTask.end(); ++it)
    if (PaleoTask *t = it.value().data(); t && t->running())
      t->requestCancel();
  for (auto it = m_decodeSeq.begin(); it != m_decodeSeq.end(); ++it)
    ++it.value();
  for (auto it = m_lasSeq.begin(); it != m_lasSeq.end(); ++it)
    ++it.value();
  m_decodeTask.clear();
  m_lasTask.clear();
  m_pyramidTask.clear();
  m_segyReaders.clear();
  m_shaVerified.clear();
  m_lasSiblings.clear();
  m_pyramidState.clear();
}

void PreviewDocService::releaseLas(const QString &key)
{
  // 标签/调用方关掉即释放世代号；进行中的解析请求取消——结果没人等了。
  ++m_lasSeq[key]; // 同 releaseSection：+1 作废在途代，不移除（防撞号）
  m_lasSiblings.remove(key);
  if (auto *t = m_lasTask.value(key).data(); t && t->running())
    t->requestCancel();
  m_lasTask.remove(key);
}

int PreviewDocService::prefetch(const QStringList &absPaths)
{
  // D1.3/D4.6：低优先级后台预取——用户点击（Normal/High）不被预取挡道；
  // LasCache 的 in-flight 合并保证「预取跑到一半用户点开」不双解析。
  if (!m_taskSvc)
  {
    LasCache::shared().prefetch(absPaths);
    return 0;
  }
  int submitted = 0;
  for (const QString &p : absPaths)
  {
    m_taskSvc->start(QStringLiteral("预取测井 %1").arg(QFileInfo(p).fileName()),
                     [p](PaleoTask *) -> QString {
                       LasCache::shared().load(p);
                       return QString();
                     },
                     QString(), PaleoTask::Priority::Low,
                     /*quiet=*/true); // 后台预取不拉起任务中心
    ++submitted;
  }
  return submitted;
}

// ---------------------------------------------------------------------------
// B3（wave/deepen-perf）：栅格金字塔版本预热。
// ---------------------------------------------------------------------------
bool PreviewDocService::rasterPyramidReady(const QString &assetId) const
{
  return m_pyramidState.value(assetId, int(PyramidState::None)) ==
         int(PyramidState::Ready);
}

void PreviewDocService::ensureRasterPyramidVersion(const QString &assetId)
{
  if (assetId.isEmpty() || !m_svc)
    return;
  const int st = m_pyramidState.value(assetId, int(PyramidState::None));
  if (st == int(PyramidState::InFlight) || st == int(PyramidState::Ready))
    return; // 在途合并 / 会话内已就绪——幂等

  // 解析当前版本的栅格绝对路径（GUI 线程读 catalog——同步快照）。
  DataCatalog *cat = m_svc->catalog();
  if (!cat)
    return;
  const CatalogAsset asset = cat->assetById(assetId);
  if (asset.id.isEmpty())
    return;
  const CatalogVersion version = cat->currentVersion(assetId);
  if (version.id.isEmpty())
    return;
  const QString ext = QFileInfo(version.fileName).suffix().toLower();
  const bool rasterExt = ext == QLatin1String("tif") || ext == QLatin1String("tiff") ||
                         ext == QLatin1String("png") || ext == QLatin1String("jpg") ||
                         ext == QLatin1String("jpeg");
  if (!rasterExt)
    return;
  const QString abs = m_svc->absolutePathForVersion(version);
  if (abs.isEmpty() || !QFileInfo::exists(abs))
    return;
  // 外链源不建 .ovr（不往用户目录写边车）；受管副本才建。
  const bool managed = version.managed;

  const auto work = [io = m_svc, abs, managed](PaleoTask *t) -> QString {
    QString err;
    io->ensureRasterPyramids({abs}, &err);
    if (managed)
    {
      const qint64 size = QFileInfo(abs).size();
      io->buildRasterOverviews(
          abs, &err,
          [t, size](double fraction) {
            if (t)
              t->reportBytes(qint64(fraction * size), size); // 0..1 → 字节面
            return !(t && t->cancelRequested());
          });
    }
    return err;
  };
  const auto apply = [this, assetId](bool ok) {
    m_pyramidState[assetId] = int(ok ? PyramidState::Ready : PyramidState::Failed);
    emit rasterPyramidFinished(assetId, ok);
  };

  if (m_taskSvc)
  {
    m_pyramidState[assetId] = int(PyramidState::InFlight);
    // quiet：交互内嵌预热（大图预览打开时的后台加速），不拉起任务中心。
    auto *task = m_taskSvc->start(
        QStringLiteral("构建栅格金字塔 %1").arg(QFileInfo(abs).fileName()), work,
        QString(), /*quiet=*/true);
    m_pyramidTask[assetId] = task;
    connect(task, &PaleoTask::finished, this, [apply, task] {
      apply(task->state() == PaleoTask::State::Succeeded);
    });
  }
  else
  {
    // 无任务服务（测试/小环境）：同步旧路径。
    apply(work(nullptr).isEmpty());
  }
}
