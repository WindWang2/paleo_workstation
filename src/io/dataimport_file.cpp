// 层：数据
// 方向57：从 dataimportservice.cpp 按格式族析出。公共 API（dataimportservice.h）
// 零改动；跨族共享辅助（setError/readFileOrEmpty/FamilyContext）经
// dataimport_internal.h；importOneFile 分支体族函数亦声明于该头。
#include "dataimportservice.h"

#include "lascache.h"
#include "rasterpyramid.h"
#include "segyindexstore.h"
#include "shacache.h"

#include "../catalog/datacatalog.h"
#include "../metadata/layermanifest.h"
#include "../metadata/paleoprojectstore.h"
#include "../domain/arearules.h"
#include "horizonbinner.h"
#include "ingestplan.h"
#include "lasparser.h"
#include "welllogread.h" // 方向44：井名提取分派
#include "../domain/projectclassifier.h"
#include "segyreader.h"
#include "wellcompositexml.h"
#include "wellfileparsers.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QThread>
#include <QUuid>

#include <cpl_conv.h>
#include <cpl_error.h>
#include <gdal.h>
#include <QStandardPaths>
#include <QUrl>

#include "../metadata/atomicfile.h"

#include <algorithm>
#include <cstdio>
#include "dataimport_internal.h"

using paleo::dataimport_detail::setError;
using paleo::dataimport_detail::readFileOrEmpty;


void DataImportService::produceFile(ImportSession &s, const QString &sourcePath,
                                    const ImportOptions &options)
{
  // C 包三段式入口（docs/DATA_FABRIC_ADOPTION.md）：单文件也先立 1 项 plan——
  // 同一 buildIngestPlan（分类/身份/plan 期 dedup 标记全走），重复项的
  // duplicateOfVersionId 如实标上。显式单文件导入的语义是「仍导入」：这里不走
  // 执行器的 skip/幂等分支，同字节结局由内部 dedup 消化（AlreadyStored+补挂，
  // 与既有测试同口径）。plan 不适用时（服务未接线/文件不存在/目录源）原路进
  // importOneFile——错误与 importFailed 信号口径不变。
  s.hasFileResult = true;
  s.fileSourcePath = sourcePath;
  QString err;
  if (s.cat && s.catalogReady && !sourcePath.isEmpty() && QFileInfo(sourcePath).isFile())
  {
    const IngestPlan plan = planFor(s, sourcePath);
    if (!plan.items.isEmpty())
    {
      const PlannedItem &item = plan.items.constFirst();
      ImportOptions eff = options;
      if (eff.forceType.isEmpty())
        eff.forceType = item.type; // 生效类型取 plan 分类（与内部同口径）
      s.fileResult = importOneFile(s, item.path, eff, &err);
      s.error = err;
      return;
    }
  }
  s.fileResult = importOneFile(s, sourcePath, options, &err);
  s.error = err;
}

