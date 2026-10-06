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

// ---------------------------------------------------------------------------
// §3/§4「导入工区文件夹」：递归枚举普通文件（分类依赖 井位/井分层/时深/层位
// 路径段），两阶段处理——全部 well_head 行先走（井建齐），其余文件再对已齐
// 的井集解析。单行失败只落行、不中断；逃出所选根目录的符号链接与非普通文
// 件标 Skipped。dedup（AlreadyStored）记 Imported，message 留「字节已在库」。
// ---------------------------------------------------------------------------
// 文件夹枚举/分类/shp 归组/身份匹配/去重/两阶段排序已并入 C 包 plan 构建器
// （io/ingestplan.cpp 的 buildIngestPlan）——previewFolder 与 importFolder
// 共用同一份 plan，确认表逐行看到的就是将要执行的决策。
// ---------------------------------------------------------------------------


QVector<FolderPreviewRow>
DataImportService::previewFolder(const QString &dirPath, QString *error)
{
  return previewFolder(dirPath, error, {});
}

QVector<FolderPreviewRow>
DataImportService::previewFolder(const QString &dirPath, QString *error,
                                 const std::function<bool(int, const QString &)> &scanProgress)
{
  if (error)
    error->clear();
  // 审计 02 M-8：预览也只读 session 的 staging 副本；后台预览走
  // beginImport + producePreview（FolderImportWorkflow::previewFolderAsync）。
  const std::shared_ptr<ImportSession> s = beginImport();
  if (!s)
  {
    setError(error, offThreadError());
    return {};
  }
  return producePreview(*s, dirPath, error, scanProgress);
}

QVector<FolderPreviewRow>
DataImportService::producePreview(ImportSession &sess, const QString &dirPath, QString *error,
                                  const std::function<bool(int, const QString &)> &scanProgress)
{
  QVector<FolderPreviewRow> rows;
  if (error)
    error->clear();
  if (dirPath.isEmpty() || !QFileInfo(dirPath).isDir())
  {
    setError(error, QStringLiteral("找不到目录: %1").arg(dirPath));
    return rows;
  }
  // C 包：预览即 plan。T2：经 COW 快照在当前线程构建（扫描/分类/族归组/
  // 身份匹配/去重/主建议）——GUI 直调等价旧路径，worker 调用（经
  // FolderImportWorkflow::previewFolderAsync）整段离 GUI。行序 = 执行序。
  const IngestPlan plan = planFor(sess, dirPath, scanProgress);
  if (plan.cancelled)
  {
    setError(error, QStringLiteral("已取消"));
    return rows; // 扫描段取消：不展示半份预览
  }
  if (plan.items.isEmpty() && plan.skipped.isEmpty())
  {
    setError(error, plan.issues.isEmpty()
                        ? QStringLiteral("目录里没有可导入的文件: %1").arg(dirPath)
                        : plan.issues.join(QStringLiteral("\n")));
    return rows;
  }
  // 确认表显示语义在预览期算好（domain/importrows.h）：行显示类型、可改
  // 与否、类型词表——视图纯渲染，不回查分类器谓词。
  const QStringList vocab = projectClassifierTypes(); // 「reference」伪类型含在内
  const auto fillDisplay = [&vocab](FolderPreviewRow &r) {
    // 行默认显示类型：HZ28-6-1 固定辅助 → 「参考」；「参考资料」目录内井类/
    // 未判内容默认「参考」（可改）；其余行显示分类器原类型。
    QString disp = r.classifiedType;
    if (isFixedAuxiliaryPath(r.path))
      disp = QStringLiteral("reference");
    else if (isDefaultReferencePath(r.path))
    {
      // document/image_reference/geojson/seismic/horizon 等显示真实类型——
      // 它们本来就走辅助实体，且类型名驱动预览分支（document → PDF 预览）。
      static const QSet<QString> kWellish = {QStringLiteral("well_head"),
                                             QStringLiteral("well_log"),
                                             QStringLiteral("unknown")};
      if (kWellish.contains(r.classifiedType))
        disp = QStringLiteral("reference");
    }
    if (disp.isEmpty())
      disp = QStringLiteral("unknown");
    r.displayType = disp;
    r.typeVocab = vocab;
    if (!r.classifiedType.isEmpty() && !vocab.contains(r.classifiedType))
      r.typeVocab.append(r.classifiedType); // 词表外类型（未来扩展）保住可选
    r.typeEditable = !r.skipped && !isFixedAuxiliaryPath(r.path);
  };
  // 归位预览（方向 30）：plan 期身份匹配 → 确认表「实体」列的预显文案。
  // well_head 是建井来源：entityId 恒空——匹配到既有井显示「井 X（既有）」，
  // 否则「新井」；井类行零匹配 =「未决（名字）」；歧义如实标注不猜。
  const auto entityPreviewFor = [](const PlannedItem &item) {
    if (item.entityAmbiguous)
      return QStringLiteral("歧义: %1").arg(item.entityName);
    if (item.type == QLatin1String("well_head"))
      return item.entityName.isEmpty()
                 ? QStringLiteral("新井")
                 : QStringLiteral("井 %1（既有）").arg(item.entityName);
    if (!item.entityId.isEmpty())
      return item.entityName.isEmpty() ? item.entityId : item.entityName;
    if (item.entityType == QLatin1String("well"))
      return item.entityName.isEmpty() ? QString()
                                       : QStringLiteral("未决（%1）").arg(item.entityName);
    return item.entityName;
  };
  for (const PlannedItem &item : plan.items)
  {
    FolderPreviewRow r;
    r.path = item.path;
    r.classifiedType = item.type;
    r.decision = item.decision; // plan 期决策（重复→跳过等）随行进确认表
    r.sizeBytes = item.size;    // T2 大小估算（族项 = 主件字节）
    r.entityPreview = entityPreviewFor(item);
    fillDisplay(r);
    rows.append(r);
  }
  for (const PlannedItem &s : plan.skipped)
  {
    FolderPreviewRow r;
    r.path = s.path;
    r.classifiedType = s.type;
    r.skipped = true;
    r.skipReason = s.note;
    r.sizeBytes = s.size; // 枚举期跳过行也带大小（用户看得见它占了多少）
    fillDisplay(r);
    rows.append(r);
  }
  return rows;
}

