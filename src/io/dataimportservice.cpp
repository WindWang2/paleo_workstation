#include "dataimportservice.h"

#include "../catalog/datacatalog.h"
#include "../metadata/layermanifest.h"
#include "../metadata/paleoprojectstore.h"
#include "../qgis/qgislayerservice.h"
#include "horizonbinner.h"
#include "lasparser.h"
#include "projectclassifier.h"
#include "segyreader.h"
#include "wellfileparsers.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSet>
#include <QStandardPaths>

#include <cstdio>

// ---------------------------------------------------------------------------
// project_area 导入契约（plan §3）：
//   classify → parse metadata → resolve/create entity → managed RAW（复制边算
//   SHA-256，落盘只读）/ 外部链接（SEG-Y 一律外链）→ 显式 entity_asset_link。
//   层位在 8 个层序界面集合内 → 派生时间栅格（DERIVED，父版本=RAW）并登记
//   LayerManifest（图层清单只登记要画进 QGIS 的结果）。
//   身份顺序：已有 id → UWI → 规范化井名 → 别名；文件名不作身份，仅作回退。
// ---------------------------------------------------------------------------
namespace
{
  void setError(QString *error, const QString &text)
  {
    if (error)
      *error = text;
  }

  // 编图层序界面集合（plan §3/§5E）。
  bool isKnownSequenceBoundary(const QString &stem)
  {
    static const QSet<QString> kBoundaries{
        QStringLiteral("C3"),  QStringLiteral("C6"),  QStringLiteral("D53"),
        QStringLiteral("D61"), QStringLiteral("D62"), QStringLiteral("D63"),
        QStringLiteral("D71"), QStringLiteral("D72")};
    return kBoundaries.contains(stem.toUpper());
  }

  // 阶段 D 固定规则：参考资料目录 / HZ28-6-1 命名的 XML 一律作辅助参考，
  // 不按内容挂井、不并进 A1–A20。
  bool isFixedAuxiliaryPath(const QString &path)
  {
    const QStringList parts = QFileInfo(path).absolutePath().split(QLatin1Char('/'));
    for (const QString &p : parts)
      if (p == QString::fromUtf8("参考资料"))
        return true;
    return QFileInfo(path).completeBaseName().contains(QStringLiteral("HZ28-6-1"));
  }

  QString readFileOrEmpty(const QString &path)
  {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
      return QString();
    return QString::fromUtf8(f.readAll());
  }
} // namespace

DataImportService::DataImportService(QgisLayerService *layers, PaleoProjectStore *store,
                                     QObject *parent)
  : QObject(parent)
  , m_layers(layers)
  , m_store(store)
  , m_catalog(new DataCatalog(this))
{
}

DataImportService::~DataImportService()
{
  if (m_pdfProc)
  {
    m_pdfProc->kill();
    m_pdfProc->waitForFinished(2000);
  }
}

void DataImportService::setProjectDir(const QString &dir)
{
  // 工程切换时丢弃在途/排队转换——它们写向旧工程目录，且状态随之失效。
  if (m_pdfProc)
  {
    m_pdfProc->disconnect(this);
    m_pdfProc->kill();
    m_pdfProc->deleteLater();
    m_pdfProc = nullptr;
  }
  m_pdfQueue.clear();
  m_pdfCurrent.clear();
  m_pdfPending.clear();
  m_pdfErrors.clear();

  m_projectDir = dir;
  QString err;
  if (!m_catalog->open(dir, &err))
    qWarning("DataImportService: catalog open failed: %s", qPrintable(err));
}

DataImportService::WellBind DataImportService::resolveWell(const QString &name) const
{
  WellBind b;
  if (name.trimmed().isEmpty())
  {
    b.unresolved = true;
    return b;
  }
  const QStringList ids = m_catalog->wellsMatchingName(name);
  if (ids.size() == 1)
  {
    b.entityId = ids.front();
    return b;
  }
  b.unresolved = true;
  b.candidates = ids; // 0 个或 2+ 个
  return b;
}

bool DataImportService::ensureAuxUnresolved(QString *entityId, QString *error)
{
  const QString id = QStringLiteral("aux-unresolved");
  if (!m_catalog->hasEntity(id))
  {
    CatalogEntity aux;
    aux.id = id;
    aux.entityType = QStringLiteral("auxiliary");
    aux.name = QStringLiteral("未解析数据");
    if (!m_catalog->addEntity(aux, error))
      return false;
  }
  *entityId = id;
  return true;
}

