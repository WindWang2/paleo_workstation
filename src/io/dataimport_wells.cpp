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
#include "dataimport_log.h"

using paleo::dataimport_detail::setError;
using paleo::dataimport_detail::readFileOrEmpty;

// ---- 井族分支：well_head / well_log / well_stratification+time_depth /
// well_deviation（importOneFile 分支体族化 + 井身份解析 resolveWell +
// NS 辅助 candidatesNote/unmatchedNameNote/assignResolvedWellLogSlot）。

namespace
{
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

namespace paleo::dataimport_detail {

// 方向57：importOneFile 的 importWellHeadFamily 分支体（逐字搬迁；return fail(X) → return X，
// 失败记账/信号仍由分派壳的 fail 完成）。返回错误文案，空串 = 成功。
QString importWellHeadFamily(FamilyContext &ctx)
{
  DataCatalog *const cat = ctx.cat;
  const QString &sourcePath = ctx.sourcePath;
  const QString &assetId = ctx.assetId;
  QString *const error = ctx.error;
    const QVector<WellHeadRecord> &rows = ctx.parsedHeads;
    if (rows.isEmpty())
      return QStringLiteral("no well head rows in %1").arg(sourcePath);
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
        if (!cat->addLink(link, error))
          return *error;
        continue;
      }
      const QStringList matches =
          cat->wellsMatchingName(r.name);
      if (matches.size() >= 2)
      {
        link.unresolved = true;
        link.note = candidatesNote(cat, matches);
        if (!cat->addLink(link, error))
          return *error;
        continue;
      }
      QString wid;
      if (matches.size() == 1)
      {
        wid = matches.front(); // 已有井：直接挂，不另建
        // F5（review-security）：测井导入先建的井没有坐标（unknown/无
        // hasSurface）——井位表命中时回填坐标与状态，否则该井被剖面/地图
        // 静默排除。已有坐标的井不动（先到的坐标优先，不静默改写）。
        // 更新走 updateEntity → staging journal → 提交时重放。
        CatalogEntity w = cat->entityById(wid);
        if (w.entityType == QLatin1String("well") &&
            (w.coordinateStatus == QLatin1String("unknown") || !w.hasSurface))
        {
          w.hasSurface = true;
          w.surfaceX = r.x;
          w.surfaceY = r.y;
          w.kb = r.kb;
          w.td = r.td;
          double lonDeg = 0.0, latDeg = 0.0;
          if (ctx.s->georeference &&
              applyGeoreference(*ctx.s->georeference, r.x, r.y, &lonDeg, &latDeg))
          {
            w.coordinateStatus = QStringLiteral("ok");
            w.extra.insert(QStringLiteral("projectLon"), lonDeg);
            w.extra.insert(QStringLiteral("projectLat"), latDeg);
          }
          else
          {
            w.coordinateStatus = QStringLiteral("untransformed");
          }
          if (!cat->updateEntity(w, error))
            return *error;
        }
      }
      else
      {
        wid = QStringLiteral("well-%1").arg(r.name);
        // id 被别的规范化名占用（罕见）→ 让位于序号 id。
        if (cat->hasEntity(wid))
          wid = cat->nextEntityId(QStringLiteral("well"));
        CatalogEntity w;
        w.id = wid;
        w.entityType = QStringLiteral("well");
        w.name = r.name;
        w.hasSurface = true;
        w.surfaceX = r.x;
        w.surfaceY = r.y;
        w.kb = r.kb;
        w.td = r.td;
        // 局部测网坐标：真投影参数出现前保持未变换（plan §3）。工程带
        // georeference 时换算 WGS84 落 extra（surfaceX/Y 仍存原始网格——
        // 地图读它，渲染管线不变），状态进 ok。
        double lonDeg = 0.0, latDeg = 0.0;
        if (ctx.s->georeference &&
            applyGeoreference(*ctx.s->georeference, r.x, r.y, &lonDeg, &latDeg))
        {
          w.coordinateStatus = QStringLiteral("ok");
          w.extra.insert(QStringLiteral("projectLon"), lonDeg);
          w.extra.insert(QStringLiteral("projectLat"), latDeg);
        }
        else
        {
          w.coordinateStatus = QStringLiteral("untransformed");
        }
        if (!cat->addEntity(w, error))
          return *error;
      }
      link.entityId = wid;
      link.isPrimary = true;
      if (!cat->addLink(link, error))
        return *error;
    }
  return QString();
}


// 方向57：importOneFile 的 importWellLogFamily 分支体（逐字搬迁；return fail(X) → return X，
// 失败记账/信号仍由分派壳的 fail 完成）。返回错误文案，空串 = 成功。
QString importWellLogFamily(FamilyContext &ctx)
{
  return importInspectedLog(ctx);
}


