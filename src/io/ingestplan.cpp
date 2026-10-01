// 层：数据
#include "ingestplan.h"

#include "shacache.h"

#include "lasparser.h"
#include "../domain/projectclassifier.h"
#include "wellfileparsers.h"
#include "../metadata/paleoprojectfile.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QSet>
#include <QThread>

#include <algorithm>
#include <type_traits>
#include <utility>

// ---------------------------------------------------------------------------
// IngestPlan 三段式（C 包，见 ingestplan.h 头注）。buildIngestPlan 是纯函数：
// 枚举/分类/归组/身份匹配/去重/主建议全部只读。catalog 只读面有两个实现
// （T2）：活对象包装仅限 catalog 线程；COW 快照在 catalog 线程拷出后任意
// 线程构建 plan——worker 上扫描/哈希不再 marshal 回 GUI。
// ---------------------------------------------------------------------------
namespace
{
  // >200MB 不做 plan 期哈希（SEG-Y：sha 留空、永不因 plan 去重拦截；执行期
  // importOneFile 内部照原口径流式算 sha/dedup）。
  constexpr qint64 kPlanHashLimitBytes = 200ll * 1024 * 1024;

  // 身份提取只读文件前缀——井名都在头部/数据行里，前缀足以提议身份；真导入
  // 仍整份解析。不封顶的话把 2MB+ 的 .dat 摆进语义目录会让「枚举」变成全量
  // 解析（tst_perfbudget::folderEnumerationBudget 钉的就是这条）。
  constexpr qint64 kIdentityPeekBytes = 128 * 1024;
  QByteArray readFilePrefix(const QString &path, qint64 maxBytes)
  {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
      return {};
    QByteArray buf = f.read(maxBytes);
    if (buf.size() == maxBytes) // 截到最后一行边界，不给解析器留半截行
    {
      const int nl = buf.lastIndexOf('\n');
      if (nl > 0)
        buf.truncate(nl);
    }
    return buf;
  }

  void setPlanError(QString *error, const QString &text)
  {
    if (error)
      *error = text;
  }

  QString readFileOrEmpty(const QString &path)
  {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
      return QString();
    return QString::fromUtf8(f.readAll());
  }

  // catalog marshal：与 DataImportService::catInvoke 同一纪律——catalog 只被
  // 它自己的线程触碰。自由函数摸不到服务的私有模板，按同一模式就地实现。
  template <typename Fn> auto onCatalogThread(DataCatalog *cat, Fn &&fn)
  {
    using R = std::invoke_result_t<Fn>;
    if (QThread::currentThread() == cat->thread())
    {
      if constexpr (std::is_void_v<R>)
      {
        fn();
        return;
      }
      else
        return fn();
    }
    if constexpr (std::is_void_v<R>)
      QMetaObject::invokeMethod(cat, std::forward<Fn>(fn),
                                Qt::BlockingQueuedConnection);
    else
    {
      R result{};
      QMetaObject::invokeMethod(cat, [&result, &fn] { result = fn(); },
                                Qt::BlockingQueuedConnection);
      return result;
    }
  }

  QString fileStem(const QString &path)
  {
    return QFileInfo(path).completeBaseName();
  }

