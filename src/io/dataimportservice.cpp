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
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSet>
#include <QStandardPaths>

#include <algorithm>
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

  // 阶段 D 固定辅助规则：isFixedAuxiliaryPath（HZ28-6-1 命名文件）在
  // projectclassifier.cpp——T22 起只锁这一个文件；「参考资料」目录段的
  // 默认「参考」展示归确认表（isDefaultReferencePath），改动成 override。

  QString readFileOrEmpty(const QString &path)
  {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
      return QString();
    return QString::fromUtf8(f.readAll());
  }

  // 未决链接备注（§3 修订）：双候选记两个规范化井名（附实体 id 消歧）；
  // 零匹配记规范化的未匹配井名。
  QString candidatesNote(const DataCatalog *cat, const QStringList &candidateIds)
  {
    QStringList parts;
    for (const QString &id : candidateIds)
    {
      const CatalogEntity e = cat->entityById(id);
      const QString name = e.name.isEmpty() ? id : DataCatalog::normalizeWellName(e.name);
      parts.append(QStringLiteral("%1(%2)").arg(name, id));
    }
    return QStringLiteral("候选: ") + parts.join(QStringLiteral(", "));
  }

  QString unmatchedNameNote(const QStringList &triedNames)
  {
    QStringList norm;
    for (const QString &n : triedNames)
    {
      const QString nn = DataCatalog::normalizeWellName(n);
      if (!nn.isEmpty() && !norm.contains(nn))
        norm.append(nn);
    }
    return QStringLiteral("未匹配井名: ") + norm.join(QStringLiteral(", "));
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
  m_catalogReady = false;
  QString err;
  m_catalogReady = m_catalog->open(dir, &err);
  // T20a：记错误面 + 发信号——catalog 已进入拒绝写入态，后续 mutator
  // 都如实失败。状态栏/消息区接线归 paleomainwindow。
  m_catalogOpenError = m_catalogReady ? QString() : err;
  if (!m_catalogReady)
  {
    qWarning("DataImportService: catalog open failed: %s", qPrintable(err));
    emit catalogOpenFailed(err);
  }
}

