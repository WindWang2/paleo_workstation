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

// ---- 地物族分支：horizon（含 dedup 再派生特道）/ seismic / 辅助参考
// （document/image_reference/geojson/unknown）。

namespace
{
  // 编图层序界面集合（plan §3/§5E）。名单经 AreaRules（本工区默认
  // C3/C6/D53/D61/D62/D63/D71/D72；第二工区经 project_area.json 换名单）。
  bool isKnownSequenceBoundary(const QString &stem)
  {
    return AreaRules::active().sequenceBoundaries.contains(stem.toUpper());
  }

  // ASCII 字母数字才当井名边界的一部分：井名常与中文目录词直接相连
  //（A1岩屑录井数据——岩是字母但不是 ASCII 字母，是合法边界）；而
  // A31868.62…（薄片：井名 A3 + 深度）里的 '1' 是 ASCII 数字，绝不能当边界。
  bool asciiAlnum(QChar c)
  {
    const ushort u = c.unicode();
    return (u >= '0' && u <= '9') || (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z');
  }

  // 井附件前缀识井：candidates（文件主名/父目录名）任一以「井名+边界」开头
  // → 命中。井名长者先试（A10 先于 A1；边界判也独立挡掉 A10→A1 误配）。
  // 返回唯一井实体 id；命中井名经 matchedNames 带出（≥2 口不同井 → 返回空
  // id，调用方按双候选不猜）。
  QString wellAttachmentTarget(const DataCatalog *cat, const QStringList &candidates,
                               QStringList *matchedNames)
  {
    struct Cand
    {
      QString id, name;
    };
    QVector<Cand> wells;
    for (const CatalogEntity &w : cat->entities(QStringLiteral("well")))
      if (!w.name.isEmpty())
        wells.append({w.id, w.name});
    std::sort(wells.begin(), wells.end(),
              [](const Cand &a, const Cand &b) { return a.name.size() > b.name.size(); });
    QHash<QString, QString> idToName; // 命中井 id → 名（去重）
    for (const QString &cand : candidates)
    {
      if (cand.isEmpty())
        continue;
      for (const Cand &w : wells)
      {
        if (!cand.startsWith(w.name, Qt::CaseInsensitive))
          continue;
        const int pos = w.name.size();
        if (pos < cand.size() && asciiAlnum(cand.at(pos)))
          continue; // A1 撞 A10/A31868… 前缀——边界判
        if (!idToName.contains(w.id))
          idToName.insert(w.id, w.name);
      }
    }
    if (idToName.isEmpty())
      return QString();
    for (auto it = idToName.cbegin(); it != idToName.cend(); ++it)
      matchedNames->append(it.value());
    return idToName.size() == 1 ? idToName.cbegin().key() : QString();
  }

  // 井附件角色（目录段关键词表——与 AreaRules 默认目录词表同思路，工区
  // 方言）：岩心照片→core（图片井道）、岩屑录井→cuttings（岩性道）、
  // 薄片/粒度→lab_analysis；无关键词 → 井域 reference。只看目录段——
  // 文件名是自由文本（薄片描述「富含白云岩屑.JPG」会误中「岩屑」）。
  QString attachmentRoleForKeywords(const QString &dirPath)
  {
    if (dirPath.contains(QStringLiteral("岩心")))
      return QStringLiteral("core");
    if (dirPath.contains(QStringLiteral("岩屑")))
      return QStringLiteral("cuttings");
    if (dirPath.contains(QStringLiteral("薄片")) || dirPath.contains(QStringLiteral("粒度")))
      return QStringLiteral("lab_analysis");
    return QStringLiteral("reference");
  }
} // namespace

namespace paleo::dataimport_detail {

// 方向57：importOneFile 的 importHorizonFamily 分支体（逐字搬迁；return fail(X) → return X，
// 失败记账/信号仍由分派壳的 fail 完成）。返回错误文案，空串 = 成功。
QString importHorizonFamily(FamilyContext &ctx)
{
  DataCatalog *const cat = ctx.cat;
  const QString &sourcePath = ctx.sourcePath;
  const QString &assetId = ctx.assetId;
  QString *const error = ctx.error;
  ImportSession &s = *ctx.s;
  const QString &stem = ctx.stem;
  const QString &versionId = ctx.versionId;
  const QFileInfo &fi = ctx.fi;
  QString &manifestLayerId = ctx.manifestLayerId;
    const bool known = isKnownSequenceBoundary(stem);
    const QString sbId = QStringLiteral("sb-%1").arg(stem.toUpper());
    if (!cat->hasEntity(sbId))
    {
      CatalogEntity sb;
      sb.id = sbId;
      sb.entityType = QStringLiteral("sequence_boundary");
      sb.name = stem.toUpper();
      if (!known)
        sb.extra.insert(QStringLiteral("pending"), true); // 未决层位，不进编图 chip
      if (!cat->addEntity(sb, error))
        return *error;
    }
    EntityAssetLink link;
    link.entityType = QStringLiteral("sequence_boundary");
    link.entityId = sbId;
    link.assetId = assetId;
    link.role = QStringLiteral("horizon");
    link.isPrimary = true;
    link.unresolved = !known;
    if (!cat->addLink(link, error))
      return *error;

    // 已知界面：装箱派生时间栅格（DERIVED，父版本=RAW）并登记图层清单。
    if (known)
    {
      QFile f(sourcePath);
      if (!f.open(QIODevice::ReadOnly))
        return QStringLiteral("cannot read %1").arg(sourcePath);
      BinnedHorizon binned;
      if (!binHorizon(f.readAll(), &binned, error))
        return *error;

      const QString derivedVersionId =
          cat->nextVersionId();
      const QString tifName = stem.toUpper() + QStringLiteral(".tif");
      const QString derivedRel = DataCatalog::managedPath(QStringLiteral("derived"), assetId,
                                                          derivedVersionId, tifName);
      if (derivedRel.isEmpty())
        return QStringLiteral("派生文件名不是合法路径段: %1").arg(tifName);
      const QString relPath = QStringLiteral("artifacts/") + derivedRel;
      CatalogVersion pending;
      pending.managed = true;
      pending.path = relPath;
      // 最终位置（提交后）——图层声明/金字塔用它；字节先写进暂存根。
      const QString tifPath = DataCatalog::resolvedVersionPath(s.projectDir, pending);
      if (tifPath.isEmpty())
        return QStringLiteral("unsafe managed destination: %1").arg(relPath);
      if (!s.ensureStagingRoot(error))
        return *error;
      const QString stagedTif = DataCatalog::resolvedVersionPath(s.stagingRoot, pending);
      if (stagedTif.isEmpty())
        return QStringLiteral("unsafe managed destination: %1").arg(relPath);
      if (!QDir().mkpath(QFileInfo(stagedTif).absolutePath()))
        return QStringLiteral("cannot create directory %1")
                        .arg(QFileInfo(stagedTif).absolutePath());
      if (!writeHorizonGeoTiff(binned, stagedTif, error))
        return *error;
      QFile::setPermissions(stagedTif, QFileDevice::ReadOwner | QFileDevice::ReadUser |
                                           QFileDevice::ReadGroup | QFileDevice::ReadOther);
      // B3：派生时间栅格同批接线——Lazy ensure 瓦片金字塔（近零开销）。提交
      // 后在 owner 线程对最终路径执行（暂存路径不进金字塔身份表）。
      s.pyramidTargets.append(tifPath);

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
      if (!cat->addVersion(derived, error))
        return *error;

      // 图层清单只登记要画的结果（§2）：北向上时间栅格 + 局部测网 CRS。
      LayerDeclaration decl;
      decl.layerId = QStringLiteral("horizon.%1").arg(stem.toUpper());
      decl.horizon = stem.toUpper();
      decl.type = QStringLiteral("raster");
      decl.source = tifPath;
      decl.group = QStringLiteral("00_Data");
      // declare 写 layer manifest（sqlite）——提交成功后由 owner 线程发射。
      s.recordLayerDeclared(decl);
      manifestLayerId = decl.layerId;
    }
  return QString();
}


// 方向57：importOneFile 的 importSeismicFamily 分支体（逐字搬迁；return fail(X) → return X，
// 失败记账/信号仍由分派壳的 fail 完成）。返回错误文案，空串 = 成功。
QString importSeismicFamily(FamilyContext &ctx)
{
  DataCatalog *const cat = ctx.cat;
  const QString &sourcePath = ctx.sourcePath;
  const QString &assetId = ctx.assetId;
  QString *const error = ctx.error;
  const QString &stem = ctx.stem;
    // 打开（索引式）冻结 survey 几何：角点、inline/crossline 范围、采样间隔、起始时间。
    SegyReader reader;
    QString serr;
    if (!reader.open(sourcePath, &serr))
      return serr;
    const SegyGeometry g = reader.geometry();
    const QString surveyId = QStringLiteral("survey-%1").arg(stem);
    if (!cat->hasEntity(surveyId))
    {
      CatalogEntity svy;
      svy.id = surveyId;
      svy.entityType = QStringLiteral("seismic_survey");
      svy.name = stem;
      svy.inlineMin = g.inlineMin;
      svy.inlineMax = g.inlineMax;
      svy.xlineMin = g.xlineMin;
      svy.xlineMax = g.xlineMax;
      svy.sampleIntervalUs = reader.sampleIntervalUs();
      svy.startTimeMs = g.startTimeMs;
      for (int i = 0; i < 4; ++i)
        svy.corners.append({g.cornerX[i], g.cornerY[i]});
      // 配准可用时把角点换算成 WGS84 包围盒落 extra（原始 corners 仍存局部
      // 网格——视图/测线几何管线不变）。
      if (ctx.s->georeference)
      {
        double lo = 180, hi = -180, la = 90, ha = -90;
        bool have = false;
        for (int i = 0; i < 4; ++i)
        {
          double lonDeg = 0, latDeg = 0;
          if (!applyGeoreference(*ctx.s->georeference, g.cornerX[i], g.cornerY[i],
                                 &lonDeg, &latDeg))
          {
            have = false;
            break;
          }
          have = true;
          lo = qMin(lo, lonDeg);
          hi = qMax(hi, lonDeg);
          la = qMin(la, latDeg);
          ha = qMax(ha, latDeg);
        }
        if (have)
        {
          svy.extra.insert(QStringLiteral("wgs84BboxWest"), lo);
          svy.extra.insert(QStringLiteral("wgs84BboxEast"), hi);
          svy.extra.insert(QStringLiteral("wgs84BboxSouth"), la);
          svy.extra.insert(QStringLiteral("wgs84BboxNorth"), ha);
        }
      }
      if (!cat->addEntity(svy, error))
        return *error;
    }
    EntityAssetLink link;
    link.entityType = QStringLiteral("seismic_survey");
    link.entityId = surveyId;
    link.assetId = assetId;
    link.role = QStringLiteral("seismic_volume");
    link.isPrimary = true;
    if (!cat->addLink(link, error))
      return *error;
  return QString();
}


// 方向57：importOneFile 的 importAuxReferenceFamily 分支体（逐字搬迁；return fail(X) → return X，
// 失败记账/信号仍由分派壳的 fail 完成）。返回错误文案，空串 = 成功。
QString importAuxReferenceFamily(FamilyContext &ctx)
{
  // 原壳的辅助角色常量（实体关联 role=reference）。
  const QString auxRefRole = QStringLiteral("reference");
  DataCatalog *const cat = ctx.cat;
  const QString &sourcePath = ctx.sourcePath;
  const QString &assetId = ctx.assetId;
  QString *const error = ctx.error;
  const QString &stem = ctx.stem;
  const ProjectClassification &cls = ctx.cls;

  // 井附件优先挂井（竞赛工区数据形态）：兜底类型（image_reference/document/
  // outsource_workbook/tabular/unknown）若能从「文件名主名或父目录名」唯一
  // 识井——前缀 = 既有井名 + 非 ASCII 字母数字边界，如 A1,1849.35m.JPG /
  // A1岩屑录井数据.xlsx / A3薄片鉴定/A31868.62粒间孔.JPG——挂井链接而不是
  // 落辅助实体。角色按路径关键词表（岩心→core、岩屑→cuttings、薄片/粒度→
  // lab_analysis），无关键词 → 井域 reference。双候选 → 未决井链接（§3 不
  // 猜）；零匹配 → 原辅助实体路径不变。
  {
    QStringList matchedNames;
    const QString wellId = wellAttachmentTarget(cat, {stem, ctx.fi.dir().dirName()},
                                                &matchedNames);
    if (!wellId.isEmpty() || matchedNames.size() >= 2)
    {
      EntityAssetLink link;
      link.entityType = QStringLiteral("well");
      link.assetId = assetId;
      link.role = attachmentRoleForKeywords(ctx.fi.absolutePath());
      if (!wellId.isEmpty())
      {
        link.entityId = wellId;
        link.isPrimary = true;
      }
      else
      {
        link.unresolved = true;
        link.note = QStringLiteral("井附件双候选: %1").arg(matchedNames.join(
            QStringLiteral("、")));
      }
      if (!cat->addLink(link, error))
        return *error;
      return QString();
    }
  }

    // document / image_reference / geojson / unknown / 参考资料 XML：辅助实体 + reference。
    const QString auxId =
        cat->nextEntityId(QStringLiteral("aux"));
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
    if (!cat->addEntity(aux, error))
      return *error;
    EntityAssetLink link;
    link.entityType = QStringLiteral("auxiliary");
    link.entityId = auxId;
    link.assetId = assetId;
    link.role = auxRefRole;
    link.isPrimary = true;
    if (!cat->addLink(link, error))
      return *error;
  return QString();
}


// 方向57：dedup 命中已知层位界面的再派生特道（原 importOneFile 内芯逐字搬迁，
// return fail(X) → return X）。返回错误文案；空串 = 无事可做或成功。
QString dedupHorizonRederive(DataCatalog *cat, ImportSession &s,
                             const CatalogVersion &existing, const QString &sourcePath,
                             QString *error)
{
      const QString horizon = QFileInfo(existing.fileName.isEmpty() ? sourcePath : existing.fileName)
                                  .completeBaseName().toUpper();
      if (isKnownSequenceBoundary(horizon))
      {
        CatalogVersion derived;
        const QVector<CatalogVersion> siblings =
            cat->versionsForAsset(existing.assetId);
        for (const CatalogVersion &v : siblings)
          if (v.stage.compare(QStringLiteral("DERIVED"), Qt::CaseInsensitive) == 0 &&
              v.parentVersionIds.contains(existing.id) &&
              QFileInfo::exists(cat->versionFilePath(v))) // 同批新增派生件在暂存根
          {
            derived = v;
            break;
          }
        if (derived.id.isEmpty())
        {
          QFile source(sourcePath);
          if (!source.open(QIODevice::ReadOnly))
            return QStringLiteral("cannot read %1").arg(sourcePath);
          BinnedHorizon binned;
          if (!binHorizon(source.readAll(), &binned, error))
            return error ? *error : QStringLiteral("horizon binning failed");
          derived.id = cat->nextVersionId();
          derived.assetId = existing.assetId;
          derived.stage = QStringLiteral("DERIVED");
          derived.versionNumber =
              cat->currentVersion(existing.assetId).versionNumber + 1;
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
          if (DataCatalog::resolvedVersionPath(s.projectDir, derived).isEmpty())
            return QStringLiteral("unsafe managed destination: %1").arg(derived.path);
          if (!s.ensureStagingRoot(error))
            return *error;
          const QString dst = DataCatalog::resolvedVersionPath(s.stagingRoot, derived);
          if (dst.isEmpty())
            return QStringLiteral("unsafe managed destination: %1").arg(derived.path);
          if (!QDir().mkpath(QFileInfo(dst).absolutePath()))
            return QStringLiteral("cannot create directory %1")
                            .arg(QFileInfo(dst).absolutePath());
          if (!writeHorizonGeoTiff(binned, dst, error))
            return error ? *error : QStringLiteral("horizon raster write failed");
          QFile::setPermissions(dst, QFileDevice::ReadOwner | QFileDevice::ReadUser |
                                     QFileDevice::ReadGroup | QFileDevice::ReadOther);
          if (!cat->addVersion(derived, error))
            return error ? *error : QStringLiteral("catalog addVersion failed");
        }
        LayerDeclaration decl;
        decl.layerId = QStringLiteral("horizon.%1").arg(horizon);
        decl.horizon = horizon;
        decl.type = QStringLiteral("raster");
        decl.source = DataCatalog::resolvedVersionPath(s.projectDir, derived); // 提交后的最终位置
        decl.group = QStringLiteral("00_Data");
        s.recordLayerDeclared(decl);
      }
  return QString();
}

} // namespace paleo::dataimport_detail