  // ---- 枚举（与旧 collectFolderCandidates 同一口径）-----------------------
  //
  // 递归收普通文件：目录只下钻不出行；fifo/socket 等非普通文件、指向所选根
  // 目录之外的符号链接进 plan.skipped；隐藏文件不进表。被选目录包住工程
  // 目录时，工程产物子树不出行。
  void scanFolder(const QString &dirPath, const QString &projectDir, IngestPlan *plan)
  {
    const QFileInfo dirInfo(dirPath);
    const QString rootCanon = dirInfo.canonicalFilePath();
    if (rootCanon.isEmpty())
    {
      plan->issues.append(QStringLiteral("无法解析目录: %1").arg(dirPath));
      return;
    }
    const QString rootPrefix = rootCanon + QLatin1Char('/');
    const QString projectCanon = QFileInfo(projectDir).canonicalFilePath();
    const QString projectPrefix =
        projectCanon.isEmpty() ? QString() : projectCanon + QLatin1Char('/');
    // 根在工程目录之内（如选中了 artifacts/ 子目录）仍拒——把工程产物喂回来
    // 没有意义。
    if (!projectCanon.isEmpty() && rootCanon.startsWith(projectPrefix))
    {
      plan->issues.append(
          QStringLiteral("不能把工程目录内的子目录选作导入源: %1").arg(dirPath));
      return;
    }
    // 就地工程（源目录==工程根，「从工区文件夹新建」形态）：束成员与
    // artifacts/ 受管子树不当作源数据出行，其余文件照常分类。
    const bool inPlace = !projectCanon.isEmpty() && rootCanon == projectCanon;
    QSet<QString> bundleMembers;
    QString managedPrefix;
    if (inPlace)
    {
      const QDir pd(projectCanon);
      managedPrefix = pd.absoluteFilePath(QStringLiteral("artifacts")) +
                      QLatin1Char('/');
      bundleMembers.insert(
          pd.absoluteFilePath(QString::fromLatin1(PaleoProjectFile::kFileName)));
      bool ok = false;
      const PaleoProjectFile pf =
          readProjectFile(paleoProjectFilePath(projectCanon), &ok);
      if (ok)
        for (const QString &rel :
             {pf.qgz, pf.catalog, pf.manifest, pf.gpkg, pf.areaRules})
          if (!rel.isEmpty())
          {
            const QString abs = pd.absoluteFilePath(rel);
            bundleMembers.insert(abs);
            const QString cm = QFileInfo(abs).canonicalFilePath();
            if (!cm.isEmpty())
              bundleMembers.insert(cm);
          }
    }

    // 迭代器不带 FollowSymlinks：目录符号链接天然不下钻；文件符号链接用
    // canonical 判定是否逃出所选根目录。不带 Hidden：.preview_cache 之类不进表。
    QDirIterator it(dirInfo.absoluteFilePath(),
                    QDir::AllEntries | QDir::System | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext())
    {
      const QString path = it.next();
      const QFileInfo fi = it.fileInfo();
      const QString canon = fi.canonicalFilePath();
      if (!projectPrefix.isEmpty() && !canon.isEmpty() &&
          canon.startsWith(projectPrefix))
      {
        if (!inPlace)
          continue; // 工程产物子树不是源数据——跳过且不出行
        if (canon.startsWith(managedPrefix) || bundleMembers.contains(canon) ||
            bundleMembers.contains(fi.absoluteFilePath()))
          continue; // 就地工程：束成员/受管产物不出行
      }

      PlannedItem item;
      item.path = path;
      item.type = classifyProjectPath(path).type; // 跳过行也带分类器原类型
      if (fi.isSymLink() &&
          (canon.isEmpty() || (!canon.startsWith(rootPrefix) && canon != rootCanon)))
      {
        item.decision = QStringLiteral("skip");
        item.note = canon.isEmpty()
                        ? QStringLiteral("悬空符号链接，已跳过")
                        : QStringLiteral("符号链接指向所选目录之外，已跳过");
        plan->skipped.append(item);
        continue;
      }
      if (fi.isDir())
        continue; // 目录只用来下钻，自身不成项
      if (!fi.isFile())
      {
        item.decision = QStringLiteral("skip");
        item.note = QStringLiteral("不是普通文件，已跳过");
        plan->skipped.append(item);
        continue;
      }

      // .xml 看内容判定——与 importOneFile 同一分类口径；TOCTOU 复核同 T33：
      // 读字节前重取 canonical，目标被改指向就降级为跳过项。
      QByteArray xml;
      if (fi.suffix().compare(QLatin1String("xml"), Qt::CaseInsensitive) == 0)
      {
        const QFileInfo recheck(path);
        if (!recheck.isFile() || recheck.canonicalFilePath() != canon)
        {
          item.decision = QStringLiteral("skip");
          item.note = QStringLiteral("符号链接目标在枚举后已变化，已跳过");
          plan->skipped.append(item);
          continue;
        }
        xml = readFileOrEmpty(path).toUtf8();
      }
      const ProjectClassification cls = classifyProjectImport(path, xml);
      item.canonicalPath = canon;
      item.type = cls.type;
      item.format = cls.format;
      item.size = fi.size();
      plan->items.append(item);
    }
  }