QVector<FolderRowResult>
DataImportService::importFolder(const QString &dirPath, QString *error)
{
  return importFolder(dirPath, error, QMap<QString, QString>{}, {}, {});
}

QVector<FolderRowResult>
DataImportService::importFolder(
    const QString &dirPath, QString *error,
    const QMap<QString, QString> &typeOverrides,
    const std::function<bool(int, int, const QString &)> &progress)
{
  return importFolder(dirPath, error, typeOverrides, QStringList{}, progress);
}

QVector<FolderRowResult>
DataImportService::importFolder(
    const QString &dirPath, QString *error,
    const QMap<QString, QString> &typeOverrides,
    const QStringList &forceImportPaths,
    const std::function<bool(int, int, const QString &)> &progress)
{
  if (error)
    error->clear(); // 成功路径不写 error——先清掉调用方复用的旧值
  const auto s = runInline([&](ImportSession &sess) {
    produceFolder(sess, dirPath, typeOverrides, forceImportPaths, progress);
  });
  if (!s)
  {
    setError(error, offThreadError());
    return {};
  }
  setError(error, s->error);
  return s->rows;
}

void DataImportService::produceFolder(
    ImportSession &sess, const QString &dirPath, const QMap<QString, QString> &typeOverrides,
    const QStringList &forceImportPaths,
    const std::function<bool(int, int, const QString &)> &progress)
{
  sess.rows.clear();
  sess.error.clear();
  const auto finish = [&sess](QVector<FolderRowResult> rows, const QString &err) {
    sess.rows = std::move(rows);
    sess.error = err;
  };
  QVector<FolderRowResult> rows;
  if (!sess.wired)
    return finish(rows, QStringLiteral("import service is not fully wired"));
  if (sess.projectDir.isEmpty())
    return finish(rows, QStringLiteral("project dir is not set"));
  QString errText;
  QString *const error = &errText;

  if (dirPath.isEmpty() || !QFileInfo(dirPath).isDir())
    return finish(rows, QStringLiteral("找不到目录: %1").arg(dirPath));

  // C 包三段式：T2 起 plan 经 COW 快照在当前线程构建（扫描/分类/shp 归组/
  // 身份匹配/去重/主建议不再 marshal 回 GUI）。扫描期进度复用 progress 回
  // 调——total=0 标记扫描段（调用方区分跑马灯/行进度）。阶段 1 全部
  // well_head（井建齐）、阶段 2 其余——序在 builder 里已排好。
  IngestPlan plan;
  if (progress)
  {
    const auto scanCb = [&progress](int seen, const QString &p) {
      return progress(seen, 0, p); // total=0：plan 扫描段
    };
    plan = planFor(sess, dirPath, scanCb);
  }
  else
    plan = planFor(sess, dirPath);
  if (plan.cancelled)
  {
    // 扫描段取消：零行执行零行入库（执行段取消另有「保留已处理行」语义）。
    return finish(rows, QStringLiteral("已取消"));
  }
  if (plan.items.isEmpty() && plan.skipped.isEmpty())
  {
    return finish(rows, plan.issues.isEmpty()
                            ? QStringLiteral("目录里没有可导入的文件: %1").arg(dirPath)
                            : plan.issues.join(QStringLiteral("\n")));
  }

  // 确认表「改类型」：合法 override 并进 item.type（生效类型）；非法 override
  // 忽略——不落进资产类型。D5：阶段归属按生效类型算——改成 well_head 的行
  // 回阶段 1，井建得够早排前的阶段 2 行仍能挂上。
  bool retype = false;
  for (PlannedItem &item : plan.items)
  {
    const QString o = typeOverrides.value(item.path);
    if (isClassifierType(o) && o != item.type)
    {
      item.type = o;
      retype = true;
    }
  }
  if (retype)
    orderIngestPlanItems(plan.items);

  // 「仍导入」改判（T2）：只翻 plan 期 sha 重复行（decision==skip 且带
  // duplicateOfVersionId）；枚举期跳过行（软链逃逸等）不在用户改判面。
  if (!forceImportPaths.isEmpty())
    for (PlannedItem &item : plan.items)
      if (forceImportPaths.contains(item.path) && item.decision == QLatin1String("skip") &&
          !item.duplicateOfVersionId.isEmpty())
      {
        item.decision = QStringLiteral("as_new_version");
        item.note =
            (item.note.isEmpty() ? QString() : item.note + QStringLiteral("；")) +
            QStringLiteral("用户改判：仍导入");
      }

  // 执行面：executeIngestPlan 逐项 produceItem（写 staging 副本）+ progress
  // 协作取消；入库在 owner 线程 commitImport 一次事务落盘。幂等——
  // decision==skip 或 (path,sha) 已注册的项不再登记；重跑同目录每行
  // Skipped、目录零增量。
  rows = executeIngestPlan(plan, sess, progress, error);
  finish(std::move(rows), errText);
}