DataImportService::WellBind DataImportService::resolveWell(const QString &name) const
{
  WellBind b;
  if (name.trimmed().isEmpty())
  {
    b.unresolved = true;
    return b;
  }
  const QStringList ids =
      catInvoke([&] { return m_catalog->wellsMatchingName(name); });
  if (ids.size() == 1)
  {
    b.entityId = ids.front();
    return b;
  }
  b.unresolved = true;
  b.candidates = ids; // 0 个或 2+ 个
  return b;
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
  // §3：文件名必须是一段合法路径段（import 入口已拦，这里再兜底）。
  const QString relDir =
      DataCatalog::managedPath(QStringLiteral("raw"), assetId, versionId, fileName);
  if (relDir.isEmpty())
  {
    setError(error, QStringLiteral("文件名不是合法路径段: %1").arg(fileName));
    return false;
  }
  const QString relPath = QStringLiteral("artifacts/") + relDir;
  CatalogVersion pending;
  pending.managed = true;
  pending.path = relPath;
  const QString dst = DataCatalog::resolvedVersionPath(m_projectDir, pending);
  if (dst.isEmpty())
  {
    setError(error, QStringLiteral("unsafe managed destination: %1").arg(relPath));
    return false;
  }
  const QDir dir = QFileInfo(dst).absoluteDir();
  if (!dir.exists() && !dir.mkpath(QStringLiteral(".")))
  {
    setError(error, QStringLiteral("cannot create directory %1").arg(dir.absolutePath()));
    return false;
  }
  if (DataCatalog::resolvedVersionPath(m_projectDir, pending).isEmpty())
  {
    setError(error, QStringLiteral("unsafe managed destination: %1").arg(relPath));
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
  // 64KB chunks: a 1MB stack buffer overflows the default Windows thread
  // stack when the import runs on the QTest main thread.
  char buf[64 << 10];
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
  // 兼容签名：dedup 命中也返回（已存在的）资产 id——分辨结局用 importProjectFileEx。
  return importProjectFileEx(sourcePath, options, error).assetId;
}

DataImportService::ImportResult
DataImportService::importProjectFileEx(const QString &sourcePath, QString *error)
{
  return importProjectFileEx(sourcePath, ImportOptions{}, error);
}

DataImportService::ImportResult
DataImportService::importProjectFileEx(const QString &sourcePath, const ImportOptions &options,
                                       QString *error)
{
  QString internalError;
  if (!error)
    error = &internalError;
  else
    error->clear();

  ImportResult res;
  const auto fail = [&](const QString &msg) -> ImportResult {
    setError(error, msg);
    emit importFailed(QString(), sourcePath, msg);
    res.outcome = ImportOutcome::Failed;
    res.message = msg;
    return res;
  };
  const auto done = [&](ImportOutcome outcome, const QString &assetId,
                        const QString &message = QString()) -> ImportResult {
    res.outcome = outcome;
    res.assetId = assetId;
    res.message = message;
    return res;
  };

  if (!m_layers || !m_store)
    return fail(QStringLiteral("import service is not fully wired"));
  if (m_projectDir.isEmpty())
    return fail(QStringLiteral("project dir is not set"));
  // T20a：catalog 拒绝写入时提前如实失败——不等算完 SHA/复制完字节才撞墙。
  if (!m_catalogReady)
    return fail(QStringLiteral("catalog 拒绝写入：%1")
                    .arg(m_catalogOpenError.isEmpty()
                             ? QStringLiteral("catalog 打开失败")
                             : m_catalogOpenError));
  if (sourcePath.isEmpty() || !QFile::exists(sourcePath))
    return fail(QStringLiteral("找不到源文件: %1").arg(sourcePath));

  const QFileInfo fi(sourcePath);

  // §3 路径卫生：文件名本身必须是一段合法路径段——否则受管路径和 displayName
  // 都带病。坏文件名让这一行如实失败，不进 catalog。
  if (!DataCatalog::isSafePathSegment(fi.fileName()))
    return fail(QStringLiteral("文件名不是合法路径段: %1").arg(fi.fileName()));

  // 流式算一遍源文件 SHA-256：dedup 查询与外链入库留底共用这一趟。
  QString shaErr;
  const QString sourceSha = DataCatalog::sha256FileHex(sourcePath, &shaErr);
  if (sourceSha.isEmpty())
    return fail(shaErr.isEmpty() ? QStringLiteral("cannot hash %1").arg(sourcePath) : shaErr);

  // §3 dedup：同一 SHA-256 已在库 → 不新建资产/版本/主关联；只把现在恰好能
  // 匹配到一口井的未决关联补挂上（不建井、不并井）。versionBySha256 只认
  // 文件仍在且重哈希一致的版本——受管文件丢失/被改的旧条目不再冒充命中，
  // 重导据此走全新导入（issue #5）。
  const CatalogVersion existing =
      catInvoke([&] { return m_catalog->versionBySha256(sourceSha); });
  if (!existing.id.isEmpty())
  {
    const CatalogAsset existingAsset =
        catInvoke([&] { return m_catalog->assetById(existing.assetId); });
    if (existingAsset.type == QLatin1String("horizon"))
    {
      const QString horizon = QFileInfo(existing.fileName.isEmpty() ? sourcePath : existing.fileName)
                                  .completeBaseName().toUpper();
      if (isKnownSequenceBoundary(horizon))
      {
        CatalogVersion derived;
        const QVector<CatalogVersion> siblings =
            catInvoke([&] { return m_catalog->versionsForAsset(existing.assetId); });
        for (const CatalogVersion &v : siblings)
          if (v.stage.compare(QStringLiteral("DERIVED"), Qt::CaseInsensitive) == 0 &&
              v.parentVersionIds.contains(existing.id) &&
              QFileInfo::exists(DataCatalog::resolvedVersionPath(m_projectDir, v)))
          {
            derived = v;
            break;
          }
        if (derived.id.isEmpty())
        {
          QFile source(sourcePath);
          if (!source.open(QIODevice::ReadOnly))
            return fail(QStringLiteral("cannot read %1").arg(sourcePath));
          BinnedHorizon binned;
          if (!binHorizon(source.readAll(), &binned, error))
            return fail(error ? *error : QStringLiteral("horizon binning failed"));
          derived.id = catInvoke([&] { return m_catalog->nextVersionId(); });
          derived.assetId = existing.assetId;
          derived.stage = QStringLiteral("DERIVED");
          derived.versionNumber =
              catInvoke([&] { return m_catalog->currentVersion(existing.assetId).versionNumber; }) + 1;
          derived.managed = true;
          derived.fileName = horizon + QStringLiteral(".tif");
          derived.path = QStringLiteral("artifacts/") + DataCatalog::managedPath(
              QStringLiteral("derived"), existing.assetId, derived.id, derived.fileName);
          derived.sourceUri = sourcePath;
          derived.parentVersionIds = QStringList{existing.id};
          derived.extra.insert(QStringLiteral("collisions"), binned.collisions);
          derived.extra.insert(QStringLiteral("rejected"), binned.rejected);
          derived.extra.insert(QStringLiteral("grid_rows"), binned.rows);
          derived.extra.insert(QStringLiteral("grid_cols"), binned.cols);
          derived.extra.insert(QStringLiteral("z_units"), QStringLiteral("ms"));
          derived.extra.insert(QStringLiteral("z_min"), binned.zMin);
          derived.extra.insert(QStringLiteral("z_max"), binned.zMax);
          derived.extra.insert(QStringLiteral("filled_cells"), binned.filledCells);
          const QString dst = DataCatalog::resolvedVersionPath(m_projectDir, derived);
          if (dst.isEmpty())
            return fail(QStringLiteral("unsafe managed destination: %1").arg(derived.path));
          if (!writeHorizonGeoTiff(binned, dst, error))
            return fail(error ? *error : QStringLiteral("horizon raster write failed"));
          QFile::setPermissions(dst, QFileDevice::ReadOwner | QFileDevice::ReadUser |
                                     QFileDevice::ReadGroup | QFileDevice::ReadOther);
          if (!catInvoke([&] { return m_catalog->addVersion(derived, error); }))
            return fail(error ? *error : QStringLiteral("catalog addVersion failed"));
        }
        LayerDeclaration decl;
        decl.layerId = QStringLiteral("horizon.%1").arg(horizon);
        decl.horizon = horizon;
        decl.type = QStringLiteral("raster");
        decl.source = DataCatalog::resolvedVersionPath(m_projectDir, derived);
        decl.group = QStringLiteral("00_Data");
        QString derr;
        if (!catInvoke([&] { return m_layers->declare(decl, &derr); }))
          return fail(derr.isEmpty() ? QStringLiteral("manifest declare failed") : derr);
      }
    }
    QString aerr;
    // 整个补挂过程 marshal 回 GUI 一次执行（内部原生访问 catalog）。
    const int attached = catInvoke(
        [&] { return attachResolvableLinks(existingAsset, sourcePath, &aerr); });
    if (!aerr.isEmpty())
      qWarning("import dedup attach: %s", qPrintable(aerr));
    const QString msg =
        QStringLiteral("字节已在库 · %1")
            .arg(attached > 0 ? QStringLiteral("已补上关联")
                              : QStringLiteral("没有新的关联"));
    qInfo("import: %s — %s", qPrintable(msg), qPrintable(sourcePath));
    res.linkAttached = attached > 0;
    emit imported(existingAsset.type, existing.assetId, QString()); // 聚焦已有资产
    return done(ImportOutcome::AlreadyStored, existing.assetId, msg);
  }
  const QByteArray xmlContent =
      fi.suffix().compare(QStringLiteral("xml"), Qt::CaseInsensitive) == 0
          ? readFileOrEmpty(sourcePath).toUtf8()
          : QByteArray();
  ProjectClassification cls = classifyProjectImport(sourcePath, xmlContent);
  // 确认表「改类型」：forceType 覆盖分类器结果（其余分类字段保留）。
  if (!options.forceType.isEmpty())
    cls.type = options.forceType;
  const QString stem = fi.completeBaseName();

  const QString assetId = catInvoke([&] { return m_catalog->nextAssetId(); });
  const QString versionId = catInvoke([&] { return m_catalog->nextVersionId(); });

  CatalogAsset asset;
  asset.id = assetId;
  asset.type = cls.type;
  asset.format = cls.format;
  asset.displayName = fi.fileName();
  if (!catInvoke([&] { return m_catalog->addAsset(asset, error); }))
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
    // §3：外链也留入库时 SHA-256（上面流式算好的同一趟），打开时照它校验。
    version.sha256 = sourceSha;
  }
  else
  {
    QString relPath, sha;
    if (!storeManagedRaw(sourcePath, assetId, versionId, &relPath, &sha, error))
      return fail(*error);
    version.path = relPath;
    version.sha256 = sha;
  }
  if (!catInvoke([&] { return m_catalog->addVersion(version, error); }))
    return fail(*error);

  // ---- 实体解析与关联（角色沿用已有名字）----
  const QString auxRefRole = QStringLiteral("reference");
  QString manifestLayerId;
  // 阶段 D：HZ28-6-1 命名的井类内容 XML 固定作辅助参考——override 也不理
  // （T22 收窄：「参考资料」目录内其他文件的默认「参考」由确认表给，可改）。
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
    // §3：井口是建井来源，但同文件里同一规范化井名出现两行、或一行同时匹配
    // 两口已有井 → 该行标 unresolved（实体 id 留空、备注记名），不新建不合并；
    // 恰好匹配一口已有井时挂 well_head，不另建井。
    QHash<QString, int> normRowCount;
    for (const WellHeadRecord &r : rows)
      normRowCount[DataCatalog::normalizeWellName(r.name)] += 1;
    for (const WellHeadRecord &r : rows)
    {
      const QString norm = DataCatalog::normalizeWellName(r.name);
      EntityAssetLink link;
      link.entityType = QStringLiteral("well");
      link.assetId = assetId;
      link.role = QStringLiteral("well_head");
      if (normRowCount.value(norm) >= 2)
      {
        link.unresolved = true;
        link.note = QStringLiteral("井口重名: %1").arg(norm);
        if (!catInvoke([&] { return m_catalog->addLink(link, error); }))
          return fail(*error);
        continue;
      }
      const QStringList matches =
          catInvoke([&] { return m_catalog->wellsMatchingName(r.name); });
      if (matches.size() >= 2)
      {
        link.unresolved = true;
        link.note = catInvoke([&] { return candidatesNote(m_catalog, matches); });
        if (!catInvoke([&] { return m_catalog->addLink(link, error); }))
          return fail(*error);
        continue;
      }
      QString wid;
      if (matches.size() == 1)
      {
        wid = matches.front(); // 已有井：直接挂，不另建
      }
      else
      {
        wid = QStringLiteral("well-%1").arg(r.name);
        // id 被别的规范化名占用（罕见）→ 让位于序号 id。
        if (catInvoke([&] { return m_catalog->hasEntity(wid); }))
          wid = catInvoke(
              [&] { return m_catalog->nextEntityId(QStringLiteral("well")); });
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
        if (!catInvoke([&] { return m_catalog->addEntity(w, error); }))
          return fail(*error);
      }
      link.entityId = wid;
      link.isPrimary = true;
      if (!catInvoke([&] { return m_catalog->addLink(link, error); }))
        return fail(*error);
    }
  }
  else if (cls.type == QLatin1String("well_log") && !fixedAux)
  {
    // LAS 先读 ~W 的 WELL；XML 测井/读不到时用文件名主名。
    // （D12：UWI 回退已随 uwi/aliases 字段剥离——井身份只走 name。）
    QString wellName;
    if (cls.format == QLatin1String("las"))
      LasParser::readWellInfo(sourcePath, wellName);
    QStringList tried{wellName};
    WellBind bind = resolveWell(wellName);
    if (bind.unresolved && bind.candidates.isEmpty())
    {
      bind = resolveWell(stem); // A1.Las → A1
      if (bind.unresolved && bind.candidates.isEmpty())
        tried.append(stem);
    }
    // §3 修订：未决也是一条链接——实体 id 留空，备注记候选或未匹配名。
    EntityAssetLink link;
    link.entityType = QStringLiteral("well");
    link.assetId = assetId;
    link.role = QStringLiteral("well_log");
    if (!bind.unresolved)
    {
      link.entityId = bind.entityId;
      link.isPrimary = true;
    }
    else
    {
      link.unresolved = true;
      link.note = bind.candidates.size() >= 2
                      ? catInvoke([&] {
                          return candidatesNote(m_catalog, bind.candidates);
                        })
                      : unmatchedNameNote(tried);
    }
    if (!catInvoke([&] { return m_catalog->addLink(link, error); }))
      return fail(*error);
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
    // §3 修订：每个井名一条链接；未决链接实体 id 留空，备注记候选或未匹配名，
    // 不新建井、不合并、不再挂辅助实体。
    for (const QString &n : names)
    {
      QStringList tried{n};
      WellBind bind = resolveWell(n);
      if (bind.unresolved && bind.candidates.isEmpty() && names.size() == 1)
      {
        bind = resolveWell(stem); // 单井文件的文件名主名回退
        tried.append(stem);
      }
      EntityAssetLink link;
      link.entityType = QStringLiteral("well");
      link.assetId = assetId;
      link.role = role;
      if (!bind.unresolved)
      {
        link.entityId = bind.entityId;
        link.isPrimary = true;
      }
      else
      {
        link.unresolved = true;
        link.note = bind.candidates.size() >= 2
                        ? catInvoke([&] {
                            return candidatesNote(m_catalog, bind.candidates);
                          })
                        : unmatchedNameNote(tried);
      }
      if (!catInvoke([&] { return m_catalog->addLink(link, error); }))
        return fail(*error);
    }
  }
  else if (cls.type == QLatin1String("horizon"))
  {
    const bool known = isKnownSequenceBoundary(stem);
    const QString sbId = QStringLiteral("sb-%1").arg(stem.toUpper());
    if (!catInvoke([&] { return m_catalog->hasEntity(sbId); }))
    {
      CatalogEntity sb;
      sb.id = sbId;
      sb.entityType = QStringLiteral("sequence_boundary");
      sb.name = stem.toUpper();
      if (!known)
        sb.extra.insert(QStringLiteral("pending"), true); // 未决层位，不进编图 chip
      if (!catInvoke([&] { return m_catalog->addEntity(sb, error); }))
        return fail(*error);
    }
    EntityAssetLink link;
    link.entityType = QStringLiteral("sequence_boundary");
    link.entityId = sbId;
    link.assetId = assetId;
    link.role = QStringLiteral("horizon");
    link.isPrimary = true;
    link.unresolved = !known;
    if (!catInvoke([&] { return m_catalog->addLink(link, error); }))
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

      const QString derivedVersionId =
          catInvoke([&] { return m_catalog->nextVersionId(); });
      const QString tifName = stem.toUpper() + QStringLiteral(".tif");
      const QString derivedRel = DataCatalog::managedPath(QStringLiteral("derived"), assetId,
                                                          derivedVersionId, tifName);
      if (derivedRel.isEmpty())
        return fail(QStringLiteral("派生文件名不是合法路径段: %1").arg(tifName));
      const QString relPath = QStringLiteral("artifacts/") + derivedRel;
      CatalogVersion pending;
      pending.managed = true;
      pending.path = relPath;
      const QString tifPath = DataCatalog::resolvedVersionPath(m_projectDir, pending);
      if (tifPath.isEmpty())
        return fail(QStringLiteral("unsafe managed destination: %1").arg(relPath));
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
      derived.extra.insert(QStringLiteral("rejected"), binned.rejected);
      derived.extra.insert(QStringLiteral("grid_rows"), binned.rows);
      derived.extra.insert(QStringLiteral("grid_cols"), binned.cols);
      derived.extra.insert(QStringLiteral("z_units"), QStringLiteral("ms"));
      derived.extra.insert(QStringLiteral("z_min"), binned.zMin);
      derived.extra.insert(QStringLiteral("z_max"), binned.zMax);
      derived.extra.insert(QStringLiteral("filled_cells"), binned.filledCells);
      if (!catInvoke([&] { return m_catalog->addVersion(derived, error); }))
        return fail(*error);

      // 图层清单只登记要画的结果（§2）：北向上时间栅格 + 局部测网 CRS。
      LayerDeclaration decl;
      decl.layerId = QStringLiteral("horizon.%1").arg(stem.toUpper());
      decl.horizon = stem.toUpper();
      decl.type = QStringLiteral("raster");
      decl.source = tifPath;
      decl.group = QStringLiteral("00_Data");
      QString derr;
      // declare 写 layer manifest（sqlite）——marshal 回 GUI 线程执行。
      if (!catInvoke([&] { return m_layers->declare(decl, &derr); }))
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
    if (!catInvoke([&] { return m_catalog->hasEntity(surveyId); }))
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
      if (!catInvoke([&] { return m_catalog->addEntity(s, error); }))
        return fail(*error);
    }
    EntityAssetLink link;
    link.entityType = QStringLiteral("seismic_survey");
    link.entityId = surveyId;
    link.assetId = assetId;
    link.role = QStringLiteral("seismic_volume");
    link.isPrimary = true;
    if (!catInvoke([&] { return m_catalog->addLink(link, error); }))
      return fail(*error);
  }
  else
  {
    // document / image_reference / geojson / unknown / 参考资料 XML：辅助实体 + reference。
    const QString auxId =
        catInvoke([&] { return m_catalog->nextEntityId(QStringLiteral("aux")); });
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
    if (!catInvoke([&] { return m_catalog->addEntity(aux, error); }))
      return fail(*error);
    EntityAssetLink link;
    link.entityType = QStringLiteral("auxiliary");
    link.entityId = auxId;
    link.assetId = assetId;
    link.role = auxRefRole;
    link.isPrimary = true;
    if (!catInvoke([&] { return m_catalog->addLink(link, error); }))
      return fail(*error);
  }

  emit imported(cls.type, assetId, manifestLayerId);
  return done(ImportOutcome::Imported, assetId);
}