  // ---- shp 族归组 ----------------------------------------------------------
  //
  // .shp/.shx/.dbf/.prj 同主名 → 单个 PlannedItem（主件 .shp，members 收全组
  // 排序路径）。必备件 .shp/.shx/.dbf 缺一不强行归组——各自成项如实单列。
  bool isShpMemberExt(const QString &ext)
  {
    return ext == QLatin1String("shp") || ext == QLatin1String("shx") ||
           ext == QLatin1String("dbf") || ext == QLatin1String("prj");
  }

  void groupShapefileFamilies(QVector<PlannedItem> &items)
  {
    QMap<QPair<QString, QString>, QVector<int>> groups; // (目录, 主名) → 成员下标
    for (int i = 0; i < items.size(); ++i)
    {
      const QFileInfo fi(items.at(i).path);
      if (isShpMemberExt(fi.suffix().toLower()))
        groups[{fi.absolutePath(), fi.completeBaseName()}].append(i);
    }
    QSet<int> absorbed;
    for (auto it = groups.begin(); it != groups.end(); ++it)
    {
      const QVector<int> &idxs = it.value();
      bool hasShp = false, hasShx = false, hasDbf = false;
      int primary = -1;
      for (const int i : idxs)
      {
        const QString e = QFileInfo(items.at(i).path).suffix().toLower();
        hasShp = hasShp || e == QLatin1String("shp");
        hasShx = hasShx || e == QLatin1String("shx");
        hasDbf = hasDbf || e == QLatin1String("dbf");
        if (e == QLatin1String("shp"))
          primary = i;
      }
      if (!(hasShp && hasShx && hasDbf))
        continue;
      QStringList members;
      for (const int i : idxs)
        members.append(items.at(i).path);
      members.sort();
      items[primary].members = members; // 主件项保留自身 path/type
      for (const int i : idxs)
        if (i != primary)
          absorbed.insert(i);
    }
    if (absorbed.isEmpty())
      return;
    QVector<PlannedItem> kept;
    kept.reserve(items.size() - absorbed.size());
    for (int i = 0; i < items.size(); ++i)
      if (!absorbed.contains(i))
        kept.append(items.at(i));
    items = kept;
  }

  // ---- 角色/身份匹配 --------------------------------------------------------

  // 与 importOneFile 落链接用的角色同一词表。
  QString roleForType(const QString &type)
  {
    if (type == QLatin1String("well_head"))
      return QStringLiteral("well_head");
    if (type == QLatin1String("well_log"))
      return QStringLiteral("well_log");
    if (type == QLatin1String("well_stratification"))
      return QStringLiteral("tops");
    if (type == QLatin1String("time_depth"))
      return QStringLiteral("time_depth");
    if (type == QLatin1String("horizon"))
      return QStringLiteral("horizon");
    if (type == QLatin1String("seismic"))
      return QStringLiteral("seismic_volume");
    return QStringLiteral("reference");
  }

  // 与 resolveWell 同口径：按序尝试候选名——恰好 1 个匹配即绑定；0 个换下
  // 一个名字；≥2 个标 entityAmbiguous 不再往后试（歧义不猜）。
  void proposeWell(PlannedItem &item, const QStringList &tried,
                   const IngestCatalogSource &catalog)
  {
    item.entityType = QStringLiteral("well");
    QStringList names;
    for (const QString &n : tried)
      if (!n.trimmed().isEmpty() && !names.contains(n))
        names.append(n);
    for (const QString &n : names)
    {
      const QStringList ids = catalog.wellsMatchingName(n);
      if (ids.size() == 1)
      {
        item.entityId = ids.front();
        const CatalogEntity e = catalog.entityById(ids.front());
        item.entityName = e.name.isEmpty() ? n : e.name;
        return;
      }
      if (ids.size() >= 2)
      {
        item.entityAmbiguous = true;
        item.entityName = n;
        item.note =
            (item.note.isEmpty() ? QString() : item.note + QStringLiteral("；")) +
            QStringLiteral("候选: ") + ids.join(QStringLiteral(", "));
        return;
      }
    }
    if (!names.isEmpty())
      item.entityName = names.front(); // 零匹配：名字留作未决展示，不建井
  }