bool DataImportService::storeManagedRaw(const QString &sourcePath, const QString &assetId,
                                        const QString &versionId, QString *relPathOut,
                                        QString *shaOut, QString *error)
{
  QFile src(sourcePath);
  if (!src.open(QIODevice::ReadOnly))
  {
    setError(error, QStringLiteral("cannot read source %1").arg(sourcePath));
    return false;
  }
  const QString fileName = QFileInfo(sourcePath).fileName();
  const QString relPath = QStringLiteral("artifacts/") +
                          DataCatalog::managedPath(QStringLiteral("raw"), assetId, versionId, fileName);
  const QString dst = QDir(m_projectDir).absoluteFilePath(relPath);
  const QDir dir = QFileInfo(dst).absoluteDir();
  if (!dir.exists() && !dir.mkpath(QStringLiteral(".")))
  {
    setError(error, QStringLiteral("cannot create directory %1").arg(dir.absolutePath()));
    return false;
  }

  // 边复制边算 SHA-256；partial + rename 原子落位；成功后置只读。
  QCryptographicHash hash(QCryptographicHash::Sha256);
  const QString partial = dst + QStringLiteral(".partial");
  QFile out(partial);
  if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
  {
    setError(error, QStringLiteral("cannot write %1").arg(partial));
    return false;
  }
  char buf[1 << 20];
  qint64 n = 0;
  while ((n = src.read(buf, sizeof(buf))) > 0)
  {
    hash.addData(QByteArrayView(buf, static_cast<qsizetype>(n)));
    if (out.write(buf, n) != n)
    {
      out.close();
      QFile::remove(partial);
      setError(error, QStringLiteral("short write to %1").arg(partial));
      return false;
    }
  }
  if (n < 0)
  {
    out.close();
    QFile::remove(partial);
    setError(error, QStringLiteral("read error on %1").arg(sourcePath));
    return false;
  }
  out.close();
  if (::rename(QFile::encodeName(partial).constData(), QFile::encodeName(dst).constData()) != 0)
  {
    QFile::remove(partial);
    setError(error, QStringLiteral("cannot place %1").arg(dst));
    return false;
  }
  QFile::setPermissions(dst, QFileDevice::ReadOwner | QFileDevice::ReadUser |
                                 QFileDevice::ReadGroup | QFileDevice::ReadOther);
  if (relPathOut)
    *relPathOut = relPath;
  if (shaOut)
    *shaOut = QString::fromLatin1(hash.result().toHex());
  return true;
}

QString DataImportService::importProjectFile(const QString &sourcePath, QString *error)
{
  return importProjectFile(sourcePath, ImportOptions{}, error);
}