// ---------------------------------------------------------------------------
// §3/§4「导入工区文件夹」：递归枚举普通文件（分类依赖 井位/井分层/时深/层位
// 路径段），两阶段处理——全部 well_head 行先走（井建齐），其余文件再对已齐
// 的井集解析。单行失败只落行、不中断；逃出所选根目录的符号链接与非普通文
// 件标 Skipped。dedup（AlreadyStored）记 Imported，message 留「字节已在库」。
// ---------------------------------------------------------------------------
namespace
{
  struct FolderCand
  {
    QString path;
    QString canon;          // 枚举时刻的 canonicalFilePath——TOCTOU 复核基准
    QString classifiedType; // 分类器原类型（覆盖前的）；阶段归属看生效类型
  };

  void appendFolderSkip( QVector<DataImportService::FolderRowResult> *skipped,
                         const QString &path, const QString &msg )
  {
    DataImportService::FolderRowResult row;
    row.path = path;
    row.classifiedType = classifyProjectPath( path ).type;
    row.outcome = DataImportService::FolderRowResult::Outcome::Skipped;
    row.message = msg;
    skipped->append( row );
  }

  // 行的生效类型：覆盖表里有合法类型用覆盖，否则用分类器结果。
  // 非法 override（不在分类器词表里的字符串）忽略——不落进资产类型。
  QString effectiveFolderType(const FolderCand &c,
                              const QMap<QString, QString> &overrides)
  {
    const QString o = overrides.value(c.path);
    return isClassifierType(o) ? o : c.classifiedType;
  }