// ---------------------------------------------------------------------------
// C 包执行面：单条 plan 项 → 行结果（executeIngestPlan 逐项调它；确认表
// 「重试」经 folderRowFor 合成的单项也走这里）。FolderRowResult::
// classifiedType 记生效类型（override 已并进 item.type），与确认表显示一致。
// ---------------------------------------------------------------------------

FolderRowResult
DataImportService::executePlannedItem(const PlannedItem &item, QString *error)
{
  if (error)
    error->clear();
  FolderRowResult row;
  QString perr;
  const auto s = runInline([&](ImportSession &sess) {
    row = produceItem(sess, item, &perr);
    sess.rows = {row};
    sess.error = perr;
  });
  if (!s)
  {
    setError(error, offThreadError());
    row = FolderRowResult{};
    row.path = item.path;
    row.classifiedType = item.type;
    row.outcome = FolderRowResult::Outcome::Failed;
    row.message = offThreadError();
    return row;
  }
  // 提交失败会把行改写成 Failed（failSession）。
  row = s->rows.value(0, row);
  setError(error, s->error);
  return row;
}


FolderRowResult
DataImportService::produceItem(ImportSession &s, const PlannedItem &item, QString *error)
{
  if (error)
    error->clear();
  DataCatalog *const cat = s.cat.get();
  FolderRowResult row;
  row.path = item.path;
  row.classifiedType = item.type;
  const auto appendStoredWellReport = [&](const CatalogVersion &version) {
    const QString summary = version.extra.value(QStringLiteral("wellParseReport"))
                                .toMap().value(QStringLiteral("summary")).toString();
    if (!summary.isEmpty())
      row.message += QCoreApplication::translate("DataImportService", "；已有版本：%1").arg(summary);
  };

  // T33 符号链接 TOCTOU 终验：真正读字节的是 importOneFile 里的 open/hash/
  // copy——入它之前再核一次：路径仍解析到枚举时判定的同一 canonical、仍是
  // 普通文件；被改指向的链接/被换掉的文件如实跳过。
  if (!item.canonicalPath.isEmpty())
  {
    const QFileInfo now(item.path);
    if (!now.isFile() || now.canonicalFilePath() != item.canonicalPath)
    {
      row.outcome = FolderRowResult::Outcome::Skipped;
      row.message = QStringLiteral(
          "文件在导入前已变化（符号链接改指向或不再是普通文件），已跳过");
      return row;
    }
  }

  if (item.decision == QLatin1String("skip"))
  {
    row.outcome = FolderRowResult::Outcome::Skipped;
    row.message = !item.duplicateOfVersionId.isEmpty()
                      ? QStringLiteral("重复→跳过")
                      : (item.note.isEmpty() ? QStringLiteral("已跳过")
                                             : item.note);
    // shp 族补齐：重复跳过的族顺带把缺的成员拷进已注册版本目录（幂等修复）。
    if (!item.members.isEmpty() && !item.duplicateOfVersionId.isEmpty())
      copyBundleMembersIntoVersion(s, item, item.duplicateOfVersionId, &row.message);
    if (!item.duplicateOfVersionId.isEmpty())
      appendStoredWellReport(cat->versionById(item.duplicateOfVersionId));
    return row;
  }

  // 幂等：同一（源路径, sha256）已注册 → 不再导入。as_new_version 有意绕过
  // ——显式「仍导入」的同字节结局交内部 dedup（AlreadyStored + 补挂）。
  // 路径按字符串口径比对（不解析符号链接）：link 路径≠目标路径，mirror 文件
  // 仍走 dedup 结局行，与旧行为一致。
  if (!item.sha256.isEmpty() && item.decision != QLatin1String("as_new_version"))
  {
    const CatalogVersion hit =
        cat->versionBySha256(item.sha256);
    if (!hit.id.isEmpty() &&
        QDir::cleanPath(hit.sourceUri) == QDir::cleanPath(item.path))
    {
      row.outcome = FolderRowResult::Outcome::Skipped;
      row.message = QStringLiteral("已在库，幂等跳过");
      if (!item.members.isEmpty())
        copyBundleMembersIntoVersion(s, item, hit.id, &row.message);
      appendStoredWellReport(hit);
      return row;
    }
  }

  QString ferr;
  ImportOptions opts;
  opts.forceType = item.type; // 恒下传——与分类器同值时是无害同值
  const ImportResult res = importOneFile(s, item.path, opts, &ferr);
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
  struct ResolveSummary
  {
    QStringList names;
    QStringList notes;
    int resolved = 0;
  };
  ResolveSummary summary;
  for (const EntityAssetLink &l : cat->linksForAsset(res.assetId))
  {
    if (l.unresolved)
    {
      if (!l.note.isEmpty() && !summary.notes.contains(l.note))
        summary.notes.append(l.note);
      continue;
    }
    ++summary.resolved;
    const CatalogEntity e = cat->entityById(l.entityId);
    const QString n = e.name.isEmpty() ? l.entityId : e.name;
    if (!n.isEmpty() && !summary.names.contains(n))
      summary.names.append(n);
  }
  row.entityName = summary.names.join(QStringLiteral(", "));
  row.outcome = res.outcome == ImportOutcome::Imported && summary.resolved == 0
                    ? FolderRowResult::Outcome::Unresolved
                    : FolderRowResult::Outcome::Imported;
  if (!summary.notes.isEmpty())
    row.message = row.message.isEmpty()
                      ? summary.notes.join(QStringLiteral("；"))
                      : row.message + QStringLiteral("；") +
                            summary.notes.join(QStringLiteral("；"));

  // shp 族：主件入库成功后把其余成员拷进同一受管 RAW 版本目录（外链版本的
  // 成员本就在源目录相邻，copyBundleMembersIntoVersion 自己不拷）。
  if (!item.members.isEmpty() && res.outcome == ImportOutcome::Imported)
  {
    CatalogVersion raw;
    for (const CatalogVersion &v : cat->versionsForAsset(res.assetId))
      if (v.stage == QLatin1String("RAW") && v.managed)
      {
        raw = v;
        break;
      }
    copyBundleMembersIntoVersion(s, item, raw.id, &row.message);
  }
  return row;
}