QString DataImportService::importProjectFile(const QString &sourcePath, const ImportOptions &options,
                                             QString *error)
{
  const auto fail = [&](const QString &msg) -> QString {
    setError(error, msg);
    emit importFailed(QString(), sourcePath, msg);
    return QString();
  };

  if (!m_layers || !m_store)
    return fail(QStringLiteral("import service is not fully wired"));
  if (m_projectDir.isEmpty())
    return fail(QStringLiteral("project dir is not set"));
  if (sourcePath.isEmpty() || !QFile::exists(sourcePath))
    return fail(QStringLiteral("找不到源文件: %1").arg(sourcePath));

  const QFileInfo fi(sourcePath);
  const QByteArray xmlContent =
      fi.suffix().compare(QStringLiteral("xml"), Qt::CaseInsensitive) == 0
          ? readFileOrEmpty(sourcePath).toUtf8()
          : QByteArray();
  const ProjectClassification cls = classifyProjectImport(sourcePath, xmlContent);
  const QString stem = fi.completeBaseName();

  const QString assetId = m_catalog->nextAssetId();
  const QString versionId = m_catalog->nextVersionId();

  CatalogAsset asset;
  asset.id = assetId;
  asset.type = cls.type;
  asset.format = cls.format;
  asset.displayName = fi.fileName();
  if (!m_catalog->addAsset(asset, error))
    return fail(*error);

  CatalogVersion version;
  version.id = versionId;
  version.assetId = assetId;
  version.stage = QStringLiteral("RAW");
  version.versionNumber = 1;
  version.fileName = fi.fileName();
  version.sourceUri = fi.absoluteFilePath();

  // SEG-Y 一律外链（plan §3：966 MB 体不复制）。其余默认受管 RAW。
  const bool external = options.linkExternal || cls.type == QLatin1String("seismic");
  if (external)
  {
    version.managed = false;
    version.path = fi.absoluteFilePath();
  }
  else
  {
    QString relPath, sha;
    if (!storeManagedRaw(sourcePath, assetId, versionId, &relPath, &sha, error))
      return fail(*error);
    version.path = relPath;
    version.sha256 = sha;
  }
  if (!m_catalog->addVersion(version, error))
    return fail(*error);

  // ---- 实体解析与关联（角色沿用已有名字）----
  const QString auxRefRole = QStringLiteral("reference");
  QString manifestLayerId;
  // 阶段 D：参考资料目录 / HZ28-6-1 命名的井类内容 XML 仍固定作辅助参考。
  const bool fixedAux = isFixedAuxiliaryPath(sourcePath) &&
                        (cls.type == QLatin1String("well_head") ||
                         cls.type == QLatin1String("well_log"));

  if (cls.type == QLatin1String("well_head") && !fixedAux)
  {
    QFile f(sourcePath);
    if (!f.open(QIODevice::ReadOnly))
      return fail(QStringLiteral("cannot read %1").arg(sourcePath));
    const QVector<WellHeadRecord> rows = parseWellHeadText(f.readAll());
    if (rows.isEmpty())
      return fail(QStringLiteral("no well head rows in %1").arg(sourcePath));
    for (const WellHeadRecord &r : rows)
    {
      const QString wid = QStringLiteral("well-%1").arg(r.name);
      if (!m_catalog->hasEntity(wid))
      {
        CatalogEntity w;
        w.id = wid;
        w.entityType = QStringLiteral("well");
        w.name = r.name;
        w.hasSurface = true;
        w.surfaceX = r.x;
        w.surfaceY = r.y;
        w.kb = r.kb;
        w.td = r.td;
        // 局部测网坐标：真投影参数出现前保持未变换（plan §3）。
        w.coordinateStatus = QStringLiteral("untransformed");
        if (!m_catalog->addEntity(w, error))
          return fail(*error);
      }
      EntityAssetLink link;
      link.entityType = QStringLiteral("well");
      link.entityId = wid;
      link.assetId = assetId;
      link.role = QStringLiteral("well_head");
      link.isPrimary = true;
      if (!m_catalog->addLink(link, error))
        return fail(*error);
    }
  }
  else if (cls.type == QLatin1String("well_log") && !fixedAux)
  {
    // LAS 先读 ~W 的 WELL；XML 测井/读不到时用文件名主名。
    QString wellName, uwi;
    if (cls.format == QLatin1String("las"))
      LasParser::readWellInfo(sourcePath, wellName, uwi);
    WellBind bind = resolveWell(wellName);
    if (bind.unresolved && bind.candidates.isEmpty())
      bind = resolveWell(stem); // A1.Las → A1
    EntityAssetLink link;
    link.entityType = QStringLiteral("well");
    link.assetId = assetId;
    link.role = QStringLiteral("well_log");
    if (!bind.unresolved)
    {
      link.entityId = bind.entityId;
      link.isPrimary = true;
      if (!m_catalog->addLink(link, error))
        return fail(*error);
    }
    else if (bind.candidates.size() >= 2)
    {
      // 双候选：每个候选一条 unresolved 链接，不并井（§3）。
      link.unresolved = true;
      for (const QString &cand : bind.candidates)
      {
        link.entityId = cand;
        if (!m_catalog->addLink(link, error))
          return fail(*error);
      }
    }
    else
    {
      QString auxId;
      if (!ensureAuxUnresolved(&auxId, error))
        return fail(*error);
      link.entityType = QStringLiteral("auxiliary");
      link.entityId = auxId;
      link.unresolved = true;
      if (!m_catalog->addLink(link, error))
        return fail(*error);
    }
  }
  else if (cls.type == QLatin1String("well_stratification") ||
           cls.type == QLatin1String("time_depth"))
  {
    // 井名来自文件内容（分层=井名列；时深=# Well 行），规则同测井。
    QStringList names;
    if (cls.type == QLatin1String("well_stratification"))
    {
      QFile f(sourcePath);
      if (!f.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("cannot read %1").arg(sourcePath));
      const QVector<WellTopRecord> tops = parseWellTopsText(f.readAll());
      for (const WellTopRecord &t : tops)
        if (!names.contains(t.wellName))
          names.append(t.wellName);
    }
    else
    {
      QFile f(sourcePath);
      if (!f.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("cannot read %1").arg(sourcePath));
      const TimeDepthTable td = parseTimeDepthText(f.readAll());
      names.append(td.wellName.isEmpty() ? stem : td.wellName);
    }
    if (names.isEmpty())
      return fail(QStringLiteral("no well names in %1").arg(sourcePath));

    const QString role = cls.type == QLatin1String("well_stratification")
                             ? QStringLiteral("tops")
                             : QStringLiteral("time_depth");
    bool anyLinked = false;
    for (const QString &n : names)
    {
      WellBind bind = resolveWell(n);
      if (bind.unresolved && bind.candidates.isEmpty() && names.size() == 1)
        bind = resolveWell(stem); // 单井文件的文件名主名回退
      EntityAssetLink link;
      link.entityType = QStringLiteral("well");
      link.assetId = assetId;
      link.role = role;
      if (!bind.unresolved)
      {
        link.entityId = bind.entityId;
        link.isPrimary = true;
        if (!m_catalog->addLink(link, error))
          return fail(*error);
        anyLinked = true;
      }
      else if (bind.candidates.size() >= 2)
      {
        link.unresolved = true;
        for (const QString &cand : bind.candidates)
        {
          link.entityId = cand;
          if (!m_catalog->addLink(link, error))
            return fail(*error);
        }
        anyLinked = true;
      }
    }
    if (!anyLinked)
    {
      // 零匹配：一条 unresolved 挂辅助实体，不新建井。
      EntityAssetLink link;
      QString auxId;
      if (!ensureAuxUnresolved(&auxId, error))
        return fail(*error);
      link.entityType = QStringLiteral("auxiliary");
      link.entityId = auxId;
      link.assetId = assetId;
      link.role = role;
      link.unresolved = true;
      if (!m_catalog->addLink(link, error))
        return fail(*error);
    }
  }
  else if (cls.type == QLatin1String("horizon"))
  {
    const bool known = isKnownSequenceBoundary(stem);
    const QString sbId = QStringLiteral("sb-%1").arg(stem.toUpper());
    if (!m_catalog->hasEntity(sbId))
    {
      CatalogEntity sb;
      sb.id = sbId;
      sb.entityType = QStringLiteral("sequence_boundary");
      sb.name = stem.toUpper();
      if (!known)
        sb.extra.insert(QStringLiteral("pending"), true); // 未决层位，不进编图 chip
      if (!m_catalog->addEntity(sb, error))
        return fail(*error);
    }
    EntityAssetLink link;
    link.entityType = QStringLiteral("sequence_boundary");
    link.entityId = sbId;
    link.assetId = assetId;
    link.role = QStringLiteral("horizon");
    link.isPrimary = true;
    link.unresolved = !known;
    if (!m_catalog->addLink(link, error))
      return fail(*error);

    // 已知界面：装箱派生时间栅格（DERIVED，父版本=RAW）并登记图层清单。
    if (known)
    {
      QFile f(sourcePath);
      if (!f.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("cannot read %1").arg(sourcePath));
      BinnedHorizon binned;
      if (!binHorizon(f.readAll(), &binned, error))
        return fail(*error);

      const QString derivedVersionId = m_catalog->nextVersionId();
      const QString relPath = QStringLiteral("artifacts/") +
                              DataCatalog::managedPath(QStringLiteral("derived"), assetId,
                                                       derivedVersionId,
                                                       stem.toUpper() + QStringLiteral(".tif"));
      const QString tifPath = QDir(m_projectDir).absoluteFilePath(relPath);
      if (!writeHorizonGeoTiff(binned, tifPath, error))
        return fail(*error);
      QFile::setPermissions(tifPath, QFileDevice::ReadOwner | QFileDevice::ReadUser |
                                         QFileDevice::ReadGroup | QFileDevice::ReadOther);

      CatalogVersion derived;
      derived.id = derivedVersionId;
      derived.assetId = assetId;
      derived.stage = QStringLiteral("DERIVED");
      derived.versionNumber = 2;
      derived.managed = true;
      derived.path = relPath;
      derived.sourceUri = fi.absoluteFilePath();
      derived.fileName = stem.toUpper() + QStringLiteral(".tif");
      derived.parentVersionIds = QStringList{versionId};
      derived.extra.insert(QStringLiteral("collisions"), binned.collisions);
      derived.extra.insert(QStringLiteral("grid_rows"), binned.rows);
      derived.extra.insert(QStringLiteral("grid_cols"), binned.cols);
      derived.extra.insert(QStringLiteral("z_units"), QStringLiteral("ms"));
      derived.extra.insert(QStringLiteral("z_min"), binned.zMin);
      derived.extra.insert(QStringLiteral("z_max"), binned.zMax);
      derived.extra.insert(QStringLiteral("filled_cells"), binned.filledCells);
      if (!m_catalog->addVersion(derived, error))
        return fail(*error);

      // 图层清单只登记要画的结果（§2）：北向上时间栅格 + 局部测网 CRS。
      LayerDeclaration decl;
      decl.layerId = QStringLiteral("horizon.%1").arg(stem.toUpper());
      decl.horizon = stem.toUpper();
      decl.type = QStringLiteral("raster");
      decl.source = tifPath;
      decl.group = QStringLiteral("00_Data");
      QString derr;
      if (!m_layers->declare(decl, &derr))
        return fail(derr.isEmpty() ? QStringLiteral("manifest declare failed") : derr);
      manifestLayerId = decl.layerId;
    }
  }
  else if (cls.type == QLatin1String("seismic"))
  {
    // 打开（索引式）冻结 survey 几何：角点、inline/crossline 范围、采样间隔、起始时间。
    SegyReader reader;
    QString serr;
    if (!reader.open(sourcePath, &serr))
      return fail(serr);
    const SegyGeometry g = reader.geometry();
    const QString surveyId = QStringLiteral("survey-%1").arg(stem);
    if (!m_catalog->hasEntity(surveyId))
    {
      CatalogEntity s;
      s.id = surveyId;
      s.entityType = QStringLiteral("seismic_survey");
      s.name = stem;
      s.inlineMin = g.inlineMin;
      s.inlineMax = g.inlineMax;
      s.xlineMin = g.xlineMin;
      s.xlineMax = g.xlineMax;
      s.sampleIntervalUs = reader.sampleIntervalUs();
      s.startTimeMs = g.startTimeMs;
      for (int i = 0; i < 4; ++i)
        s.corners.append({g.cornerX[i], g.cornerY[i]});
      if (!m_catalog->addEntity(s, error))
        return fail(*error);
    }
    EntityAssetLink link;
    link.entityType = QStringLiteral("seismic_survey");
    link.entityId = surveyId;
    link.assetId = assetId;
    link.role = QStringLiteral("seismic_volume");
    link.isPrimary = true;
    if (!m_catalog->addLink(link, error))
      return fail(*error);
  }
  else
  {
    // document / image_reference / geojson / unknown / 参考资料 XML：辅助实体 + reference。
    const QString auxId = m_catalog->nextEntityId(QStringLiteral("aux"));
    CatalogEntity aux;
    aux.id = auxId;
    aux.entityType = QStringLiteral("auxiliary");
    aux.name = stem;
    if (cls.type == QLatin1String("geojson"))
    {
      aux.extra.insert(QStringLiteral("georeferenced"), false); // 未配准，不生成地图图层
      // 相/亚相/微相名称收成图例字典（阶段 D）。
      QFile gf(sourcePath);
      if (gf.open(QIODevice::ReadOnly))
      {
        const QJsonDocument doc = QJsonDocument::fromJson(gf.readAll());
        const QJsonArray feats = doc.object().value(QStringLiteral("features")).toArray();
        QVariantMap legend;
        for (const QJsonValue &fv : feats)
        {
          const QJsonObject props = fv.toObject().value(QStringLiteral("properties")).toObject();
          for (auto it = props.begin(); it != props.end(); ++it)
          {
            if (!it.key().contains(QString::fromUtf8("相")))
              continue;
            QStringList vals = legend.value(it.key()).toStringList();
            const QString v = it.value().toString();
            if (!v.isEmpty() && !vals.contains(v))
              vals.append(v);
            legend.insert(it.key(), vals);
          }
        }
        aux.extra.insert(QStringLiteral("legend"), legend);
      }
    }
    if (!m_catalog->addEntity(aux, error))
      return fail(*error);
    EntityAssetLink link;
    link.entityType = QStringLiteral("auxiliary");
    link.entityId = auxId;
    link.assetId = assetId;
    link.role = auxRefRole;
    link.isPrimary = true;
    if (!m_catalog->addLink(link, error))
      return fail(*error);
  }

  emit imported(cls.type, assetId, manifestLayerId);
  return assetId;
}