  // 与 importFolder/previewFolder 共用的枚举：校验目录、守卫工程子树、
  // 递归收普通文件并按分类器归类（.xml 看内容判定）。
  bool collectFolderCandidates( const QString &dirPath, const QString &projectDir,
                                QVector<FolderCand> *candidates,
                                QVector<DataImportService::FolderRowResult> *skipped,
                                QString *error )
  {
    candidates->clear();
    skipped->clear();
    const QFileInfo dirInfo( dirPath );
    if ( dirPath.isEmpty() || !dirInfo.isDir() )
    {
      setError( error, QStringLiteral( "找不到目录: %1" ).arg( dirPath ) );
      return false;
    }
    const QString rootCanon = dirInfo.canonicalFilePath();
    if ( rootCanon.isEmpty() )
    {
      setError( error, QStringLiteral( "无法解析目录: %1" ).arg( dirPath ) );
      return false;
    }
    const QString rootPrefix = rootCanon + QLatin1Char( '/' );
    // 工程目录自身（或其内部目录）不能当导入源——不能把 catalog/artifacts 扫回来。
    const QString projectCanon = QFileInfo( projectDir ).canonicalFilePath();
    const QString projectPrefix =
        projectCanon.isEmpty() ? QString() : projectCanon + QLatin1Char( '/' );
    if ( !projectCanon.isEmpty() &&
         ( rootCanon == projectCanon || rootCanon.startsWith( projectPrefix ) ) )
    {
      setError( error, QStringLiteral( "不能把工程目录自身选作导入源: %1" ).arg( dirPath ) );
      return false;
    }

    // 迭代器不带 FollowSymlinks：目录符号链接天然不下钻；文件符号链接用
    // canonical 判定是否逃出所选根目录（落在根内的按普通文件处理，link 路径
    // 作为 sourceUri 留底）。不带 Hidden：.preview_cache 之类不进表。
    // AllEntries|System：fifo/socket/悬空链接也要收进来——它们各落一行 Skipped。
    QDirIterator it( dirInfo.absoluteFilePath(),
                     QDir::AllEntries | QDir::System | QDir::NoDotAndDotDot,
                     QDirIterator::Subdirectories );
    while ( it.hasNext() )
    {
      const QString path = it.next();
      const QFileInfo fi = it.fileInfo();
      const QString canon = fi.canonicalFilePath();
      // 被选目录包住工程目录时：工程产物子树不是源数据，跳过且不出行。
      if ( !projectPrefix.isEmpty() && !canon.isEmpty() && canon.startsWith( projectPrefix ) )
        continue;

      if ( fi.isSymLink() &&
           ( canon.isEmpty() || ( !canon.startsWith( rootPrefix ) && canon != rootCanon ) ) )
      {
        appendFolderSkip( skipped, path,
                          canon.isEmpty() ? QStringLiteral( "悬空符号链接，已跳过" )
                                          : QStringLiteral( "符号链接指向所选目录之外，已跳过" ) );
        continue;
      }
      if ( fi.isDir() )
        continue; // 目录只用来下钻，自身不成行
      if ( !fi.isFile() )
      {
        appendFolderSkip( skipped, path, QStringLiteral( "不是普通文件，已跳过" ) );
        continue;
      }

      const QString candCanon = fi.canonicalFilePath();

      // 与 importProjectFileEx 同一分类口径：.xml 要看内容判定。
      QByteArray xml;
      if ( QFileInfo( path ).suffix().compare( QLatin1String( "xml" ), Qt::CaseInsensitive ) == 0 )
      {
        // T33 符号链接 TOCTOU 复核：上面 containment 判定用的是枚举时刻
        // stat；读字节前重取 canonical——目标被改指向（或已非普通文件）就
        // 如实跳过，不读逃出根目录的内容。
        const QFileInfo recheck( path );
        if ( !recheck.isFile() || recheck.canonicalFilePath() != candCanon )
        {
          appendFolderSkip( skipped, path,
                            QStringLiteral( "符号链接目标在枚举后已变化，已跳过" ) );
          continue;
        }
        xml = readFileOrEmpty( path ).toUtf8();
      }
      const ProjectClassification cls = classifyProjectImport( path, xml );
      FolderCand c;
      c.path = path;
      c.canon = candCanon;
      c.classifiedType = cls.type;
      candidates->append( c );
    }

    if ( candidates->isEmpty() && skipped->isEmpty() )
    {
      setError( error, QStringLiteral( "目录里没有可导入的文件: %1" ).arg( dirPath ) );
      return false;
    }
    return true;
  }