  // 多井文件（井分层）：每个井名一条匹配——任一名字双候选 → 歧义；恰好一个
  // 唯一命中 → entityId；多命中 → entityId 留空、entityName 记已决名集合。
  void proposeWellMulti(PlannedItem &item, const QStringList &names,
                        const IngestCatalogSource &catalog)
  {
    item.entityType = QStringLiteral("well");
    QStringList resolvedIds, resolvedNames, unmatched;
    for (const QString &n : names)
    {
      const QStringList ids = catalog.wellsMatchingName(n);
      if (ids.size() >= 2)
      {
        item.entityAmbiguous = true;
        if (item.note.isEmpty())
          item.note =
              QStringLiteral("候选: ") + ids.join(QStringLiteral(", "));
        continue;
      }
      if (ids.size() == 1)
      {
        if (!resolvedIds.contains(ids.front()))
        {
          resolvedIds.append(ids.front());
          const CatalogEntity e = catalog.entityById(ids.front());
          resolvedNames.append(e.name.isEmpty() ? n : e.name);
        }
      }
      else
      {
        const QString nn = DataCatalog::normalizeWellName(n);
        if (!nn.isEmpty() && !unmatched.contains(nn))
          unmatched.append(nn);
      }
    }
    if (!resolvedIds.isEmpty())
      item.entityId = resolvedIds.front(); // 代表性已决实体：归组/未决计数用
    item.entityName = resolvedNames.isEmpty()
                          ? unmatched.join(QStringLiteral(", "))
                          : resolvedNames.join(QStringLiteral(", "));
  }

  // 井口文件是建井来源：plan 期不做实体归属（entityId 恒空），只如实检测
  // 歧义——文件内规范化重名行 / 同时匹配两口已有井的行都标 entityAmbiguous。
  void proposeWellHead(PlannedItem &item, const QString &path,
                       const IngestCatalogSource &catalog)
  {
    item.entityType = QStringLiteral("well");
    const QByteArray prefix = readFilePrefix(path, kIdentityPeekBytes);
    if (prefix.isEmpty())
      return; // 读不了：导入期如实失败，plan 不猜
    const QVector<WellHeadRecord> rows = parseWellHeadText(prefix);
    QHash<QString, int> normCount;
    for (const WellHeadRecord &r : rows)
      normCount[DataCatalog::normalizeWellName(r.name)] += 1;
    QStringList resolvedNames;
    for (const WellHeadRecord &r : rows)
    {
      const QString norm = DataCatalog::normalizeWellName(r.name);
      if (normCount.value(norm) >= 2)
      {
        item.entityAmbiguous = true;
        continue;
      }
      const QStringList ids = catalog.wellsMatchingName(r.name);
      if (ids.size() >= 2)
      {
        item.entityAmbiguous = true;
        continue;
      }
      if (ids.size() == 1)
      {
        const CatalogEntity e = catalog.entityById(ids.front());
        const QString n = e.name.isEmpty() ? r.name : e.name;
        if (!resolvedNames.contains(n))
          resolvedNames.append(n);
      }
    }
    item.entityName = resolvedNames.join(QStringLiteral(", "));
  }

  // 测井/时深的井名提取与 importOneFile 同序：LAS 读 ~W WELL、时深读 # Well
  // 行，零候选才回退文件名主名。
  void proposeWellLog(PlannedItem &item, const IngestCatalogSource &catalog)
  {
    QString wellName;
    if (item.format == QLatin1String("las"))
      LasParser::readWellInfo(item.path, wellName);
    proposeWell(item, {wellName, fileStem(item.path)}, catalog);
  }

  void proposeTimeDepth(PlannedItem &item, const IngestCatalogSource &catalog)
  {
    QString name = fileStem(item.path);
    const QByteArray prefix = readFilePrefix(item.path, kIdentityPeekBytes);
    if (!prefix.isEmpty())
    {
      const TimeDepthTable td = parseTimeDepthText(prefix); // '# Well :' 在头部
      if (!td.wellName.isEmpty())
        name = td.wellName;
    }
    proposeWell(item, {name, fileStem(item.path)}, catalog);
  }

  void proposeStratification(PlannedItem &item, const IngestCatalogSource &catalog)
  {
    QStringList names;
    const QByteArray prefix = readFilePrefix(item.path, kIdentityPeekBytes);
    for (const WellTopRecord &t : parseWellTopsText(prefix))
      if (!names.contains(t.wellName))
        names.append(t.wellName);
    if (names.size() == 1)
      proposeWell(item, {names.front(), fileStem(item.path)}, catalog); // 单井主名回退
    else
      proposeWellMulti(item, names, catalog);
  }