QString DataImportService::importFile(const QString &kind, const QString &sourcePath, QString *error)
{
  // 旧签名：kind 只透传给信号（由 importProjectFile 内部再次发射真实类型）。
  Q_UNUSED(kind);
  return importProjectFile(sourcePath, ImportOptions{}, error);
}

QString DataImportService::absolutePath(const QString &assetId) const
{
  return absolutePathForVersion(m_catalog->currentVersion(assetId));
}

QString DataImportService::absolutePathForVersion(const CatalogVersion &v) const
{
  if (v.path.isEmpty())
    return QString();
  if (!v.managed)
    return v.path;
  return QDir(m_projectDir).absoluteFilePath(v.path);
}

// ---------------------------------------------------------------------------
// 文档 PDF 预览：doc/docx/ppt/pptx 经 LibreOffice headless 转 PDF，落受管
// DERIVED 版本（parent=RAW），预览标签页用 QtPdf 渲染。原件仍是规范来源；
// 转换失败/无 soffice 如实 Failed，UI 降级为「用系统程序打开」。
// ---------------------------------------------------------------------------

void DataImportService::resolveDocumentConverter()
{
  if (m_converterResolved)
    return;
  m_converter = QStandardPaths::findExecutable(QStringLiteral("soffice"));
  if (m_converter.isEmpty())
    m_converter = QStandardPaths::findExecutable(QStringLiteral("libreoffice"));
  m_converterResolved = true;
}