  // 与 importFolder 相同的行序：阶段 1 全部 well_head（井建齐）、阶段 2 其余，
  // 各阶段内按路径排序——previewFolder 与 importFolder 必须用同一序，确认表
  // 才能按行索引对齐预览行与结果行。
  // D5：阶段划分按「生效类型」（覆盖后）——用户在确认表把一行改成
  // well_head，它就回阶段 1，井建得够早，排前的阶段 2 行仍能挂上。
  QVector<FolderCand> orderFolderCandidates( const QVector<FolderCand> &candidates,
                                             const QMap<QString, QString> &overrides = {} )
  {
    QVector<FolderCand> ordered;
    for ( int phase = 0; phase < 2; ++phase )
    {
      QVector<FolderCand> bucket;
      for ( const FolderCand &c : candidates )
      {
        const bool phase0 = effectiveFolderType(c, overrides) ==
                                QLatin1String("well_head") &&
                            !isFixedAuxiliaryPath(c.path);
        if ( ( phase == 0 ) == phase0 )
          bucket.append( c );
      }
      std::sort( bucket.begin(), bucket.end(),
                 []( const FolderCand &a, const FolderCand &b ) { return a.path < b.path; } );
      ordered += bucket;
    }
    return ordered;
  }
} // namespace

QVector<DataImportService::FolderPreviewRow>
DataImportService::previewFolder(const QString &dirPath, QString *error)
{
  QVector<FolderPreviewRow> rows;
  if (error)
    error->clear();
  QVector<FolderCand> cands;
  QVector<FolderRowResult> skipped;
  if (!collectFolderCandidates(dirPath, m_projectDir, &cands, &skipped, error))
    return rows;
  for (const FolderCand &c : orderFolderCandidates(cands))
  {
    FolderPreviewRow r;
    r.path = c.path;
    r.classifiedType = c.classifiedType;
    rows.append(r);
  }
  std::sort(skipped.begin(), skipped.end(),
            [](const FolderRowResult &a, const FolderRowResult &b) {
              return a.path < b.path;
            });
  for (const FolderRowResult &s : skipped)
  {
    FolderPreviewRow r;
    r.path = s.path;
    r.classifiedType = s.classifiedType;
    r.skipped = true;
    r.skipReason = s.message;
    rows.append(r);
  }
  return rows;
}