  void proposeIdentity(PlannedItem &item, const IngestCatalogSource &catalog)
  {
    item.role = roleForType(item.type);
    const QString stem = fileStem(item.path);
    if (item.type == QLatin1String("well_log"))
      proposeWellLog(item, catalog);
    else if (item.type == QLatin1String("time_depth"))
      proposeTimeDepth(item, catalog);
    else if (item.type == QLatin1String("well_stratification"))
      proposeStratification(item, catalog);
    else if (item.type == QLatin1String("well_head"))
      proposeWellHead(item, item.path, catalog);
    else if (item.type == QLatin1String("horizon"))
    {
      item.entityType = QStringLiteral("sequence_boundary");
      item.entityId = QStringLiteral("sb-%1").arg(stem.toUpper());
      item.entityName = stem.toUpper();
    }
    else if (item.type == QLatin1String("seismic"))
    {
      item.entityType = QStringLiteral("seismic_survey");
      item.entityId = QStringLiteral("survey-%1").arg(stem);
      item.entityName = stem;
    }
    else
    {
      // document / image_reference / geojson / tabular / unknown / 参考：
      // aux 实体 id 导入时才分配——plan 期留空。
      item.entityType = QStringLiteral("auxiliary");
      item.entityName = stem;
    }
  }
} // namespace

IngestPlan buildIngestPlan(const QString &root, const DataCatalog &catalog)
{
  // 旧签名 = 活对象零拷贝直通（仅 catalog 线程；GUI 直调等价旧路径）。
  const LiveCatalogSource live(&catalog);
  return buildIngestPlan(root, live);
}