void DataImportService::setDocumentConverterProgram(const QString &program)
{
  m_converter = program;
  m_converterResolved = true;
}

DataImportService::DocPdfState
DataImportService::documentPdfState(const QString &assetId) const
{
  for (const CatalogVersion &v : m_catalog->versionsForAsset(assetId))
    if (v.stage == QLatin1String("DERIVED") &&
        v.fileName.endsWith(QLatin1String(".pdf"), Qt::CaseInsensitive))
      return DocPdfState::Ready;
  if (m_pdfPending.contains(assetId))
    return DocPdfState::Pending;
  if (m_pdfErrors.contains(assetId))
    return DocPdfState::Failed;
  return DocPdfState::None;
}

QString DataImportService::documentPdfPath(const QString &assetId) const
{
  for (const CatalogVersion &v : m_catalog->versionsForAsset(assetId))
    if (v.stage == QLatin1String("DERIVED") &&
        v.fileName.endsWith(QLatin1String(".pdf"), Qt::CaseInsensitive))
      return absolutePathForVersion(v);
  return QString();
}

QString DataImportService::documentPdfError(const QString &assetId) const
{
  return m_pdfErrors.value(assetId);
}

void DataImportService::ensureDocumentPdf(const QString &assetId)
{
  if (documentPdfState(assetId) != DocPdfState::None)
    return;

  const auto failNow = [this, &assetId](const QString &msg) {
    m_pdfErrors.insert(assetId, msg);
    emit documentPdfFailed(assetId, msg);
  };

  resolveDocumentConverter();
  if (m_converter.isEmpty())
    return failNow(tr("找不到 LibreOffice（soffice）——无法生成 PDF 预览"));

  QString rawAbs, rawVersionId;
  for (const CatalogVersion &v : m_catalog->versionsForAsset(assetId))
    if (v.stage == QLatin1String("RAW"))
    {
      rawAbs = absolutePathForVersion(v);
      rawVersionId = v.id;
      break;
    }
  if (rawAbs.isEmpty() || !QFile::exists(rawAbs))
    return failNow(tr("原始文件缺失，无法转换"));

  m_pdfPending.insert(assetId);
  m_pdfQueue.append(assetId);
  startNextDocumentPdf();
}