QVector<DataImportService::FolderRowResult>
DataImportService::importFolder(const QString &dirPath, QString *error)
{
  return importFolder(dirPath, error, QMap<QString, QString>{}, {});
}

QVector<DataImportService::FolderRowResult>
DataImportService::importFolder(
    const QString &dirPath, QString *error,
    const QMap<QString, QString> &typeOverrides,
    const std::function<bool(int, int, const QString &)> &progress)
{
  QVector<FolderRowResult> rows;
  if (error)
    error->clear(); // 成功路径不写 error——先清掉调用方复用的旧值
  if (!m_layers || !m_store)
  {
    setError(error, QStringLiteral("import service is not fully wired"));
    return rows;
  }
  if (m_projectDir.isEmpty())
  {
    setError(error, QStringLiteral("project dir is not set"));
    return rows;
  }

  QVector<FolderCand> candidates;
  QVector<FolderRowResult> skipped;
  if (!collectFolderCandidates(dirPath, m_projectDir, &candidates, &skipped, error))
    return rows;

  // 阶段 1：全部 well_head（井建齐）；阶段 2：其余文件对已齐的井集解析。
  // D5：阶段归属按生效类型（覆盖后）算——改成 well_head 的行回阶段 1。
  const QVector<FolderCand> ordered = orderFolderCandidates(candidates, typeOverrides);

  // T33/audit row 37：整个文件夹导入并成一个落盘批次——每行 ~5 次全量
  // JSON 序列化收敛成一次 save() + 一次 changed()；中途崩溃不留
  // 「资产已落盘、链接没落盘」的半截 catalog。
  // BatchSave 的 RAII 摸 catalog 私有态——构造/析构都 marshal 回 GUI 线程；
  // 期间的逐行 catalog 操作经 catInvoke 排队执行，save() 被批次挂起不变。
  auto *batch =
      catInvoke([&] { return new DataCatalog::BatchSave(m_catalog); });

  bool cancelled = false;
  int doneCount = 0;
  for (const FolderCand &c : ordered)
  {
    // T33 符号链接 TOCTOU 终验：真正读字节的是 importProjectFileEx 里的
    // open/hash/copy——入它之前再核一次：路径仍解析到枚举时判定的同一
    // canonical、仍是普通文件；被改指向的链接/被换掉的文件如实跳过。
    const QFileInfo now( c.path );
    if ( !now.isFile() || now.canonicalFilePath() != c.canon )
    {
      FolderRowResult row;
      row.path = c.path;
      row.classifiedType = effectiveFolderType(c, typeOverrides);
      row.outcome = FolderRowResult::Outcome::Skipped;
      row.message = QStringLiteral( "文件在导入前已变化（符号链接改指向或不再是普通文件），已跳过" );
      rows.append( row );
    }
    else
      rows.append(folderRowFor(c.path, c.classifiedType,
                               effectiveFolderType(c, typeOverrides)));
    ++doneCount;
    if (progress && !progress(doneCount, static_cast<int>(ordered.size()), c.path))
    {
      cancelled = true;
      break; // 协作取消：已处理的行保留，未处理的不再动
    }
  }

  std::sort(skipped.begin(), skipped.end(),
            [](const FolderRowResult &a, const FolderRowResult &b) {
              return a.path < b.path;
            });
  rows += skipped;

  // 批次结算：析构即 flush——marshal 回 GUI 线程销毁（落盘失败如实写 error；
  // 行里的 Imported 结局不变——内存态已是入库态，磁盘没写成功要 surfaced）。
  QString berr;
  const bool flushed = catInvoke([&] {
    const bool ok = batch->flush(&berr);
    delete batch;
    return ok;
  });
  if (!flushed)
  {
    qWarning("importFolder: catalog batch save failed: %s", qPrintable(berr));
    setError(error, berr.isEmpty() ? QStringLiteral("catalog batch save failed") : berr);
  }
  if (cancelled)
    setError(error, QStringLiteral("已取消（已入库的行保留）"));
  return rows;
}