DataImportService::ImportResult
DataImportService::importProjectFileEx(const QString &sourcePath, const ImportOptions &options,
                                       QString *error)
{
  if (error)
    error->clear();
  const auto s = runInline(
      [&](ImportSession &sess) { produceFile(sess, sourcePath, options); });
  if (!s)
  {
    setError(error, offThreadError());
    return {};
  }
  setError(error, s->error);
  return s->fileResult;
}
// 单文件导入实体——plan 化前的 importProjectFileEx 主体：分类 → dedup →
// 受管 RAW 复制/外链 → 实体解析 → 关联，语义原样未动。
DataImportService::ImportResult
DataImportService::importOneFile(ImportSession &s, const QString &sourcePath,
                                 const ImportOptions &options, QString *error)
{
  QString internalError;
  if (!error)
    error = &internalError;
  else
    error->clear();

  // 审计 02 M-8：本函数在 produce 线程跑——只碰 session（staging 副本 +
  // 暂存根 + 事件表）。信号记成事件，owner 线程提交成功后按原序发射。
  DataCatalog *const cat = s.cat.get();
  ImportResult res;
  const auto fail = [&](const QString &msg) -> ImportResult {
    setError(error, msg);
    s.recordImportFailed(QString(), sourcePath, msg);
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

  if (!s.wired)
    return fail(QStringLiteral("import service is not fully wired"));
  if (s.projectDir.isEmpty())
    return fail(QStringLiteral("project dir is not set"));
  // T20a：catalog 拒绝写入时提前如实失败——不等算完 SHA/复制完字节才撞墙。
  if (!s.catalogReady)
    return fail(QStringLiteral("catalog 拒绝写入：%1")
                    .arg(s.catalogOpenError.isEmpty()
                             ? QStringLiteral("catalog 打开失败")
                             : s.catalogOpenError));
  // 单写实例降级（T4）：锁在别的实例手里——受管字节还没开始复制就拒，
  // 不留「文件拷进 artifacts/ 而 catalog 拒登记」的孤儿。
  if (cat->isLockedReadOnly())
    return fail(QStringLiteral("工程目录被另一个实例锁定——本实例只读，导入被拒绝"));
  if (sourcePath.isEmpty() || !QFile::exists(sourcePath))
    return fail(QStringLiteral("找不到源文件: %1").arg(sourcePath));

  const QFileInfo fi(sourcePath);

  // §3 路径卫生：文件名本身必须是一段合法路径段——否则受管路径和 displayName
  // 都带病。坏文件名让这一行如实失败，不进 catalog。
  if (!DataCatalog::isSafePathSegment(fi.fileName()))
    return fail(QStringLiteral("文件名不是合法路径段: %1").arg(fi.fileName()));

  // 流式算一遍源文件 SHA-256：dedup 查询与外链入库留底共用这一趟。
  QString shaErr;
  const QString sourceSha = ShaCache::shared().sha256Hex(sourcePath, &shaErr); // D7.7
  if (sourceSha.isEmpty())
    return fail(shaErr.isEmpty() ? QStringLiteral("cannot hash %1").arg(sourcePath) : shaErr);

  // §3 dedup：同一 SHA-256 已在库 → 不新建资产/版本/主关联；只把现在恰好能
  // 匹配到一口井的未决关联补挂上（不建井、不并井）。versionBySha256 只认
  // 文件仍在且重哈希一致的版本——受管文件丢失/被改的旧条目不再冒充命中，
  // 重导据此走全新导入（issue #5）。
  const CatalogVersion existing =
      cat->versionBySha256(sourceSha);
  if (!existing.id.isEmpty())
  {
    const CatalogAsset existingAsset =
        cat->assetById(existing.assetId);
    if (existingAsset.type == QLatin1String("horizon"))
    {
      const QString rerr = paleo::dataimport_detail::dedupHorizonRederive(
          cat, s, existing, sourcePath, error);
      if (!rerr.isEmpty())
        return fail(rerr);
    }
    QString aerr;
    // 补挂在 staging 副本上执行（attachLink 记进 journal，提交时重放）。
    const int attached = attachResolvableLinks(cat, existingAsset, sourcePath, &aerr);
    if (!aerr.isEmpty())
      qWarning("import dedup attach: %s", qPrintable(aerr));
    const QString msg =
        QStringLiteral("字节已在库 · %1")
            .arg(attached > 0 ? QStringLiteral("已补上关联")
                              : QStringLiteral("没有新的关联"));
    qInfo("import: %s — %s", qPrintable(msg), qPrintable(sourcePath));
    res.linkAttached = attached > 0;
    s.recordImported(existingAsset.type, existing.assetId, QString()); // 聚焦已有资产
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

  const QString assetId = cat->nextAssetId();
  const QString versionId = cat->nextVersionId();

  CatalogAsset asset;
  asset.id = assetId;
  asset.type = cls.type;
  asset.format = cls.format;
  asset.displayName = fi.fileName();
  if (!cat->addAsset(asset, error))
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
    if (!storeManagedRaw(s, sourcePath, assetId, versionId, &relPath, &sha, error))
      return fail(*error);
    version.path = relPath;
    version.sha256 = sha;
  }
  if (!cat->addVersion(version, error))
    return fail(*error);

  // ---- 实体解析与关联（角色沿用已有名字）----
  QString manifestLayerId;
  // 阶段 D：HZ28-6-1 命名的井类内容 XML 固定作辅助参考——override 也不理
  // （T22 收窄：「参考资料」目录内其他文件的默认「参考」由确认表给，可改）。
  // 阶段 D 固定辅助规则：isFixedAuxiliaryPath（HZ28-6-1 命名文件）在
  // projectclassifier.cpp——T22 起只锁这一个文件；「参考资料」目录段的
  // 默认「参考」展示归确认表（isDefaultReferencePath），改动成 override。
  const bool fixedAux = isFixedAuxiliaryPath(sourcePath) &&
                        (cls.type == QLatin1String("well_head") ||
                         cls.type == QLatin1String("well_log"));

  paleo::dataimport_detail::FamilyContext ctx;
  ctx.cat = cat;
  ctx.s = &s;
  ctx.sourcePath = sourcePath;
  ctx.stem = stem;
  ctx.assetId = assetId;
  ctx.versionId = versionId;
  ctx.cls = cls;
  ctx.fi = fi;
  ctx.external = external;
  ctx.version = version;
  ctx.error = error;
  QString branchErr;
  if (cls.type == QLatin1String("well_head") && !fixedAux)
    branchErr = paleo::dataimport_detail::importWellHeadFamily(ctx);
  else if (cls.type == QLatin1String("well_log") && !fixedAux)
    branchErr = paleo::dataimport_detail::importWellLogFamily(ctx);
  else if (cls.type == QLatin1String("well_stratification") ||
           cls.type == QLatin1String("time_depth"))
    branchErr = paleo::dataimport_detail::importWellTopsFamily(ctx);
  else if (cls.type == QLatin1String("well_deviation"))
    branchErr = paleo::dataimport_detail::importDeviationFamily(ctx);
  else if (cls.type == QLatin1String("horizon"))
    branchErr = paleo::dataimport_detail::importHorizonFamily(ctx);
  else if (cls.type == QLatin1String("seismic"))
    branchErr = paleo::dataimport_detail::importSeismicFamily(ctx);
  else
    branchErr = paleo::dataimport_detail::importAuxReferenceFamily(ctx);
  if (!branchErr.isEmpty())
    return fail(branchErr);
  manifestLayerId = ctx.manifestLayerId;


  // B3（wave/deepen-perf）：栅格资产（image_reference 的 tif/png/jpg）入库即
  // Lazy ensure 瓦片金字塔——近零开销（只铺目录/状态表），消费侧首次取瓦片
  // 才付构建成本。受管副本与外链源都适用：金字塔落工程缓存根，不动源文件。
  if (cls.type == QLatin1String("image_reference"))
  {
    const QString ext = fi.suffix().toLower();
    if (ext == QLatin1String("tif") || ext == QLatin1String("tiff") ||
        ext == QLatin1String("png") || ext == QLatin1String("jpg") ||
        ext == QLatin1String("jpeg"))
    {
      // 受管副本取提交后的最终位置；ensure 在 owner 线程提交成功后执行。
      const QString rasterAbs =
          external ? version.path
                   : DataCatalog::resolvedVersionPath(s.projectDir, version);
      if (!rasterAbs.isEmpty())
        s.pyramidTargets.append(rasterAbs);
    }
  }

  s.recordImported(cls.type, assetId, manifestLayerId);
  return done(ImportOutcome::Imported, assetId);
}

// ---------------------------------------------------------------------------
int DataImportService::attachResolvableLinks(DataCatalog *cat, const CatalogAsset &asset,
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
    if (asset.format == QLatin1String("las") || asset.format == QLatin1String("dlis") ||
        asset.format == QLatin1String("lis"))
      WellLogRead::readWellInfo(sourcePath, wellName); // 失败如实：井名空走候选
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

  const QVector<EntityAssetLink> links = cat->links(); // 快照（attachLink 不增删）
  // T33：一次 dedup 可能连挂多条链接——并入批次（嵌套在 importFolder 的
  // 外层批次里也安全，深度计数）。
  DataCatalog::BatchSave batch(cat);
  int nameIdx = 0, attached = 0;
  for (int i = 0; i < links.size() && nameIdx < namesPerLink.size(); ++i)
  {
    const EntityAssetLink &l = links.at(i);
    if (l.assetId != asset.id || l.entityType != QLatin1String("well"))
      continue;
    const QStringList &tried = namesPerLink.at(nameIdx++);
    if (!l.unresolved)
      continue;
    QString target;
    for (const QString &n : tried)
    {
      const QStringList ids = cat->wellsMatchingName(n);
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
    for (const EntityAssetLink &o : cat->linksForEntity(target))
      if (o.role == l.role && o.isPrimary && !o.unresolved)
        hasPrimary = true;
    if (hasPrimary)
      continue;
    QString aerr;
    if (!cat->attachLink(i, target, &aerr))
    {
      setError(error, aerr);
      continue;
    }
    ++attached;
  }
  // 批次结算：落盘失败如实透给调用方（dedup 路径只记 qWarning，不中断）。
  QString berr;
  if (!batch.flush(&berr))
  {
    // #169：结算失败时批内改动已回滚，不能再报「已补上关联」。
    setError(error, berr.isEmpty() ? QStringLiteral("catalog 落盘失败") : berr);
    return 0;
  }
  return attached;
}