void DataImportService::startNextDocumentPdf()
{
  if (m_pdfProc || m_pdfQueue.isEmpty())
    return;

  m_pdfCurrent = m_pdfQueue.takeFirst();
  m_pdfCurrentVersionId = m_catalog->nextVersionId();

  QString rawAbs;
  for (const CatalogVersion &v : m_catalog->versionsForAsset(m_pdfCurrent))
    if (v.stage == QLatin1String("RAW"))
    {
      rawAbs = absolutePathForVersion(v);
      m_pdfRawVersionId = v.id;
      break;
    }

  const QString relDir = QStringLiteral("artifacts/derived/%1/%2")
                             .arg(m_pdfCurrent, m_pdfCurrentVersionId);
  const QString outDir = QDir(m_projectDir).absoluteFilePath(relDir);
  if (!QDir().mkpath(outDir))
    return finishDocumentPdf(-1);
  m_pdfOutFile = outDir + QLatin1Char('/') +
                 QFileInfo(rawAbs).completeBaseName() + QStringLiteral(".pdf");

  // 独立 UserInstallation：避开 LibreOffice 单实例 profile 锁。
  const QString profile = QStringLiteral("-env:UserInstallation=file://") +
                          QDir::temp().filePath(QStringLiteral("paleo-lo-profile"));

  m_pdfProc = new QProcess(this);
  connect(m_pdfProc, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
          this, [this](int code, QProcess::ExitStatus status) {
            finishDocumentPdf(status == QProcess::NormalExit ? code : -1);
          });
  m_pdfProc->start(m_converter,
                   {QStringLiteral("--headless"), QStringLiteral("--norestore"),
                    profile, QStringLiteral("--convert-to"), QStringLiteral("pdf"),
                    QStringLiteral("--outdir"), outDir, rawAbs});
}