// ---------------------------------------------------------------------------
// T22：单行导入 → 确认表行结果（importFolder 每行与「重试」共用同一口径）。
// FolderRowResult::classifiedType 记生效类型（覆盖后），与确认表显示一致。
// ---------------------------------------------------------------------------
DataImportService::FolderRowResult
DataImportService::folderRowFor(const QString &path, const QString &classifiedType,
                                const QString &effectiveType)
{
  FolderRowResult row;
  row.path = path;
  row.classifiedType = effectiveType;
  ImportOptions opts;
  if (effectiveType != classifiedType)
    opts.forceType = effectiveType; // 只在生效类型不同于分类器结果时下传
  QString ferr;
  const ImportResult res = importProjectFileEx(path, opts, &ferr);
  row.message = res.message.isEmpty() ? ferr : res.message;
  if (res.outcome == ImportOutcome::Failed || res.assetId.isEmpty())
  {
    row.outcome = FolderRowResult::Outcome::Failed;
    if (row.message.isEmpty())
      row.message = QStringLiteral("导入失败");
    return row;
  }

  // 确认表口径（autoplan-dx）：未决=资产已存但实体 id 全空（没有任何已决
  // 关联——被同批新主关联降级的旧关联实体 id 仍非空，不算未决）；入库=写
  // 成了主关联；dedup 命中也记 Imported（message 已是「字节已在库」）。
  QStringList names;
  QStringList notes;
  int resolved = 0;
  catInvoke([&] {
    for (const EntityAssetLink &l : m_catalog->linksForAsset(res.assetId))
    {
      if (l.unresolved)
      {
        if (!l.note.isEmpty() && !notes.contains(l.note))
          notes.append(l.note);
        continue;
      }
      ++resolved;
      const CatalogEntity e = m_catalog->entityById(l.entityId);
      const QString n = e.name.isEmpty() ? l.entityId : e.name;
      if (!n.isEmpty() && !names.contains(n))
        names.append(n);
    }
  });
  row.entityName = names.join(QStringLiteral(", "));
  row.outcome = res.outcome == ImportOutcome::Imported && resolved == 0
                    ? FolderRowResult::Outcome::Unresolved
                    : FolderRowResult::Outcome::Imported;
  if (!notes.isEmpty())
    row.message = row.message.isEmpty()
                      ? notes.join(QStringLiteral("；"))
                      : row.message + QStringLiteral("；") +
                            notes.join(QStringLiteral("；"));
  return row;
}