IngestPlan buildIngestPlan(const QString &root, const IngestCatalogSource &catalog,
                           const IngestScanProgress &scanProgress)
{
  IngestPlan plan;
  plan.root = root;
  bool cancelled = false;
  const auto reportScan = [&cancelled, &scanProgress](int seen,
                                                      const QString &path) {
    if (!scanProgress || cancelled)
      return;
    if (!scanProgress(seen, path))
      cancelled = true;
  };
  const QFileInfo rootInfo(root);
  if (rootInfo.isFile())
  {
    // 单文件入口同一 builder：一项 plan（分类/身份/去重/主建议全走）。
    PlannedItem item;
    item.path = rootInfo.absoluteFilePath();
    item.canonicalPath = rootInfo.canonicalFilePath();
    QByteArray xml;
    if (rootInfo.suffix().compare(QLatin1String("xml"), Qt::CaseInsensitive) == 0)
      xml = readFileOrEmpty(item.path).toUtf8();
    const ProjectClassification cls = classifyProjectImport(item.path, xml);
    item.type = cls.type;
    item.format = cls.format;
    item.size = rootInfo.size();
    plan.items.append(item);
  }
  else if (rootInfo.isDir())
  {
    const QString projectDir = catalog.isOpen() ? catalog.projectDir() : QString();
    scanFolder(root, projectDir, &plan);
    groupShapefileFamilies(plan.items);
    orderIngestPlanItems(plan.items);
    std::sort(plan.skipped.begin(), plan.skipped.end(),
              [](const PlannedItem &a, const PlannedItem &b) {
                return a.path < b.path;
              });
  }
  else
  {
    plan.issues.append(QStringLiteral("找不到源文件: %1").arg(root));
    return plan;
  }
  if (cancelled)
  {
    plan.cancelled = true;
    plan.issues.append(QStringLiteral("已取消"));
    return plan;
  }
  if (plan.items.isEmpty() && plan.skipped.isEmpty() && plan.issues.isEmpty())
    plan.issues.append(QStringLiteral("目录里没有可导入的文件: %1").arg(root));

  // ---- plan 期 sha256：≤200MB 才算；SEG-Y 留空、永不因 plan 去重拦截。
  // 哈希是 plan 期的主要重活——每个待哈希文件回调一次扫描进度（枚举本身
  // 只 stat，不报；>200MB 不哈希的文件无回调，重活在执行段另有行进度）。
  for (PlannedItem &item : plan.items)
  {
    if (item.size <= 0 || item.size > kPlanHashLimitBytes)
      continue;
    reportScan(plan.items.size(), item.path);
    if (cancelled)
      break;
    QString herr;
    item.sha256 = ShaCache::shared().sha256Hex(item.path, &herr); // D7.7
    if (item.sha256.isEmpty())
      plan.issues.append(QFileInfo(item.path).fileName() +
                         (herr.isEmpty() ? QStringLiteral(" 哈希失败") : herr));
  }
  if (cancelled)
  {
    plan.cancelled = true;
    plan.issues.append(QStringLiteral("已取消"));
    return plan;
  }

  // ---- 身份匹配（只读 catalog；歧义/零匹配如实未决，不猜）----
  for (PlannedItem &item : plan.items)
    proposeIdentity(item, catalog);

  // ---- sha 去重：catalog.versionBySha256 命中 → duplicateOf + decision=skip
  // （确认框可改；同一 sha 只查一次）。快照路径下重哈希发生在调用线程
  // （worker）——这正是 plan 期要搬出 GUI 的重活。
  QHash<QString, QString> dupVersionBySha; // sha → versionId（缓存含空命中）
  for (PlannedItem &item : plan.items)
  {
    if (item.sha256.isEmpty())
      continue;
    if (!dupVersionBySha.contains(item.sha256))
      dupVersionBySha.insert(item.sha256,
                             catalog.versionBySha256(item.sha256).id);
    const QString vid = dupVersionBySha.value(item.sha256);
    if (!vid.isEmpty())
    {
      item.duplicateOfVersionId = vid;
      item.decision = QStringLiteral("skip");
      item.note =
          (item.note.isEmpty() ? QString() : item.note + QStringLiteral("；")) +
          QStringLiteral("重复：与已注册版本 %1 字节相同").arg(vid);
    }
  }

  // ---- 主关联建议：同 (entity,role) 首成员 → true；该角色槽在 catalog 已被
  // 已决链接占用的项不建议（首个占用者保持其主关联）。
  QSet<QString> seen;
  for (PlannedItem &item : plan.items)
  {
    item.suggestedPrimary = false;
    if (item.entityId.isEmpty() || item.decision != QLatin1String("accept"))
      continue;
    const QString key = item.entityType + QLatin1Char('|') + item.entityId +
                        QLatin1Char('|') + item.role;
    if (seen.contains(key))
      continue;
    bool occupied = false;
    for (const EntityAssetLink &l : catalog.linksForEntity(item.entityId))
      if (l.role == item.role && !l.unresolved)
      {
        occupied = true;
        break;
      }
    if (!occupied)
    {
      item.suggestedPrimary = true;
      seen.insert(key);
    }
  }
  return plan;
}

void orderIngestPlanItems(QVector<PlannedItem> &items)
{
  // 两阶段序：生效类型 well_head 且非固定辅助 → 阶段 1（井先建齐）；其余
  // 阶段 2；各阶段内按路径排——与旧 orderFolderCandidates/importFolder 同序。
  QVector<PlannedItem> ordered;
  for (int phase = 0; phase < 2; ++phase)
  {
    QVector<PlannedItem> bucket;
    for (const PlannedItem &it : items)
    {
      const bool phase0 = it.type == QLatin1String("well_head") &&
                          !isFixedAuxiliaryPath(it.path);
      if ((phase == 0) == phase0)
        bucket.append(it);
    }
    std::sort(bucket.begin(), bucket.end(),
              [](const PlannedItem &a, const PlannedItem &b) {
                return a.path < b.path;
              });
    ordered += bucket;
  }
  items = ordered;
}

// ---------------------------------------------------------------------------
// IngestCatalogSource 两个实现（T2：plan 期搬出 GUI 线程）
// ---------------------------------------------------------------------------

namespace
{
// catalogPath = <projectDir>/artifacts/metadata/catalog.json——上两级即工程
// 目录。目录推不出来（相对路径/未 open）→ 空，工程子树守卫随之关闭（与旧
// collectFolderCandidates 拿到空 projectDir 的行为一致）。
QString projectDirOfCatalogPath(const QString &catalogPath)
{
  QDir d = QFileInfo(catalogPath).absoluteDir(); // …/artifacts/metadata
  if (!d.cdUp() || !d.cdUp())
    return QString();
  return d.absolutePath();
}

// linksForEntity 的 ordinal 稳定排序（WP2 起快照走命中行集索引后仍是唯一
// 排序口径；不同角色保持入库序，ordinal 全 0 时与排序前逐项一致）。
} // namespace