// shp 族成员落位：把 members 里除主件外的文件拷进指定受管版本目录。
// 幂等——已存在同名成员跳过（受管文件只读，不覆盖）；失败成员名附进

// *messageOut。非受管版本（外链）不拷：成员文件本就在源目录与主件相邻。
void DataImportService::copyBundleMembersIntoVersion(ImportSession &s, const PlannedItem &item,
                                                     const QString &versionId,
                                                     QString *messageOut)
{
  if (item.members.size() < 2 || versionId.isEmpty())
    return;
  const CatalogVersion v = s.cat->versionById(versionId);
  if (v.id.isEmpty() || !v.managed)
    return;
  // 本 session 新建的版本落在暂存根（随提交一起 rename）；已入库版本是
  // 幂等补齐缺失成员（只增不改，与旧行为一致）。
  const QString abs = s.cat->versionFilePath(v);
  if (abs.isEmpty())
    return;
  const QDir dir = QFileInfo(abs).absoluteDir();
  QStringList failed;
  for (const QString &member : item.members)
  {
    if (QDir::cleanPath(member) == QDir::cleanPath(item.path))
      continue; // 主件已是版本文件本体
    const QString dst = dir.filePath(QFileInfo(member).fileName());
    if (QFileInfo::exists(dst))
      continue; // 幂等：已在位不重拷
    if (!QFile::copy(member, dst))
      failed.append(QFileInfo(member).fileName());
    else
      QFile::setPermissions(dst, QFileDevice::ReadOwner | QFileDevice::ReadUser |
                                 QFileDevice::ReadGroup | QFileDevice::ReadOther);
  }
  if (!failed.isEmpty() && messageOut)
    *messageOut += (messageOut->isEmpty() ? QString() : QStringLiteral("；")) +
                   QStringLiteral("族成员复制失败: ") +
                   failed.join(QStringLiteral(", "));
}

// 「重试」/兼容入口合成 1 项：决策恒 as_new_version——显式单文件导入的语义
// 是「仍导入」，同字节结局由内部 dedup 消化（AlreadyStored + 补挂），

// registered-check 对它不生效。
FolderRowResult
DataImportService::folderRowFor(const QString &path, const QString &classifiedType,
                                const QString &effectiveType)
{
  Q_UNUSED(classifiedType); // 生效类型已含 override 口径，分类器原值不再用
  PlannedItem item;
  item.path = path;
  item.type = effectiveType;
  item.decision = QStringLiteral("as_new_version");
  return executePlannedItem(item, nullptr);
}

FolderRowResult
DataImportService::importFolderRow(const QString &sourcePath, const QString &forceType,
                                   QString *error)
{
  if (error)
    error->clear();
  // 与 plan 枚举同一分类口径：.xml 要看内容判定。
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