// 方向57：importOneFile 的 importWellTopsFamily 分支体（逐字搬迁；return fail(X) → return X，
// 失败记账/信号仍由分派壳的 fail 完成）。返回错误文案，空串 = 成功。
QString importWellTopsFamily(FamilyContext &ctx)
{
  DataCatalog *const cat = ctx.cat;
  const QString &sourcePath = ctx.sourcePath;
  const QString &assetId = ctx.assetId;
  QString *const error = ctx.error;
  const QString &stem = ctx.stem;
  const ProjectClassification &cls = ctx.cls;
  {
    // 井名来自文件内容（分层=井名列；时深=# Well 行），规则同测井。
    QStringList names;
    if (cls.type == QLatin1String("well_stratification"))
    {
      const QVector<WellTopRecord> &tops = ctx.parsedTops;
      for (const WellTopRecord &t : tops)
        if (!names.contains(t.wellName))
          names.append(t.wellName);
    }
    else
    {
      const TimeDepthTable &td = ctx.parsedTd;
      names.append(td.wellName.isEmpty() ? stem : td.wellName);
    }
    if (names.isEmpty())
      return QStringLiteral("no well names in %1").arg(sourcePath);

    const QString role = cls.type == QLatin1String("well_stratification")
                             ? QStringLiteral("tops")
                             : QStringLiteral("time_depth");
    // §3 修订：每个井名一条链接；未决链接实体 id 留空，备注记候选或未匹配名，
    // 不新建井、不合并、不再挂辅助实体。
    for (const QString &n : names)
    {
      QStringList tried{n};
      WellBind bind = resolveWell(cat, n);
      if (bind.unresolved && bind.candidates.isEmpty() &&
          ctx.version.extra.value(QStringLiteral("wellTableParsed")).toBool()) {
        const auto id = ensureNamedImportWell(*cat, n, error);
        if (id.isEmpty()) return *error;
        bind = resolveWell(cat, n);
      }
      if (bind.unresolved && bind.candidates.isEmpty() && names.size() == 1)
      {
        bind = resolveWell(cat, stem); // 单井文件的文件名主名回退
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
                        ? candidatesNote(cat, bind.candidates)
                        : unmatchedNameNote(tried);
      }
      if (!cat->addLink(link, error))
        return *error;
    }
  }
  return QString();
}


// 方向57：importOneFile 的 importDeviationFamily 分支体（逐字搬迁；return fail(X) → return X，
// 失败记账/信号仍由分派壳的 fail 完成）。返回错误文案，空串 = 成功。
QString importDeviationFamily(FamilyContext &ctx)
{
  DataCatalog *const cat = ctx.cat;
  const QString &sourcePath = ctx.sourcePath;
  const QString &assetId = ctx.assetId;
  QString *const error = ctx.error;
  const QString &stem = ctx.stem;
  const ProjectClassification &cls = ctx.cls;
    // 井斜站表：井名来自文本 '# Well :' 行（XML 站表无井名——文件名主名），
    // 规则同时深：每井名一条 trajectory 链接；未决留空不建井，不猜。
    QStringList names;
    if (cls.format == QLatin1String("xml"))
    {
      QVector<WellComposite::XmlDeviationStation> parsed;
      QString perr;
      if (!WellComposite::parseDeviationSurvey(sourcePath, parsed, &perr))
        return QStringLiteral("no deviation stations in %1 (%2)")
                        .arg(sourcePath, perr);
      names.append(stem);
    }
    else
    {
      const DeviationTable &dev = ctx.parsedDev;
      names.append(dev.wellName.isEmpty() ? stem : dev.wellName);
    }
    if (names.isEmpty())
      return QStringLiteral("no well names in %1").arg(sourcePath);
    for (const QString &n : names)
    {
      QStringList tried{n};
      WellBind bind = resolveWell(cat, n);
      if (bind.unresolved && bind.candidates.isEmpty() && names.size() == 1)
      {
        bind = resolveWell(cat, stem); // 单井文件的文件名主名回退
        tried.append(stem);
      }
      EntityAssetLink link;
      link.entityType = QStringLiteral("well");
      link.assetId = assetId;
      link.role = QStringLiteral("trajectory");
      if (!bind.unresolved)
      {
        link.entityId = bind.entityId;
        link.isPrimary = true;
      }
      else
      {
        link.unresolved = true;
        link.note = bind.candidates.size() >= 2
                        ? candidatesNote(cat, bind.candidates)
                        : unmatchedNameNote(tried);
      }
      if (!cat->addLink(link, error))
        return *error;
    }
  return QString();
}

} // namespace paleo::dataimport_detail


namespace paleo::dataimport_detail
{
WellBind resolveWell(const DataCatalog *cat, const QString &name)
{
  WellBind b;
  if (name.trimmed().isEmpty())
  {
    b.unresolved = true;
    return b;
  }
  const QStringList ids = cat->wellsMatchingName(name);
  if (ids.size() == 1)
  {
    b.entityId = ids.front();
    return b;
  }
  b.unresolved = true;
  b.candidates = ids; // 0 个或 2+ 个
  return b;
}
} // namespace paleo::dataimport_detail
