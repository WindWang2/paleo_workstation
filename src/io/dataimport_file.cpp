// 层：数据
// 方向57：从 dataimportservice.cpp 按格式族析出。公共 API（dataimportservice.h）
// 零改动；跨族共享辅助（setError/readFileOrEmpty/FamilyContext）经
// dataimport_internal.h；importOneFile 分支体族函数亦声明于该头。
#include "dataimportservice.h"

#include "lascache.h"
#include "lasparser.h" // LasParser::fileSizeLimit（大文件防护共用口径）
#include "rasterpyramid.h"
#include "segyindexstore.h"
#include "shacache.h"

#include "../catalog/datacatalog.h"
#include "../metadata/layermanifest.h"
#include "../metadata/paleoprojectstore.h"
#include "../domain/arearules.h"
#include "horizonbinner.h"
#include "ingestplan.h"
#include "importclassification.h"
#include "welllogread.h" // 方向44：井名提取分派
#include "../domain/projectclassifier.h"
#include "segyreader.h"
#include "wellcompositexml.h"
#include "wellfileparsers.h"
#include "welltable.h"
#include "wellfactorfile.h"
#include "projectboundary.h"

#include <QSet>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QProcess>
#include <QRegularExpression>
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
#include <utility>
#include "dataimport_internal.h"
#include "dataimport_log.h"
#include "importrecovery.h"
#include "nativemodel.h"