DataImportService::FolderRowResult
DataImportService::importFolderRow(const QString &sourcePath, const QString &forceType,
                                   QString *error)
{
  if (error)
    error->clear();
  // 与 collectFolderCandidates 同一分类口径：.xml 要看内容判定。
  const QByteArray xml =
      QFileInfo(sourcePath).suffix().compare(QLatin1String("xml"), Qt::CaseInsensitive) == 0
          ? readFileOrEmpty(sourcePath).toUtf8()
          : QByteArray();
  const QString classified = classifyProjectImport(sourcePath, xml).type;
  const QString eff = isClassifierType(forceType) ? forceType : classified;
  const FolderRowResult row = folderRowFor(sourcePath, classified, eff);
  if (row.outcome == FolderRowResult::Outcome::Failed)
    setError(error, row.message); // 成功路径不写 error（同 importFolder）
  return row;
}

// ---------------------------------------------------------------------------
// §3 dedup 补挂：同一 SHA-256 再导入时不新建资产/版本；只把「现在恰好匹配
// 一口井」的未决链接挂上去。名称来源与原导入各分支一致——SHA-256 相同 ⇒
// 解析出的井名与顺序一致，未决链接按 links() 序与之一一配对。不建井、不并井。
// ---------------------------------------------------------------------------
int DataImportService::attachResolvableLinks(const CatalogAsset &asset,
                                             const QString &sourcePath, QString *error)
{
  if (asset.id.isEmpty())
    return 0;
  const QString stem = QFileInfo(sourcePath).completeBaseName();

  // 每条（同资产、well 型）链接按创建序对应一组按序尝试的井名。
  QVector<QStringList> namesPerLink;
  if (asset.type == QLatin1String("well_log"))
  {
    QString wellName;
    if (asset.format == QLatin1String("las"))
      LasParser::readWellInfo(sourcePath, wellName);
    namesPerLink.append({wellName, stem}); // ~W WELL → 文件名主名（D12：UWI 层已删）
  }
  else if (asset.type == QLatin1String("well_stratification"))
  {
    QFile f(sourcePath);
    if (!f.open(QIODevice::ReadOnly))
    {
      setError(error, QStringLiteral("cannot read %1").arg(sourcePath));
      return 0;
    }
    QStringList names;
    for (const WellTopRecord &t : parseWellTopsText(f.readAll()))
      if (!names.contains(t.wellName))
        names.append(t.wellName);
    for (const QString &n : names)
      namesPerLink.append(names.size() == 1 ? QStringList{n, stem} : QStringList{n});
  }
  else if (asset.type == QLatin1String("time_depth"))
  {
    QFile f(sourcePath);
    if (!f.open(QIODevice::ReadOnly))
    {
      setError(error, QStringLiteral("cannot read %1").arg(sourcePath));
      return 0;
    }
    const TimeDepthTable td = parseTimeDepthText(f.readAll());
    namesPerLink.append({td.wellName.isEmpty() ? stem : td.wellName, stem});
  }
  else if (asset.type == QLatin1String("well_head"))
  {
    QFile f(sourcePath);
    if (!f.open(QIODevice::ReadOnly))
    {
      setError(error, QStringLiteral("cannot read %1").arg(sourcePath));
      return 0;
    }
    const QVector<WellHeadRecord> rows = parseWellHeadText(f.readAll());
    QHash<QString, int> normRowCount;
    for (const WellHeadRecord &r : rows)
      normRowCount[DataCatalog::normalizeWellName(r.name)] += 1;
    for (const WellHeadRecord &r : rows)
      // 文件内规范化重名的行在原导入就是「井口重名」未决——仍歧义，不补挂。
      namesPerLink.append(normRowCount.value(DataCatalog::normalizeWellName(r.name)) >= 2
                              ? QStringList{}
                              : QStringList{r.name});
  }
  else
    return 0; // seismic/horizon/auxiliary：实体在入库时已确定，dedup 不补井关联。

  const QVector<EntityAssetLink> links = m_catalog->links(); // 快照（attachLink 不增删）
  // T33：一次 dedup 可能连挂多条链接——并入批次（嵌套在 importFolder 的
  // 外层批次里也安全，深度计数）。
  DataCatalog::BatchSave batch(m_catalog);
  int nameIdx = 0, attached = 0;
  for (int i = 0; i < links.size() && nameIdx < namesPerLink.size(); ++i)
  {
    const EntityAssetLink &l = links.at(i);
    if (l.assetId != asset.id || l.entityType != QLatin1String("well"))
      continue;
    const QStringList tried = namesPerLink.at(nameIdx++);
    if (!l.unresolved)
      continue;
    QString target;
    for (const QString &n : tried)
    {
      const QStringList ids = m_catalog->wellsMatchingName(n);
      if (ids.size() == 1)
      {
        target = ids.front();
        break;
      }
      if (!ids.isEmpty())
        break; // 2+ 候选仍不决——与导入同一判据（不试下一个名字）
    }
    if (target.isEmpty())
      continue;
    // 该 (well, role) 已有已决主关联 → 没有新的关联可补。
    bool hasPrimary = false;
    for (const EntityAssetLink &o : m_catalog->linksForEntity(target))
      if (o.role == l.role && o.isPrimary && !o.unresolved)
        hasPrimary = true;
    if (hasPrimary)
      continue;
    QString aerr;
    if (!m_catalog->attachLink(i, target, &aerr))
    {
      setError(error, aerr);
      continue;
    }
    ++attached;
  }
  // 批次结算：落盘失败如实透给调用方（dedup 路径只记 qWarning，不中断）。
  QString berr;
  if (!batch.flush(&berr) && !berr.isEmpty())
    setError(error, berr);
  return attached;
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
  return DataCatalog::resolvedVersionPath(m_projectDir, v);
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