bool LiveCatalogSource::isOpen() const { return m_cat && m_cat->isOpen(); }

QString LiveCatalogSource::projectDir() const
{
  return m_cat ? projectDirOfCatalogPath(m_cat->catalogPath()) : QString();
}

QStringList LiveCatalogSource::wellsMatchingName(const QString &name) const
{
  return m_cat ? m_cat->wellsMatchingName(name) : QStringList();
}

CatalogEntity LiveCatalogSource::entityById(const QString &id) const
{
  return m_cat ? m_cat->entityById(id) : CatalogEntity();
}

CatalogVersion LiveCatalogSource::versionBySha256(const QString &sha256) const
{
  return m_cat ? m_cat->versionBySha256(sha256) : CatalogVersion();
}

QVector<EntityAssetLink> LiveCatalogSource::linksForEntity(const QString &entityId) const
{
  return m_cat ? m_cat->linksForEntity(entityId) : QVector<EntityAssetLink>();
}

CatalogReadSnapshot CatalogReadSnapshot::fromCatalog(const DataCatalog &catalog)
{
  // 只在 catalog 线程调用。QVector 隐式共享 → 三张表 O(1) 拷出；此后
  // catalog 线程的 append/修改走 COW 分离，快照持有构建瞬间的视图。
  CatalogReadSnapshot snap;
  snap.m_open = catalog.isOpen();
  snap.m_dir = snap.m_open ? projectDirOfCatalogPath(catalog.catalogPath())
                           : QString();
  snap.m_projectDir = snap.m_dir;
  if (snap.m_open)
  {
    snap.m_entities = catalog.entities();
    snap.m_versions = catalog.versions();
    snap.m_links = catalog.links();
    // WP2：一次 O(N) 建命中索引——plan 构建期逐项查询不再每次线性扫表。
    snap.m_rowsBySha.reserve(snap.m_versions.size());
    snap.m_rowByEntityId.reserve(snap.m_entities.size());
    snap.m_rowsByEntityId.reserve(snap.m_links.size());
    for (int i = 0; i < snap.m_versions.size(); ++i)
      if (!snap.m_versions.at(i).sha256.isEmpty())
        snap.m_rowsBySha[snap.m_versions.at(i).sha256.toLower()].append(i);
    for (int i = 0; i < snap.m_entities.size(); ++i)
    {
      const CatalogEntity &e = snap.m_entities.at(i);
      // 首个重复 id 行优先——与快照旧线性扫描的「第一个匹配」一致（重复 id
      // 只会来自手改 catalog.json，addEntity 有 dup 门；live 索引同款 insert
      // 覆盖语义差异在此不可观察）。规范化井名为空的井不入索引：旧扫描
      // 对「查询名规范化为空」恒回零匹配，登记空键会让 "-" 这类井名被
      // 空查询误命中（review P2）。
      if (!e.id.isEmpty() && !snap.m_rowByEntityId.contains(e.id))
        snap.m_rowByEntityId.insert(e.id, i);
      if (e.entityType == QStringLiteral("well") && !e.name.isEmpty())
      {
        const QString norm = DataCatalog::normalizeWellName(e.name);
        if (!norm.isEmpty())
          snap.m_wellIdsByNormName[norm].append(e.id);
      }
    }
    for (int i = 0; i < snap.m_links.size(); ++i)
      if (!snap.m_links.at(i).entityId.isEmpty())
        snap.m_rowsByEntityId[snap.m_links.at(i).entityId].append(i);
  }
  return snap;
}

QStringList CatalogReadSnapshot::wellsMatchingName(const QString &name) const
{
  // 命中序 = 表序（与 DataCatalog::wellsMatchingName 的行扫描序一致）。
  // 空规范化（查询名只含空格/'-'/'_'）恒零匹配——与 live 版同一守卫。
  const QString needle = DataCatalog::normalizeWellName(name);
  if (needle.isEmpty())
    return {};
  return m_wellIdsByNormName.value(needle);
}