using paleo::dataimport_detail::setError;


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
      if (eff.expectedSourceSha256.isEmpty()) eff.expectedSourceSha256 = s.expectedSourceHashes.value(item.path);
      if (eff.forceType.isEmpty())
        eff.forceType = suggestedProjectImportType(item.path, item.type);
      s.fileResult = importOneFile(s, item.path, eff, &err);
      s.error = err;
      if (!paleo::dataimport_detail::writeImportIntent(s, &s.commitPreparationError)) s.error = s.commitPreparationError;
      return;
    }
  }
  s.fileResult = importOneFile(s, sourcePath, options, &err);
  s.error = err;
  if (!paleo::dataimport_detail::writeImportIntent(s, &s.commitPreparationError)) s.error = s.commitPreparationError;
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

  // §3 + F2（review-security）：源侧 SHA-256 一律流式真算（logFileDigest），
  // 不走 ShaCache——缓存指纹信 mtime+size（Windows 无 inode），同步工具保时
  // 间戳换内容会让 dedup 误命中「字节已在库」、新数据静默丢失。dedup 身份、
  // 外链留底、预览指纹复核共用这一趟真哈希；命中性能损失由库内复核兜底
  // （versionBySha256 本就真哈希库内副本）。
  QElapsedTimer timer; timer.start();
  ProjectClassification cls = classifyImportFile(sourcePath);
  cls.type = resolvedProjectImportType(sourcePath, cls.type, options.forceType);
  const QString sourceSha = paleo::dataimport_detail::logFileDigest(sourcePath);
  const qint64 hashMs = timer.elapsed();
  if (sourceSha.isEmpty())
    return fail(QCoreApplication::translate("DataImportService", "无法读取或哈希源文件：%1").arg(sourcePath));
  if (!options.expectedSourceSha256.isEmpty() && sourceSha != options.expectedSourceSha256)
  {
    // 预期指纹来自预览（plan 期缓存值可能已失真）——让缓存失效，下次预览重算。
    ShaCache::shared().invalidate(sourcePath);
    return fail(QCoreApplication::translate("DataImportService", "文件内容已不同于预览：%1。请重新生成预览后确认。").arg(sourcePath));
  }

  const QString stem = fi.completeBaseName();
  const bool fixedAux = isFixedAuxiliaryPath(sourcePath) &&
                        (cls.type == QLatin1String("well_head") ||
                         cls.type == QLatin1String("well_log"));
  // 在创建资产/复制 RAW 前验证井表；拒收不能留下冒充成功的实体关联。
  WellParseReport wellReport;
  QVector<WellHeadRecord> parsedHeads;
  QVector<WellTopRecord> parsedTops;
  TimeDepthTable parsedTd;
  DeviationTable parsedDev;
  const bool textWellInput = !fixedAux &&
      (cls.type == QLatin1String("well_head") ||
       cls.type == QLatin1String("well_stratification") ||
       cls.type == QLatin1String("time_depth") ||
       (cls.type == QLatin1String("well_deviation") && cls.format != QLatin1String("xml")));
  QString parseSummary;
  const auto importedFactors = paleo::io::isWellFactorFile(sourcePath)
      ? paleo::io::readWellFactorFile(sourcePath, error) : QVariantList{};
  if (paleo::io::isWellFactorFile(sourcePath) && importedFactors.isEmpty()) return fail(*error);
  paleo::dataimport_detail::LogInspection logInfo;
  const bool wellLogInput = !fixedAux && cls.type == QLatin1String("well_log");
  std::unique_ptr<QTemporaryDir> logSnapshot;
  QString validatedLogPath = sourcePath;
  if (wellLogInput)
  {
    // 引用模式直接读取原文件/已建立的工区模型；只有显式受管导入需要私有副本。
    if (!options.linkExternal) {
      logSnapshot = std::make_unique<QTemporaryDir>(QDir::tempPath() + QStringLiteral("/paleo-log-import-XXXXXX"));
      validatedLogPath = logSnapshot->filePath(fi.fileName());
      if (!logSnapshot->isValid() || !QFile::copy(sourcePath, validatedLogPath))
        return fail(QCoreApplication::translate("DataImportService", "无法为 %1 创建导入快照，请检查临时目录空间与权限后重试。").arg(sourcePath));
      QFile::setPermissions(validatedLogPath, QFile::permissions(validatedLogPath) | QFileDevice::WriteOwner);
    }
    if (!paleo::dataimport_detail::inspectLog(validatedLogPath, logInfo, error))
    {
      error->replace(validatedLogPath, sourcePath);
      return fail(*error);
    }
    for (auto &issue : logInfo.issues) issue.replace(validatedLogPath, sourcePath);
    if (paleo::dataimport_detail::logFileDigest(validatedLogPath) != sourceSha)
      return fail(QCoreApplication::translate("DataImportService", "读取期间文件发生变化：%1。请重新生成预览后确认。").arg(sourcePath));
    parseSummary = logInfo.summary;
  }
  paleo::io::ValidatedWellInput tableInput;
  if (textWellInput) {
    auto mapping = options.tableMapping;
    if (s.georeference && s.georeference->kind == QLatin1String("crs")) mapping.insert(QStringLiteral("projectCrs"), s.georeference->sourceCrs);
    if (!paleo::io::readImportWellInput(sourcePath, cls.type, sourceSha, mapping, &tableInput, error)) return fail(*error);
    parsedHeads = tableInput.heads; parsedTops = tableInput.tops;
    parsedTd = tableInput.td; parsedDev = tableInput.deviation;
    wellReport = tableInput.report; parseSummary = wellParseSummary(wellReport);
  }

  const qint64 validationMs = timer.elapsed() - hashMs;
  // 重导据此走全新导入（issue #5）。
  const CatalogVersion existing =
      cat->versionBySha256(sourceSha);
  if (!existing.id.isEmpty())
  {
    if (wellLogInput && !paleo::dataimport_detail::validateDuplicateLogTarget(
            *cat, existing.assetId, options.targetWellId, error)) return fail(*error);
    if (wellLogInput && !logInfo.sourceAnnotations.isEmpty() &&
        !cat->updateVersionExtra(existing.id, QVariantMap{{"synthetic_input", true},
          {"input_source_annotations", logInfo.sourceAnnotations}}, error)) return fail(*error);
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
    const int attached = wellLogInput ? 0 : attachResolvableLinks(cat, existingAsset, sourcePath, &aerr);
    if (!aerr.isEmpty())
      qWarning("import dedup attach: %s", qPrintable(aerr));
    QString msg =
        QStringLiteral("字节已在库 · %1")
            .arg(attached > 0 ? QStringLiteral("已补上关联")
                              : QStringLiteral("没有新的关联"));
    // 旧版本没有报告时，也对本次相同字节重新验证并展示。
    if (!parseSummary.isEmpty()) msg += QStringLiteral("；") + parseSummary;
    qInfo("import: %s — %s", qPrintable(msg), qPrintable(sourcePath));
    res.linkAttached = attached > 0;
    s.recordImported(existingAsset.type, existing.assetId, QString()); // 聚焦已有资产
    return done(ImportOutcome::AlreadyStored, existing.assetId, msg);
  }

  const QString assetId = cat->nextAssetId();
  const QString versionId = cat->nextVersionId();

  CatalogAsset asset;
  asset.id = assetId;
  asset.type = cls.type;
  asset.format = cls.format;
  asset.displayName = options.displayName.trimmed().isEmpty() ? fi.fileName()
                                                              : options.displayName.trimmed();
  if (!textWellInput && !cat->addAsset(asset, error))
    return fail(*error);

  CatalogVersion version;
  if (!importedFactors.isEmpty()) version.extra.insert(QStringLiteral("wellFactors"), importedFactors);
  version.id = versionId;
  version.assetId = assetId;
  version.stage = QStringLiteral("RAW");
  if (!tableInput.normalized.isEmpty()) version.extra.insert(QStringLiteral("wellTableParsed"), true);
  if (textWellInput)
  {
    version.extra.insert(QStringLiteral("wellParseReport"), QVariantMap{
        {QStringLiteral("acceptedRows"), wellReport.acceptedRows},
        {QStringLiteral("rejectedRows"), wellReport.rejectedRows},
        {QStringLiteral("sentinelHits"), wellReport.sentinelHits},
        {QStringLiteral("blankCells"), wellReport.blankCells},
        {QStringLiteral("invalidCells"), wellReport.invalidCells},
        {QStringLiteral("issues"), wellReport.issues},
        {QStringLiteral("summary"), parseSummary}});
  }
  version.versionNumber = 1;
  version.fileName = fi.fileName();
  version.sourceUri = fi.absoluteFilePath();
  if (cls.type == QLatin1String("well_head")) {
    const auto sourceCrs=options.tableMapping.value("crs").toString();
    const auto destinationCrs=options.tableMapping.value("projectCrs").toString();
    version.extra.insert(QStringLiteral("wellCoordinatesCrs"),
        !tableInput.normalized.isEmpty() && !destinationCrs.isEmpty() ? destinationCrs : sourceCrs);
  }
  if (wellLogInput)
  {
    if (logInfo.comprehensiveTemplate) version.extra.insert(QStringLiteral("logCompositeTemplate"), true);
    version.extra.insert(QStringLiteral("logCurves"), logInfo.header.curveNames.mid(1));
    version.extra.insert(QStringLiteral("logCurveNames"), logInfo.header.curveNames);
    version.extra.insert(QStringLiteral("logWellName"), logInfo.header.wellName);
    version.extra.insert(QStringLiteral("logIndexBasis"), logInfo.header.indexBasis);
    version.extra.insert(QStringLiteral("logSummary"), logInfo.summary);
    version.extra.insert(QStringLiteral("logIssues"), logInfo.issues);
    if (!logInfo.sourceAnnotations.isEmpty()) {
      version.extra.insert(QStringLiteral("synthetic_input"), true);
      version.extra.insert(QStringLiteral("input_source_annotations"), logInfo.sourceAnnotations);
    }
  }

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
    if (!storeManagedRaw(s, validatedLogPath, assetId, versionId, &relPath, &sha, error))
      return fail(*error);
    if ((textWellInput || wellLogInput) && sha != sourceSha)
    {
      // 本行尚无资产/版本/关联；移除自身暂存 RAW，不能提交不匹配的解析报告。
      version.managed = true;
      version.path = relPath;
      const QString staged = DataCatalog::resolvedVersionPath(s.stagingRoot, version);
      QFile::setPermissions(staged, QFile::permissions(staged) | QFileDevice::WriteOwner);
      if (!QFile::remove(staged))
        qWarning("import: cannot remove changed staged well file %s", qPrintable(staged));
      return fail(QCoreApplication::translate("DataImportService", "井表复制期间内容发生变化，请重试：%1").arg(sourcePath));
    }
    version.path = relPath;
    version.sha256 = sha;
  }
  if (textWellInput && !cat->addAsset(asset, error))
    return fail(*error);
  // 井附件深度锚（岩心/薄片照片的「<井名>,<深度>m」文件名惯例，如
  // A1,1849.35m.JPG / A1,1859.15m$1.JPG）：图片井道按深度挂图的元数据。
  // 只识别「数字 m」模式；不带单位的深度（薄片 A31868.62粒间孔.JPG）留在
  // 文件名里由消费侧解析——不猜。导入后可用 updateVersionExtra 后补/修正
  //（#source=filename 标来源；手工编辑改写为 manual）。
  if (cls.type == QLatin1String("image_reference"))
  {
    static const QRegularExpression depthM(
        QStringLiteral("(\\d+(?:\\.\\d+)?)\\s*m(?![A-Za-z0-9])"));
    const QRegularExpressionMatch dm = depthM.match(fi.completeBaseName());
    if (dm.hasMatch())
    {
      version.extra.insert(QStringLiteral("depthMd"), dm.captured(1).toDouble());
      version.extra.insert(QStringLiteral("depthMd#source"),
                           QStringLiteral("filename"));
    }
  }
  if (!cat->addVersion(version, error))
    return fail(*error);

  // ---- 实体解析与关联（角色沿用已有名字）----
  if (!paleo::io::storeNormalizedWellTable(s, version, tableInput.normalized, error)) return fail(*error);
  QString manifestLayerId;
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
  ctx.parsedHeads = std::move(parsedHeads);
  ctx.parsedTops = std::move(parsedTops);
  ctx.parsedTd = std::move(parsedTd);
  ctx.parsedDev = std::move(parsedDev);
  ctx.logWellName = logInfo.header.wellName;
  ctx.targetWellId = options.targetWellId;
  ctx.inputCrs = options.tableMapping.value("crs").toString();
  if (cls.type == "seismic" && !paleo::dataimport_detail::prepareSeismicCoordinates(ctx, options.tableMapping.value("crs").toString())) return fail(*error);
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
  else if (cls.type == QLatin1String("boundary"))
    branchErr = paleo::dataimport_detail::importProjectBoundary(ctx);
  else
    branchErr = paleo::dataimport_detail::importAuxReferenceFamily(ctx);
  if (!branchErr.isEmpty())
    return fail(branchErr);
  manifestLayerId = ctx.manifestLayerId;
  const qint64 nativeStart = timer.elapsed();
  if (!paleo::native_model::prepare(s, version, ctx, wellLogInput ? &logInfo : nullptr, error))
    return fail(*error);
  qInfo() << "catalog-import: file=" << fi.fileName() << "bytes=" << fi.size()
          << "external=" << external << "elapsed_ms=" << timer.elapsed()
          << "hash_ms=" << hashMs << "validation_ms=" << validationMs
          << "native_ms=" << timer.elapsed() - nativeStart;


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
  return done(ImportOutcome::Imported, assetId, parseSummary);
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
  // 测井归属只能来自验证内容或显式选择；即使改成参考类型后命中旧资产，
  // 也不能由此通用去重补挂入口恢复文件名猜测。
  if (asset.type == QLatin1String("well_stratification"))
  {
    QFile f(sourcePath);
    if (!f.open(QIODevice::ReadOnly))
    {
      setError(error, QStringLiteral("cannot read %1").arg(sourcePath));
      return 0;
    }
    if (f.size() > LasParser::fileSizeLimit()) // PW-076：整读前同 LAS 口径限额，防 OOM
    {
      setError(error, QStringLiteral("file too large (%1 MB limit): %2")
                          .arg(LasParser::fileSizeLimit() / (1024 * 1024))
                          .arg(sourcePath));
      return 0;
    }
    QStringList names;
    QSet<QString> seen; // PW-076：O(n) 去重
    const auto tops = paleo::io::isWellTableContainer(sourcePath) ? paleo::io::readWellTable(sourcePath).tops : parseWellTopsText(f.readAll());
    for (const WellTopRecord &t : tops)
      if (!seen.contains(t.wellName))
      {
        seen.insert(t.wellName);
        names.append(t.wellName);
      }
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
    if (f.size() > LasParser::fileSizeLimit()) // PW-076：整读前同 LAS 口径限额，防 OOM
    {
      setError(error, QStringLiteral("file too large (%1 MB limit): %2")
                          .arg(LasParser::fileSizeLimit() / (1024 * 1024))
                          .arg(sourcePath));
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
    if (f.size() > LasParser::fileSizeLimit()) // PW-076：整读前同 LAS 口径限额，防 OOM
    {
      setError(error, QStringLiteral("file too large (%1 MB limit): %2")
                          .arg(LasParser::fileSizeLimit() / (1024 * 1024))
                          .arg(sourcePath));
      return 0;
    }
    const QVector<WellHeadRecord> rows = paleo::io::isWellTableContainer(sourcePath)
        ? paleo::io::readWellTable(sourcePath).heads : parseWellHeadText(f.readAll());
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