void DataImportService::finishDocumentPdf(int exitCode)
{
  const QString assetId = m_pdfCurrent;
  QString err;
  bool ok = false;

  if (exitCode == 0 && QFile::exists(m_pdfOutFile))
  {
    QFile f(m_pdfOutFile);
    if (f.open(QIODevice::ReadOnly))
    {
      QCryptographicHash hash(QCryptographicHash::Sha256);
      hash.addData(&f);
      f.close();
      QFile::setPermissions(m_pdfOutFile, QFileDevice::ReadOwner |
                                              QFileDevice::ReadUser |
                                              QFileDevice::ReadGroup |
                                              QFileDevice::ReadOther);

      CatalogVersion d;
      d.id = m_pdfCurrentVersionId;
      d.assetId = assetId;
      d.stage = QStringLiteral("DERIVED");
      d.versionNumber = m_catalog->currentVersion(assetId).versionNumber + 1;
      d.managed = true;
      d.path = QStringLiteral("artifacts/derived/%1/%2/%3")
                   .arg(assetId, m_pdfCurrentVersionId, QFileInfo(m_pdfOutFile).fileName());
      d.sourceUri = m_converter;
      d.sha256 = QString::fromLatin1(hash.result().toHex());
      d.fileName = QFileInfo(m_pdfOutFile).fileName();
      d.parentVersionIds = QStringList{m_pdfRawVersionId};
      d.extra.insert(QStringLiteral("generator"), QStringLiteral("libreoffice"));
      ok = m_catalog->addVersion(d, &err);
    }
    else
      err = f.errorString();
  }
  else
    err = m_pdfProc ? QString::fromLocal8Bit(m_pdfProc->readAllStandardError()).trimmed()
                    : tr("无法创建输出目录");
  if (err.isEmpty() && !ok)
    err = tr("soffice 退出码 %1，未产出 PDF").arg(exitCode);

  if (m_pdfProc)
  {
    m_pdfProc->deleteLater();
    m_pdfProc = nullptr;
  }
  m_pdfCurrent.clear();
  m_pdfPending.remove(assetId);

  if (ok)
    emit documentPdfReady(assetId);
  else
  {
    m_pdfErrors.insert(assetId, err);
    emit documentPdfFailed(assetId, err);
  }
  startNextDocumentPdf();
}

QStringList DataImportService::assets(const QString &type) const
{
  QStringList out;
  for (const CatalogAsset &a : m_catalog->assets())
    if (type.isEmpty() || a.type == type)
      out.append(a.id);
  return out;
}

QString DataImportService::assetSource(const QString &assetId) const
{
  return absolutePath(assetId);
}