CatalogEntity CatalogReadSnapshot::entityById(const QString &id) const
{
  const int row = m_rowByEntityId.value(id, -1);
  return row >= 0 ? m_entities.at(row) : CatalogEntity();
}

CatalogVersion CatalogReadSnapshot::versionBySha256(const QString &sha256) const
{
  // 与 DataCatalog::versionBySha256 同口径：命中后复核文件仍在且字节一致
  // （受管文件丢失/被改的旧条目不冒充命中）——文件 IO 在调用线程执行。
  // 行集升序＝表序，「第一个匹配」语义与旧线性扫描一致（WP2 索引化）。
  if (sha256.isEmpty())
    return CatalogVersion();
  const QVector<int> rows = m_rowsBySha.value(sha256.toLower());
  for (int r : rows)
  {
    const CatalogVersion &v = m_versions.at(r);
    const QString path = DataCatalog::resolvedVersionPath(m_dir, v);
    if (path.isEmpty() || !QFileInfo(path).isFile())
      continue;
    if (ShaCache::shared().sha256Hex(path).compare(sha256, Qt::CaseInsensitive) == 0) // D7.7
      return v;
  }
  return CatalogVersion();
}

QVector<EntityAssetLink> CatalogReadSnapshot::linksForEntity(const QString &entityId) const
{
  // WP2：命中行集走索引（升序＝表序）；ordinal 稳定排序语义与
  // linksForEntityFrom 的全表扫描版一致（audit row 35：空 id 回空集）。
  if (entityId.isEmpty())
    return {};
  const QVector<int> rows = m_rowsByEntityId.value(entityId);
  QVector<EntityAssetLink> out;
  out.reserve(rows.size());
  for (int r : rows)
    out.append(m_links.at(r));
  std::stable_sort(out.begin(), out.end(),
                   [](const EntityAssetLink &a, const EntityAssetLink &b) {
                     return a.ordinal < b.ordinal;
                   });
  return out;
}

QVector<FolderRowResult>
executeIngestPlan(const IngestPlan &plan, DataImportService &svc,
                  const IngestProgress &progress, QString *error)
{
  if (error)
    error->clear();
  QVector<FolderRowResult> rows;
  DataCatalog *cat = svc.catalog();
  if (!cat)
  {
    setPlanError(error, QStringLiteral("catalog unavailable"));
    return rows;
  }

  // T33/audit row 37 口径不变：整个 plan 执行并成一个落盘批次——每项 ~5 次
  // 全量 JSON 序列化收敛成一次 save() + 一次 changed()。BatchSave 的 RAII 摸
  // catalog 私有态——构造/析构都 marshal 回 catalog 所在线程。
  auto *batch = onCatalogThread(
      cat, [&] { return new DataCatalog::BatchSave(cat); });

  bool cancelled = false;
  int doneCount = 0;
  for (const PlannedItem &item : plan.items)
  {
    QString ierr;
    rows.append(svc.executePlannedItem(item, &ierr));
    ++doneCount;
    if (progress &&
        !progress(doneCount, static_cast<int>(plan.items.size()), item.path))
    {
      cancelled = true;
      break; // 协作取消：已处理的行保留，未处理的不再动
    }
  }

  // 枚举期跳过项（逃逸链接/非普通文件）如实记 Skipped 行缀在最后。
  for (const PlannedItem &s : plan.skipped)
  {
    FolderRowResult row;
    row.path = s.path;
    row.classifiedType = s.type;
    row.outcome = FolderRowResult::Outcome::Skipped;
    row.message = s.note;
    rows.append(row);
  }

  // 批次结算：析构即 flush——marshal 回 catalog 线程销毁（落盘失败如实写
  // error；行里的 Imported 结局不变——内存态已是入库态，磁盘失败要 surfaced）。
  QString berr;
  const bool flushed = onCatalogThread(cat, [&] {
    const bool ok = batch->flush(&berr);
    delete batch;
    return ok;
  });
  if (!flushed)
  {
    qWarning("executeIngestPlan: catalog batch save failed: %s",
             qPrintable(berr));
    setPlanError(error, berr.isEmpty()
                            ? QStringLiteral("catalog batch save failed")
                            : berr);
  }
  if (cancelled)
    setPlanError(error, QStringLiteral("已取消（已入库的行保留）"));
  return rows;
}
