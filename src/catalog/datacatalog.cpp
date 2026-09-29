// 层：数据
#include "datacatalog.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QHash>
#include <QSaveFile>
#include <QSet>

#include "../metadata/atomicfile.h"

#include <algorithm>
#include <cmath>

namespace
{
  const int kSchemaVersion = 1;

  // wave/data-integrity：role 词表诊断标记。addLink/attachLink 把诊断写进
  // note 尾部，invalidRoleLinks() 按标记扫描——诊断因此随 catalog.json
  // round-trip，重开工程后诊断面仍可查。
  const QString kUnknownRoleMark = QStringLiteral("未知角色: ");
  const QString kRoleTypeMismatchMark = QStringLiteral("角色与实体类型不符: ");

  // 诚实降级：role 不在词表、或实体类型不在该 role 的 entityTypes——不拦
  // 不丢不改词（词表是工程自定义的，project_area.json 可扩；硬拦会把合法
  // 自定义挡在旧二进制外），只产出诊断文本（无违例回空）。
  // 空 entityType（调用方未给）或词表未声明挂接域（自定义角色可省
  // entity_types）无从核对，不诊断。
  QString roleDiagnosis(const EntityAssetLink &l, const RoleRegistry &roles)
  {
    const RoleDef *def = roles.find(l.role);
    if (!def)
      return kUnknownRoleMark + l.role;
    if (!l.entityType.isEmpty() && !def->entityTypes.isEmpty() &&
        !def->entityTypes.contains(l.entityType))
      return kRoleTypeMismatchMark +
             QStringLiteral("%1 于 %2").arg(l.role, l.entityType);
    return QString();
  }

  // 把诊断追加到 note 尾部（已有 note 用「；」连接）并 qWarning 一次。
  void annotateRoleDiagnostics(EntityAssetLink &l, const RoleRegistry &roles)
  {
    const QString diagnosis = roleDiagnosis(l, roles);
    if (diagnosis.isEmpty())
      return;
    l.note = l.note.isEmpty() ? diagnosis
                              : l.note + QStringLiteral("；") + diagnosis;
    qWarning() << "catalog:" << diagnosis;
  }

  void setError(QString *error, const QString &text)
  {
    if (error)
      *error = text;
  }

  QJsonArray cornersToJson(const QVector<QPair<double, double>> &cs)
  {
    QJsonArray a;
    for (const auto &c : cs)
    {
      QJsonObject o;
      o.insert(QStringLiteral("x"), c.first);
      o.insert(QStringLiteral("y"), c.second);
      a.append(o);
    }
    return a;
  }

  QVector<QPair<double, double>> cornersFromJson(const QJsonArray &a)
  {
    QVector<QPair<double, double>> cs;
    for (const auto &v : a)
    {
      const QJsonObject o = v.toObject();
      cs.append({o.value(QStringLiteral("x")).toDouble(), o.value(QStringLiteral("y")).toDouble()});
    }
    return cs;
  }

  QJsonObject entityToJson(const CatalogEntity &e)
  {
    QJsonObject o;
    o.insert(QStringLiteral("id"), e.id);
    o.insert(QStringLiteral("entity_type"), e.entityType);
    o.insert(QStringLiteral("name"), e.name);
    o.insert(QStringLiteral("surface_x"), e.surfaceX);
    o.insert(QStringLiteral("surface_y"), e.surfaceY);
    o.insert(QStringLiteral("has_surface"), e.hasSurface);
    o.insert(QStringLiteral("kb"), e.kb);
    o.insert(QStringLiteral("td"), e.td);
    o.insert(QStringLiteral("coordinate_status"), e.coordinateStatus);
    o.insert(QStringLiteral("inline_min"), e.inlineMin);
    o.insert(QStringLiteral("inline_max"), e.inlineMax);
    o.insert(QStringLiteral("xline_min"), e.xlineMin);
    o.insert(QStringLiteral("xline_max"), e.xlineMax);
    o.insert(QStringLiteral("sample_interval_us"), e.sampleIntervalUs);
    o.insert(QStringLiteral("start_time_ms"), e.startTimeMs);
    o.insert(QStringLiteral("corners"), cornersToJson(e.corners));
    if (!e.extra.isEmpty())
      o.insert(QStringLiteral("extra"), QJsonObject::fromVariantMap(e.extra));
    return o;
  }

  CatalogEntity entityFromJson(const QJsonObject &o)
  {
    CatalogEntity e;
    e.id = o.value(QStringLiteral("id")).toString();
    e.entityType = o.value(QStringLiteral("entity_type")).toString();
    e.name = o.value(QStringLiteral("name")).toString();
    // D12：旧 catalog 的 "uwi"/"aliases" 键不读不报错——留在这里被静默丢弃。
    e.surfaceX = o.value(QStringLiteral("surface_x")).toDouble();
    e.surfaceY = o.value(QStringLiteral("surface_y")).toDouble();
    e.hasSurface = o.value(QStringLiteral("has_surface")).toBool();
    e.kb = o.value(QStringLiteral("kb")).toDouble();
    e.td = o.value(QStringLiteral("td")).toDouble();
    e.coordinateStatus = o.value(QStringLiteral("coordinate_status")).toString();
    e.inlineMin = o.value(QStringLiteral("inline_min")).toDouble();
    e.inlineMax = o.value(QStringLiteral("inline_max")).toDouble();
    e.xlineMin = o.value(QStringLiteral("xline_min")).toDouble();
    e.xlineMax = o.value(QStringLiteral("xline_max")).toDouble();
    e.sampleIntervalUs = o.value(QStringLiteral("sample_interval_us")).toDouble();
    e.startTimeMs = o.value(QStringLiteral("start_time_ms")).toDouble();
    e.corners = cornersFromJson(o.value(QStringLiteral("corners")).toArray());
    e.extra = o.value(QStringLiteral("extra")).toObject().toVariantMap();
    return e;
  }

  QJsonObject linkToJson(const EntityAssetLink &l)
  {
    QJsonObject o;
    o.insert(QStringLiteral("entity_type"), l.entityType);
    o.insert(QStringLiteral("entity_id"), l.entityId);
    o.insert(QStringLiteral("asset_id"), l.assetId);
    o.insert(QStringLiteral("role"), l.role);
    o.insert(QStringLiteral("is_primary"), l.isPrimary);
    o.insert(QStringLiteral("unresolved"), l.unresolved);
    o.insert(QStringLiteral("ordinal"), l.ordinal); // 恒写——与上游 C++ 落盘约定一致
    o.insert(QStringLiteral("note"), l.note);
    return o;
  }

  EntityAssetLink linkFromJson(const QJsonObject &o)
  {
    EntityAssetLink l;
    l.entityType = o.value(QStringLiteral("entity_type")).toString();
    l.entityId = o.value(QStringLiteral("entity_id")).toString();
    l.assetId = o.value(QStringLiteral("asset_id")).toString();
    l.role = o.value(QStringLiteral("role")).toString();
    l.isPrimary = o.value(QStringLiteral("is_primary")).toBool(true);
    l.unresolved = o.value(QStringLiteral("unresolved")).toBool(false);
    l.ordinal = o.value(QStringLiteral("ordinal")).toInt(0); // 旧 catalog 无键 → 0
    l.note = o.value(QStringLiteral("note")).toString();
    return l;
  }

  QJsonObject versionToJson(const CatalogVersion &v)
  {
    QJsonObject o;
    o.insert(QStringLiteral("id"), v.id);
    o.insert(QStringLiteral("asset_id"), v.assetId);
    o.insert(QStringLiteral("stage"), v.stage);
    o.insert(QStringLiteral("version_number"), v.versionNumber);
    o.insert(QStringLiteral("managed"), v.managed);
    o.insert(QStringLiteral("path"), v.path);
    o.insert(QStringLiteral("source_uri"), v.sourceUri);
    o.insert(QStringLiteral("sha256"), v.sha256);
    o.insert(QStringLiteral("file_name"), v.fileName);
    o.insert(QStringLiteral("parent_version_ids"), QJsonArray::fromStringList(v.parentVersionIds));
    if (!v.extra.isEmpty())
      o.insert(QStringLiteral("extra"), QJsonObject::fromVariantMap(v.extra));
    return o;
  }

  CatalogVersion versionFromJson(const QJsonObject &o)
  {
    CatalogVersion v;
    v.id = o.value(QStringLiteral("id")).toString();
    v.assetId = o.value(QStringLiteral("asset_id")).toString();
    v.stage = o.value(QStringLiteral("stage")).toString();
    v.versionNumber = o.value(QStringLiteral("version_number")).toInt(1);
    v.managed = o.value(QStringLiteral("managed")).toBool(true);
    v.path = o.value(QStringLiteral("path")).toString();
    v.sourceUri = o.value(QStringLiteral("source_uri")).toString();
    v.sha256 = o.value(QStringLiteral("sha256")).toString();
    v.fileName = o.value(QStringLiteral("file_name")).toString();
    for (const auto &p : o.value(QStringLiteral("parent_version_ids")).toArray())
      v.parentVersionIds.append(p.toString());
    v.extra = o.value(QStringLiteral("extra")).toObject().toVariantMap();
    return v;
  }

  // §3 段校验（addVersion 与 catalog 装载共用）：fileName/stage/受管 path 的
  // 每一段都须过 DataCatalog::isSafePathSegment；返回出问题的那段描述，
  // 干净版本回空串。（外链 path 是文件系统绝对路径，含分隔符属正常，不查；
  // 空 path 表示未落位，交给调用方兜底。）
  QString unsafeVersionSegmentReason(const CatalogVersion &v)
  {
    if (!v.fileName.isEmpty() && !DataCatalog::isSafePathSegment(v.fileName))
      return QStringLiteral("file name: %1").arg(v.fileName);
    if (!v.stage.isEmpty() && !DataCatalog::isSafePathSegment(v.stage))
      return QStringLiteral("stage: %1").arg(v.stage);
    if (v.managed && !v.path.isEmpty())
      for (const QString &seg : v.path.split(QLatin1Char('/')))
        if (!DataCatalog::isSafePathSegment(seg))
          return QStringLiteral("managed path segment: %1").arg(seg);
    return QString();
  }
} // namespace

DataCatalog::DataCatalog(QObject *parent)
  : QObject(parent)
{
}

bool DataCatalog::ensureOpen(QString *error) const
{
  if (m_isOpen)
    return true;
  setError(error, QStringLiteral("catalog is not open"));
  return false;
}

bool DataCatalog::open(const QString &projectDir, QString *error)
{
  m_isOpen = false;
  m_openError.clear();
  m_recoveredFromBackup = false; // 恢复态是「本次 open」的属性，重开重新判
  m_backupRecoveryReason.clear();
  // 注意：m_lockedReadOnly 不在此重置——实例级只读降级由拥有者管理（见头注）。
  m_batchDepth = 0;
  m_batchDirty = false;
  m_dir = projectDir.trimmed().isEmpty() ? QString() : projectDir;
  m_revision = 0;
  m_entities.clear();
  m_assets.clear();
  m_versions.clear();
  m_links.clear();
  m_assetSeq = m_versionSeq = 0;
  m_roles = RoleRegistry::defaults();
  m_idx.clear();

  const auto fail = [&](const QString &msg) {
    setError(error, msg);
    m_openError = msg; // 拒绝写入态的原因留存——openError() 供 UI 展示
    return false;
  };

  if (m_dir.isEmpty())
    return fail(QStringLiteral("project directory is empty"));

  // A 包角色词表：<projectDir>/project_area.json 的 roles 节做工程级覆盖；
  // 缺文件/解析失败/roles 非对象 → 静默留 defaults()。词表不是数据底座，
  // 缺它绝不阻塞工程打开（catalog.json 的成败不受影响）。
  QFile areaFile(m_dir + QStringLiteral("/project_area.json"));
  if (areaFile.open(QIODevice::ReadOnly))
  {
    const QJsonDocument areaDoc = QJsonDocument::fromJson(areaFile.readAll());
    if (areaDoc.isObject())
    {
      const QJsonValue roles = areaDoc.object().value(QStringLiteral("roles"));
      if (roles.isObject())
        m_roles = RoleRegistry::fromJson(roles.toObject());
    }
  }

  QFile f(catalogPath());
  if (!f.exists())
  {
    // 初始化空 catalog（schema_version + revision 0）。落盘失败同样进
    // 拒绝写入态——否则后续 mutator 会拿内存态反复尝试覆盖。
    QString serr;
    m_isOpen = true;
    if (save(&serr))
      return true;
    m_isOpen = false;
    return fail(serr.isEmpty() ? QStringLiteral("cannot initialize catalog") : serr);
  }

  // 装载一段已解析的 JSON 根（主文件与 .bak 回退共用）：schema 校验 +
  // 四表读入 + 序号恢复。返回空串 = 成功；非空 = 拒因。
  const auto loadFrom = [this](const QJsonObject &root) -> QString {
    // T20b：缺键按当前版本处理（旧 catalog 照常打开）；显式写了且不等于
    // kSchemaVersion（无论新旧）→ 如实拒绝，不读不写。
    const QString schemaKey = QStringLiteral("schema_version");
    if (root.contains(schemaKey) && root.value(schemaKey).toInt() != kSchemaVersion)
      return QStringLiteral("unsupported catalog schema");
    m_revision = root.value(QStringLiteral("catalog_revision")).toInt();
    for (const auto &v : root.value(QStringLiteral("entities")).toArray())
      m_entities.append(entityFromJson(v.toObject()));
    for (const auto &v : root.value(QStringLiteral("assets")).toArray())
    {
      const CatalogAsset a{
          v.toObject().value(QStringLiteral("id")).toString(),
          v.toObject().value(QStringLiteral("type")).toString(),
          v.toObject().value(QStringLiteral("format")).toString(),
          v.toObject().value(QStringLiteral("display_name")).toString()};
      m_assets.append(a);
      bool ok = false;
      const int n = QString(a.id).mid(4).toInt(&ok); // "ast-N"
      if (ok)
        m_assetSeq = qMax(m_assetSeq, n);
    }
    for (const auto &v : root.value(QStringLiteral("versions")).toArray())
    {
      const CatalogVersion cv = versionFromJson(v.toObject());
      // 段校验（fileName/stage/受管 path）与 resolvedVersionPath 根包含检查：
      // 坏段版本如实跳过且不写回，不静默沿用——与 addVersion 同一校验面
      // （audit row 36/T33；resolvedVersionPath 另挡符号链接与越界绝对路径）。
      QString badSeg = unsafeVersionSegmentReason(cv);
      // D5.7（10k 资产打开预算）：装载期只做段校验——段已排除 ".."/绝对/
      // 反斜杠，无符号链接不可能逃出根；符号链接逃逸检查推迟到
      // resolvedVersionPath() 访问期（那里本来就查）。装载不再做 N 次文件
      // 系统 stat（10k 版本 × 4 段 = 4 万次 stat 是打开预算的大头）。
      if (!badSeg.isEmpty())
      {
        qWarning("catalog: skipping version %s with unsafe path segment: %s",
                 qPrintable(cv.id), qPrintable(badSeg));
        continue;
      }
      m_versions.append(cv);
      bool ok = false;
      const int n = cv.id.startsWith(QStringLiteral("ver-")) ? cv.id.mid(4).toInt(&ok) : 0;
      if (ok && n > 0)
        m_versionSeq = qMax(m_versionSeq, n);
    }
    for (const auto &v : root.value(QStringLiteral("entity_asset_links")).toArray())
      m_links.append(linkFromJson(v.toObject()));
    m_idx.rebuild(m_entities, m_assets, m_versions, m_links); // D5.1
    return QString();
  };

  if (!f.open(QIODevice::ReadOnly))
    return fail(QStringLiteral("cannot open catalog %1").arg(catalogPath()));
  QJsonParseError pe;
  const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
  const bool mainParses = pe.error == QJsonParseError::NoError && doc.isObject();
  const QString schemaKey = QStringLiteral("schema_version");
  const bool mainSchemaOk =
      !mainParses || !doc.object().contains(schemaKey) ||
      doc.object().value(schemaKey).toInt() == kSchemaVersion;
  if (mainParses && mainSchemaOk)
  {
    const QString rerr = loadFrom(doc.object());
    if (!rerr.isEmpty())
      return fail(QStringLiteral("%1 in %2").arg(rerr, catalogPath()));
    m_isOpen = true;
    emit changed();
    return true;
  }

  // ---- 腐败恢复（.bak 回退）：只对「主文件解析失败」生效。schema 不匹配
  // 是未来版本信号——.bak 与主文件同代，回退既救不了也不该静默降级数据。
  if (mainParses)
    return fail(QStringLiteral("unsupported catalog schema in %1").arg(catalogPath()));
  const QString bakPath = catalogPath() + QStringLiteral(".bak");
  QFile bak(bakPath);
  QString bakDetail;
  if (bak.exists() && bak.open(QIODevice::ReadOnly))
  {
    QJsonParseError bpe;
    const QJsonDocument bakDoc = QJsonDocument::fromJson(bak.readAll(), &bpe);
    bak.close();
    if (bpe.error == QJsonParseError::NoError && bakDoc.isObject())
    {
      const QString rerr = loadFrom(bakDoc.object());
      if (rerr.isEmpty())
      {
        // 恢复成功：open 算成功（读面可用、可续存），损坏事实如实留底并
        // 广播——UI/状态面向用户告警；下一次 save 会把恢复后的内容轮转成
        // 新 .bak，主文件重写为好文件。
        m_recoveredFromBackup = true;
        m_backupRecoveryReason = QStringLiteral("%1: %2").arg(catalogPath(), pe.errorString());
        qWarning("catalog: primary %s is corrupt (%s) — recovered from %s",
                 qPrintable(catalogPath()), qPrintable(pe.errorString()), qPrintable(bakPath));
        m_isOpen = true;
        emit backupRecovered(m_backupRecoveryReason);
        emit changed();
        return true;
      }
      bakDetail = rerr;
    }
    else
    {
      bakDetail = QStringLiteral("corrupt backup: ").append(bpe.errorString());
    }
  }
  return fail(QStringLiteral("corrupt catalog %1: %2 (no usable %3: %4)")
                  .arg(catalogPath(), pe.errorString(), bakPath,
                       bakDetail.isEmpty() ? QStringLiteral("backup missing") : bakDetail));
}

bool DataCatalog::save(QString *error)
{
  if (!ensureOpen(error))
    return false;
  // 单写实例降级（§6）：锁在别的实例手里——本实例只读，任何落盘如实拒绝。
  if (m_lockedReadOnly)
  {
    setError(error, QStringLiteral("工程目录被另一个实例锁定——本实例只读，catalog 写入被拒绝"));
    return false;
  }
  // 批量作用域内：只记脏、不落盘——endBatch 统一结算（audit row 37）。
  if (m_batchDepth > 0)
  {
    m_batchDirty = true;
    return true;
  }
  const QDir dir = QFileInfo(catalogPath()).dir();
  if (!dir.exists() && !dir.mkpath(QStringLiteral(".")))
  {
    setError(error, QStringLiteral("cannot create catalog directory %1").arg(dir.absolutePath()));
    return false;
  }

  QJsonObject root;
  root.insert(QStringLiteral("schema_version"), kSchemaVersion);
  const int nextRevision = m_revision + 1;
  root.insert(QStringLiteral("catalog_revision"), nextRevision);
  QJsonArray ents, asts, vers, lnks;
  for (const CatalogEntity &e : m_entities) ents.append(entityToJson(e));
  for (const CatalogAsset &a : m_assets)
  {
    QJsonObject o;
    o.insert(QStringLiteral("id"), a.id);
    o.insert(QStringLiteral("type"), a.type);
    o.insert(QStringLiteral("format"), a.format);
    o.insert(QStringLiteral("display_name"), a.displayName);
    asts.append(o);
  }
  for (const CatalogVersion &v : m_versions) vers.append(versionToJson(v));
  for (const EntityAssetLink &l : m_links) lnks.append(linkToJson(l));
  root.insert(QStringLiteral("entities"), ents);
  root.insert(QStringLiteral("assets"), asts);
  root.insert(QStringLiteral("versions"), vers);
  root.insert(QStringLiteral("entity_asset_links"), lnks);

  // QSaveFile writes beside the destination and replaces it only after a full
  // successful write, preserving the last good catalog on short writes.
  QSaveFile f(catalogPath());
  f.setDirectWriteFallback(false);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
  {
    setError(error, QStringLiteral("cannot write %1: %2").arg(catalogPath(), f.errorString()));
    return false;
  }
  // §9 回滚 + D5.6 备份轮转：替换前把现存 catalog 轮转一份 .bak——QSaveFile
  // 防写一半，.bak 防「写成功了但内容是错的」需要一个上一代可回退。
  // 轮转保留 backupKeepCount() 代：.bak（最新）→ .bak.2 → … → .bak.N；
  // open() 的腐败回退只读 .bak（最新一代）。
  if (QFile::exists(catalogPath()))
  {
    // 代命名：gen1 = catalog.json.bak（最新），gen g>=2 = catalog.json.bak.g。
    // keep=K：删除第 K 代 → 2..K-1 依次后移 → .bak 升为 .bak.2 → 现行内容成新 .bak。
    const QString bak = catalogPath() + QStringLiteral(".bak");
    const int keep = qBound(1, m_backupKeep, 9);
    if (keep == 1)
    {
      // 只留一代：全部扩展代清空。
      for (int g = 2; g <= 9; ++g)
        QFile::remove(bak + QStringLiteral(".%1").arg(g));
    }
    else
    {
      QFile::remove(bak + QStringLiteral(".%1").arg(keep));
      for (int gen = keep - 1; gen >= 2; --gen)
        if (QFile::exists(bak + QStringLiteral(".%1").arg(gen)))
          paleoReplaceFile(bak + QStringLiteral(".%1").arg(gen),
                           bak + QStringLiteral(".%1").arg(gen + 1));
      if (QFile::exists(bak))
        paleoReplaceFile(bak, bak + QStringLiteral(".2"));
    }
    const QString bakTmp = bak + QStringLiteral(".tmp");
    QFile::remove(bakTmp);
    if (!QFile::copy(catalogPath(), bakTmp))
    {
      setError(error, QStringLiteral("cannot copy %1 to backup tmp %2").arg(catalogPath(), bakTmp));
      return false;
    }
    if (!paleoReplaceFile(bakTmp, bak))
    {
      QFile::remove(bakTmp);
      setError(error, QStringLiteral("cannot rotate %1 to %2").arg(catalogPath(), bak));
      return false;
    }
  }

  const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Indented);
  if (f.write(bytes) != bytes.size())
  {
    const QString detail = f.errorString();
    f.cancelWriting();
    setError(error, QStringLiteral("short write to %1: %2").arg(catalogPath(), detail));
    return false;
  }
  if (!f.commit())
  {
    setError(error, QStringLiteral("cannot replace %1: %2").arg(catalogPath(), f.errorString()));
    return false;
  }
  m_revision = nextRevision;
  emit changed();
  return true;
}

void DataCatalog::beginBatch()
{
  ++m_batchDepth;
}

bool DataCatalog::endBatch(QString *error)
{
  if (m_batchDepth <= 0)
    return true; // 配对失衡由调用方栈结构保证，不 noisy
  if (--m_batchDepth > 0)
    return true; // 嵌套批次：只有最外层结算
  if (!m_batchDirty)
    return true;
  m_batchDirty = false;
  return save(error);
}

DataCatalog::BatchSave::BatchSave(DataCatalog *catalog)
  : m_catalog(catalog)
{
  if (m_catalog)
    m_catalog->beginBatch();
}

DataCatalog::BatchSave::~BatchSave()
{
  flush();
}

bool DataCatalog::BatchSave::flush(QString *error)
{
  if (m_done || !m_catalog)
    return true;
  m_done = true;
  return m_catalog->endBatch(error);
}

bool DataCatalog::addEntity(const CatalogEntity &e, QString *error)
{
  if (!ensureOpen(error))
    return false;
  if (e.id.isEmpty() || hasEntity(e.id))
  {
    setError(error, QStringLiteral("entity id empty or duplicate: %1").arg(e.id));
    return false;
  }
  m_entities.append(e);
  m_idx.entityAdded(m_entities.size() - 1, e.id, e.entityType); // D5.2 增量
  if (save(error))
    return true;
  m_entities.removeLast();
  m_idx.rebuild(m_entities, m_assets, m_versions, m_links); // 回滚——罕见路径全量换正确性
  return false;
}

bool DataCatalog::addAsset(const CatalogAsset &a, QString *error)
{
  if (!ensureOpen(error))
    return false;
  if (a.id.isEmpty() || !assetById(a.id).id.isEmpty())
  {
    setError(error, QStringLiteral("asset id is empty or duplicate: %1").arg(a.id));
    return false;
  }
  // T17 同型：显式给的 "ast-N" 也推进序号——否则 nextAssetId() 回发已用 id。
  {
    bool ok = false;
    const int n = a.id.startsWith(QStringLiteral("ast-")) ? a.id.mid(4).toInt(&ok) : 0;
    if (ok && n > 0)
      m_assetSeq = qMax(m_assetSeq, n);
  }
  m_assets.append(a);
  m_idx.assetAdded(m_assets.size() - 1, a.id, a.type); // D5.2
  if (save(error))
    return true;
  m_assets.removeLast();
  m_idx.rebuild(m_entities, m_assets, m_versions, m_links);
  return false;
}

bool DataCatalog::addVersion(const CatalogVersion &v, QString *error)
{
  if (!ensureOpen(error))
    return false;
  if (v.id.isEmpty() || v.assetId.isEmpty())
  {
    setError(error, QStringLiteral("version id or asset id is empty"));
    return false;
  }
  if (!versionById(v.id).id.isEmpty())
  {
    setError(error, QStringLiteral("duplicate version id: %1").arg(v.id));
    return false;
  }
  if (assetById(v.assetId).id.isEmpty())
  {
    setError(error, QStringLiteral("version references unknown asset: %1").arg(v.assetId));
    return false;
  }
  // §3：fileName 与受管 path 的每一段都必须是合法路径段——catalog 不落坏段。
  // （外链 path 是文件系统绝对路径，含分隔符属正常，不查；空 path 表示未落位，
  // 交给调用方兜底。）
  if (!v.fileName.isEmpty() && !isSafePathSegment(v.fileName))
  {
    setError(error, QStringLiteral("unsafe path segment in version file name: %1").arg(v.fileName));
    return false;
  }
  if (!v.stage.isEmpty() && !isSafePathSegment(v.stage))
  {
    setError(error, QStringLiteral("unsafe path segment in version stage: %1").arg(v.stage));
    return false;
  }
  if (v.managed && !v.path.isEmpty())
  {
    for (const QString &seg : v.path.split(QLatin1Char('/')))
      if (!isSafePathSegment(seg))
      {
        setError(error, QStringLiteral("unsafe managed path segment: %1").arg(seg));
        return false;
      }
    if (resolvedVersionPath(m_dir, v).isEmpty())
    {
      setError(error, QStringLiteral("unsafe managed path: %1").arg(v.path));
      return false;
    }
  }
  // T17：显式给的 "ver-N" 也推进序号——不然 addVersion("ver-9") 之后
  // nextVersionId() 还会发 ver-9（被 dup 检查挡下报错）而不是发 ver-10。
  {
    bool ok = false;
    const int n = v.id.startsWith(QStringLiteral("ver-")) ? v.id.mid(4).toInt(&ok) : 0;
    if (ok && n > 0)
      m_versionSeq = qMax(m_versionSeq, n);
  }
  // 快照：staleness 标记与版本追加在同一原子写里同进同退——save 失败
  // 不留「版本回滚了但 stale 标记还在」的半截内存态（与 addLink 同一纪律）。
  const QVector<CatalogVersion> previousVersions = m_versions;
  m_versions.append(v);
  m_idx.versionAdded(m_versions.size() - 1, v); // D5.2
  // B 包 staleness-lite：新版本入库 = 同资产 versionNumber 更低的旧版本被
  // 取代（supersede）——其下游闭包中的 DERIVED 版本输入失效，随本次
  // addVersion 同一原子写落 extra["stale"]/["staleReason"]，不二次落盘。
  QStringList superseded;
  for (int r : m_idx.versionRowsForAsset(v.assetId)) // D5.1 O(命中集) 非全表
    if (m_versions.at(r).versionNumber < v.versionNumber)
      superseded.append(m_versions.at(r).id);
  for (const QString &pid : superseded)
    markStaleDownstreamOf(
        pid, QStringLiteral("上游版本 %1 已被同资产新版本 %2 取代").arg(pid, v.id));
  if (save(error))
    return true;
  m_versions = previousVersions;
  m_idx.versionsMutated(m_versions);
  return false;
}

bool DataCatalog::addLink(const EntityAssetLink &l, QString *error)
{
  if (!ensureOpen(error))
    return false;
  // §3 修订：未决链接实体 id 留空（资产保留、不建不并）；已决链接仍必须有实体 id。
  if (l.assetId.isEmpty())
  {
    setError(error, QStringLiteral("link needs an asset id"));
    return false;
  }
  if (l.entityId.isEmpty() && !l.unresolved)
  {
    setError(error, QStringLiteral("resolved link needs an entity id"));
    return false;
  }
  const QVector<EntityAssetLink> previousLinks = m_links;
  // 词表诊断先落 note（诚实降级——不拒收，见 invalidRoleLinks()）。
  EntityAssetLink stored = l;
  annotateRoleDiagnostics(stored, m_roles);
  m_links.append(stored);
  // §3：新的已决主关联入库后，同一 (entityType, entityId, role) 只保留这一条
  // 主关联——同角色旧主关联（例如同井同角色的旧版本资产）降级为非主。
  if (stored.isPrimary && !stored.unresolved)
    for (int i : m_idx.linkRowsForEntity(stored.entityId)) // D5.1 O(命中集)
      if (i != m_links.size() - 1 && m_links[i].isPrimary && !m_links[i].unresolved &&
          m_links[i].entityType == stored.entityType &&
          m_links[i].entityId == stored.entityId &&
          m_links[i].role == stored.role)
        m_links[i].isPrimary = false;
  if (save(error))
  {
    m_idx.linkAdded(m_links.size() - 1, m_links.last()); // D5.2：纯追加——旧主关联
    return true;                                          // 只降标志，邻接键不变
  }
  m_links = previousLinks;
  m_idx.linksMutated(m_links);
  return false;
}

bool DataCatalog::attachLink(int index, const QString &entityId, QString *error)
{
  if (!ensureOpen(error))
    return false;
  if (index < 0 || index >= m_links.size())
  {
    setError(error, QStringLiteral("link index out of range: %1").arg(index));
    return false;
  }
  const QVector<EntityAssetLink> previousLinks = m_links;
  EntityAssetLink &l = m_links[index];
  if (!l.unresolved)
  {
    setError(error, QStringLiteral("link %1 is not unresolved").arg(index));
    return false;
  }
  if (entityId.isEmpty())
  {
    setError(error, QStringLiteral("attach needs an entity id"));
    return false;
  }
  l.entityId = entityId;
  l.unresolved = false;
  l.isPrimary = true;
  l.note.clear();
  // 决议清空 note 后词表诊断重下——role 词表违例不因挂上实体而消失
  // （诚实降级，见 invalidRoleLinks()）。
  annotateRoleDiagnostics(l, m_roles);
  // 与 addLink 同一不变量：同一 (entityType, entityId, role) 只留这一条主关联。
  // 注意：未决链接也可能带 entityId（上游约定——entity 邻接先于决议建立），
  // 行集可能包含 index 自身，须显式排除。
  for (int i : m_idx.linkRowsForEntity(entityId)) // D5.1 O(命中集)
    if (i != index && m_links[i].isPrimary && !m_links[i].unresolved &&
        m_links[i].entityType == l.entityType && m_links[i].entityId == entityId &&
        m_links[i].role == l.role)
      m_links[i].isPrimary = false;
  if (save(error))
  {
    m_idx.linksMutated(m_links); // entityId 获值——entity 邻接变化
    return true;
  }
  m_links = previousLinks;
  m_idx.linksMutated(m_links);
  return false;
}

bool DataCatalog::setLinkUnresolved(int index, QString *error)
{
  if (!ensureOpen(error))
    return false;
  if (index < 0 || index >= m_links.size())
  {
    setError(error, QStringLiteral("link index out of range: %1").arg(index));
    return false;
  }
  const QVector<EntityAssetLink> previousLinks = m_links;
  EntityAssetLink &l = m_links[index];
  if (l.unresolved)
  {
    setError(error, QStringLiteral("link %1 is already unresolved").arg(index));
    return false;
  }
  // 回退未决：实体 id 清空、不再持主关联。资产与被共享的井实体保留（§3）。
  l.entityId.clear();
  l.unresolved = true;
  l.isPrimary = false;
  l.note.clear();
  if (save(error))
  {
    m_idx.linksMutated(m_links); // entityId 被清空——entity 邻接变化
    return true;
  }
  m_links = previousLinks;
  m_idx.linksMutated(m_links);
  return false;
}

bool DataCatalog::setLinkPrimary(int index, QString *error)
{
  if (!ensureOpen(error))
    return false;
  if (index < 0 || index >= m_links.size())
  {
    setError(error, QStringLiteral("link index out of range: %1").arg(index));
    return false;
  }
  const QVector<EntityAssetLink> previousLinks = m_links;
  EntityAssetLink &l = m_links[index];
  if (l.unresolved || l.entityId.isEmpty())
  {
    setError(error, QStringLiteral("link %1 is not a resolved link").arg(index));
    return false;
  }
  l.isPrimary = true;
  // 与 addLink/attachLink 同一不变量：同一 (entityType, entityId, role) 只留
  // 这一条主关联——同井同角色的旧版本资产降级为非主，不复制字节。
  // 行集是提升前的快照——index 行本就在 entity 行集里，须显式排除。
  for (int i : m_idx.linkRowsForEntity(l.entityId)) // D5.1 O(命中集)
    if (i != index && m_links[i].isPrimary && !m_links[i].unresolved &&
        m_links[i].entityType == l.entityType && m_links[i].entityId == l.entityId &&
        m_links[i].role == l.role)
      m_links[i].isPrimary = false;
  if (save(error))
  {
    // 同上：纯标志位变化——零重索引。
    return true;
  }
  m_links = previousLinks;
  m_idx.linksMutated(m_links);
  return false;
}

// ---- wave/data-integrity：role 词表违例诊断面 ----
// 带「未知角色: / 角色与实体类型不符: 」note 标记的链接（addLink/attachLink
// 的诚实降级写入）。按标记扫描 note：诊断随 catalog.json round-trip，重开
// 后仍可查；词表内的干净链接（含普通未决备注）不在其中。
QVector<EntityAssetLink> DataCatalog::invalidRoleLinks() const
{
  QVector<EntityAssetLink> out;
  for (const EntityAssetLink &l : m_links)
    if (l.note.contains(kUnknownRoleMark) || l.note.contains(kRoleTypeMismatchMark))
      out.append(l);
  return out;
}

CatalogVersion DataCatalog::versionBySha256(const QString &sha256) const
{
  if (sha256.isEmpty())
    return CatalogVersion();
  for (const CatalogVersion &v : m_versions)
    if (v.sha256.compare(sha256, Qt::CaseInsensitive) == 0)
    {
      const QString path = resolvedVersionPath(m_dir, v);
      if (path.isEmpty() || !QFileInfo(path).isFile()) continue;
      if (sha256FileHex(path).compare(sha256, Qt::CaseInsensitive) == 0)
        return v;
    }
  return CatalogVersion();
}

QString DataCatalog::resolvedVersionPath(const QString &projectDir, const CatalogVersion &version)
{
  if (version.path.isEmpty()) return QString();
  if (!version.managed) return version.path;
  if (QDir::isAbsolutePath(version.path) || version.path.contains(QLatin1Char('\\')))
    return QString();
  const QStringList segments = version.path.split(QLatin1Char('/'));
  for (const QString &segment : segments)
    if (!isSafePathSegment(segment)) return QString();

  const QString root = QFileInfo(projectDir).canonicalFilePath();
  if (root.isEmpty()) return QString();
  QString current = root;
  for (const QString &segment : segments)
  {
    current = QDir(current).filePath(segment);
    const QFileInfo info(current);
    if (info.isSymbolicLink()) return QString();
    if (info.exists())
    {
      const QString canonical = info.canonicalFilePath();
      if (!canonical.startsWith(root + QDir::separator())) return QString();
    }
  }
  return current;
}

QString DataCatalog::sha256FileHex(const QString &path, QString *error)
{
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly))
  {
    setError(error, QStringLiteral("cannot read %1").arg(path));
    return QString();
  }
  QCryptographicHash hash(QCryptographicHash::Sha256);
  // 64KB chunks: a 1MB stack buffer overflows the default Windows thread
  // stack when this runs on the QTest main thread.
  char buf[64 << 10];
  qint64 n = 0;
  while ((n = f.read(buf, sizeof(buf))) > 0)
    hash.addData(QByteArrayView(buf, static_cast<qsizetype>(n)));
  if (n < 0)
  {
    setError(error, QStringLiteral("read error on %1").arg(path));
    return QString();
  }
  return QString::fromLatin1(hash.result().toHex());
}

bool DataCatalog::verifyExternalVersionSha(const CatalogVersion &v, QString *error) const
{
  // 受管版本（自己写入的副本）与未留底的外链（旧 catalog）无从校验。
  if (v.managed || v.sha256.isEmpty())
    return true;
  QString herr;
  const QString current = sha256FileHex(v.path, &herr); // 外链 path 是绝对路径
  if (current.isEmpty())
  {
    setError(error, herr.isEmpty() ? QStringLiteral("cannot read %1").arg(v.path) : herr);
    return false;
  }
  if (current.compare(v.sha256, Qt::CaseInsensitive) != 0)
  {
    setError(error, QStringLiteral("源文件与入库时的 SHA-256 不一致"));
    return false;
  }
  return true;
}

bool DataCatalog::isSafePathSegment(const QString &segment)
{
  // §3：一段路径拒绝空段、"."、任何含 ".." 的段、斜杠/反斜杠、NUL 与控制字符
  //（含换行、回车、Tab、DEL）。
  if (segment.isEmpty() || segment == QLatin1Char('.') ||
      segment.contains(QLatin1String("..")))
    return false;
  for (const QChar c : segment)
  {
    if (c == QLatin1Char('/') || c == QLatin1Char('\\'))
      return false;
    const ushort u = c.unicode();
    if (u < 0x20 || u == 0x7F)
      return false;
  }
  return true;
}

bool DataCatalog::hasEntity(const QString &id) const
{
  return m_idx.entityRow(id) >= 0; // D5.1 O(1)
}

QVector<CatalogEntity> DataCatalog::entities(const QString &entityType) const
{
  QVector<CatalogEntity> out;
  if (entityType.isEmpty())
    return m_entities;
  const QVector<int> rows = m_idx.entityRowsByType(entityType); // D5.1
  out.reserve(rows.size());
  for (int r : rows)
    out.append(m_entities.at(r));
  return out;
}

CatalogEntity DataCatalog::entityById(const QString &id) const
{
  const int row = m_idx.entityRow(id); // D5.1 O(1)
  return row >= 0 ? m_entities.at(row) : CatalogEntity();
}

QVector<CatalogAsset> DataCatalog::assets() const
{
  return m_assets;
}

CatalogAsset DataCatalog::assetById(const QString &id) const
{
  const int row = m_idx.assetRow(id); // D5.1 O(1)
  return row >= 0 ? m_assets.at(row) : CatalogAsset();
}

QVector<CatalogVersion> DataCatalog::versionsForAsset(const QString &assetId) const
{
  QVector<CatalogVersion> out;
  const QVector<int> rows = m_idx.versionRowsForAsset(assetId); // D5.1 O(1)+收集
  out.reserve(rows.size());
  for (int r : rows)
    out.append(m_versions.at(r));
  return out;
}

CatalogVersion DataCatalog::versionById(const QString &id) const
{
  const int row = m_idx.versionRow(id); // D5.1 O(1)
  return row >= 0 ? m_versions.at(row) : CatalogVersion();
}

CatalogVersion DataCatalog::currentVersion(const QString &assetId) const
{
  CatalogVersion best;
  const QVector<int> rows = m_idx.versionRowsForAsset(assetId); // D5.1
  for (int r : rows)
    if (m_versions.at(r).versionNumber >= best.versionNumber)
      best = m_versions.at(r);
  return best;
}

QVector<EntityAssetLink> DataCatalog::linksForEntity(const QString &entityId) const
{
  // audit row 35：空 id 不等于「全部未决链接」——未决集合走 unresolvedLinks()；
  // 这里如实返回空集，不然调用方拿空串查询会静默命中全部未决链接。
  if (entityId.isEmpty())
    return {};
  QVector<EntityAssetLink> out;
  const QVector<int> rows = m_idx.linkRowsForEntity(entityId); // D5.1（行升序）
  out.reserve(rows.size());
  for (int r : rows)
    out.append(m_links.at(r));
  // B 包：同 (entity,role) 成员按 ordinal 升序展示。stable_sort 只按 ordinal
  // 排——不同角色间的相对序保持入库序；旧数据 ordinal 全 0 时输出与排序前
  // 完全一致，对既有消费方零扰动。
  std::stable_sort(out.begin(), out.end(),
                   [](const EntityAssetLink &a, const EntityAssetLink &b) {
                     return a.ordinal < b.ordinal;
                   });
  return out;
}

QVector<EntityAssetLink> DataCatalog::unresolvedLinks() const
{
  QVector<EntityAssetLink> out;
  for (const EntityAssetLink &l : m_links)
    if (l.unresolved)
      out.append(l);
  return out;
}

QVector<EntityAssetLink> DataCatalog::linksForAsset(const QString &assetId) const
{
  QVector<EntityAssetLink> out;
  const QVector<int> rows = m_idx.linkRowsForAsset(assetId); // D5.1（行升序）
  out.reserve(rows.size());
  for (int r : rows)
    out.append(m_links.at(r));
  return out;
}

QVector<EntityAssetLink> DataCatalog::links() const
{
  return m_links;
}

QVector<CatalogVersion> DataCatalog::downstreamClosure(const QString &versionId) const
{
  QVector<CatalogVersion> out;
  if (versionId.isEmpty())
    return out;
  // BFS：邻接表反查（D5.4 childrenByParent）——不再每层全表扫 m_versions。
  // 结果序 = BFS 发现序（frontier 序 × 子版本行序），与旧「全表扫描 × 表序」
  // 逐项一致（子版本按行序入边）。seen 先放种子——环（A→B→A）里种子不作为
  // 「自己的下游」进结果，也保证遍历终止。
  QSet<QString> seen;
  seen.insert(versionId);
  QStringList frontier{versionId};
  for (int head = 0; head < frontier.size(); ++head)
  {
    // 值拷贝：frontier.append 可能重分配 QList 存储，引用会悬垂。
    const QString cur = frontier.at(head);
    for (const QString &childId : m_idx.childVersionIds(cur))
    {
      if (seen.contains(childId))
        continue;
      const int row = m_idx.versionRow(childId);
      if (row < 0)
        continue;
      seen.insert(childId);
      frontier.append(childId);
      out.append(m_versions.at(row));
    }
  }
  return out;
}

int DataCatalog::markStaleDownstreamOf(const QString &versionId, const QString &reason)
{
  const QVector<CatalogVersion> downstream = downstreamClosure(versionId);
  QHash<QString, int> indexOf;
  indexOf.reserve(m_versions.size());
  for (int i = 0; i < m_versions.size(); ++i)
    indexOf.insert(m_versions[i].id, i);
  int changed = 0;
  for (const CatalogVersion &d : downstream)
  {
    // staleness-lite：只标 DERIVED 产物。闭包里的 INTERMEDIATE/OUTPUT 等
    // 参与溯源穿透（DERIVED 的祖先可以是任意阶段），但自身不记 stale。
    if (d.stage != QLatin1String("DERIVED"))
      continue;
    const int i = indexOf.value(d.id, -1);
    if (i < 0)
      continue; // 闭包快照自洽——防御性跳过
    CatalogVersion &m = m_versions[i];
    if (m.extra.value(QStringLiteral("stale")).toBool() &&
        m.extra.value(QStringLiteral("staleReason")).toString() == reason)
      continue; // 同一标记已在——不算变更（幂等，不空涨 revision）
    m.extra.insert(QStringLiteral("stale"), true);
    m.extra.insert(QStringLiteral("staleReason"), reason);
    ++changed;
  }
  return changed;
}

bool DataCatalog::markDownstreamStale(const QString &versionId, const QString &reason,
                                      QString *error)
{
  if (!ensureOpen(error))
    return false;
  if (versionId.isEmpty())
  {
    setError(error, QStringLiteral("cannot mark downstream of an empty version id"));
    return false;
  }
  if (versionById(versionId).id.isEmpty())
  {
    setError(error,
             QStringLiteral("cannot mark downstream of unknown version: %1").arg(versionId));
    return false;
  }
  const QString why =
      reason.isEmpty() ? QStringLiteral("上游版本源已失效") : reason;
  const QVector<CatalogVersion> previousVersions = m_versions;
  if (markStaleDownstreamOf(versionId, why) == 0)
    return true; // 无下游或标记未变——不落盘、不空涨 revision
  if (save(error))
    return true;
  m_versions = previousVersions; // 与 addLink 同一纪律：落盘失败回滚内存
  return false;
}

QString DataCatalog::normalizeWellName(const QString &name)
{
  QString out;
  out.reserve(name.size());
  for (const QChar c : name)
  {
    if (c.isSpace() || c == QLatin1Char('-') || c == QLatin1Char('_'))
      continue;
    out.append(c.toLower());
  }
  return out;
}

QStringList DataCatalog::wellsMatchingName(const QString &name) const
{
  const QString needle = normalizeWellName(name);
  QStringList out;
  if (needle.isEmpty())
    return out;
  const QVector<int> rows = m_idx.entityRowsByType(QStringLiteral("well")); // D5.1
  for (int r : rows)
  {
    // D12：井身份只走规范化 name——uwi/别名匹配已随字段一并剥离。
    if (normalizeWellName(m_entities.at(r).name) == needle)
      out.append(m_entities.at(r).id);
  }
  return out;
}

QString DataCatalog::managedPath(const QString &stage, const QString &assetId,
                                 const QString &versionId, const QString &fileName)
{
  // §3：任一段不是合法路径段就回空串——catalog 拒绝产出坏路径。
  if (!isSafePathSegment(stage) || !isSafePathSegment(assetId) ||
      !isSafePathSegment(versionId) || !isSafePathSegment(fileName))
    return QString();
  return QStringLiteral("%1/%2/%3/%4").arg(stage.toLower(), assetId, versionId, fileName);
}

QString DataCatalog::nextAssetId()
{
  QString id;
  do { id = QStringLiteral("ast-%1").arg(++m_assetSeq); }
  while (!assetById(id).id.isEmpty());
  return id;
}

QString DataCatalog::nextVersionId()
{
  QString id;
  do { id = QStringLiteral("ver-%1").arg(++m_versionSeq); }
  while (!versionById(id).id.isEmpty());
  return id;
}

QString DataCatalog::nextEntityId(const QString &prefix)
{
  int max = 0;
  for (const CatalogEntity &e : m_entities)
  {
    if (!e.id.startsWith(prefix + QLatin1Char('-')))
      continue;
    bool ok = false;
    const int n = e.id.mid(prefix.size() + 1).toInt(&ok);
    if (ok)
      max = qMax(max, n);
  }
  return QStringLiteral("%1-%2").arg(prefix).arg(max + 1);
}

bool DataCatalog::writeWellsGeoJson(const QString &path, QString *error) const
{
  // §4 井位图层数据源：只写有 surface 坐标且坐标有限的井；坐标是原始
  // surface_x/y（局部测网米），真投影参数出现前地图一直读它。
  QJsonArray feats;
  for (const CatalogEntity &e : m_entities)
  {
    if (e.entityType != QLatin1String("well") || !e.hasSurface)
      continue;
    if (!std::isfinite(e.surfaceX) || !std::isfinite(e.surfaceY))
      continue;
    QJsonObject props;
    props.insert(QStringLiteral("id"), e.id);
    props.insert(QStringLiteral("name"), e.name);
    props.insert(QStringLiteral("coordinate_status"), e.coordinateStatus);
    QJsonObject geom;
    geom.insert(QStringLiteral("type"), QStringLiteral("Point"));
    geom.insert(QStringLiteral("coordinates"), QJsonArray{e.surfaceX, e.surfaceY});
    QJsonObject f;
    f.insert(QStringLiteral("type"), QStringLiteral("Feature"));
    f.insert(QStringLiteral("properties"), props);
    f.insert(QStringLiteral("geometry"), geom);
    feats.append(f);
  }
  if (feats.isEmpty())
    return true; // 没有可定位的井——不写空文件，也不算失败

  QJsonObject root;
  root.insert(QStringLiteral("type"), QStringLiteral("FeatureCollection"));
  QJsonObject crsProps;
  crsProps.insert(QStringLiteral("name"), localGridCrsWkt());
  QJsonObject crs;
  crs.insert(QStringLiteral("type"), QStringLiteral("name"));
  crs.insert(QStringLiteral("properties"), crsProps);
  root.insert(QStringLiteral("crs"), crs);
  root.insert(QStringLiteral("features"), feats);

  QDir().mkpath(QFileInfo(path).absolutePath());
  // 原子写审计（T6）：井点 GeoJSON 是 wells 图层的数据源——裸 QFile 截断
  // 写中途崩溃会留下半截文件，OGR 照常打开但要素缺失（静默坏图层）。
  // QSaveFile 全量写成功才替换，坏盘/短写保住上一份好文件。
  QSaveFile file(path);
  file.setDirectWriteFallback(false);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
  {
    setError(error, tr("井点 GeoJSON 写入失败：%1（%2）").arg(path, file.errorString()));
    return false;
  }
  if (file.write(QJsonDocument(root).toJson(QJsonDocument::Compact)) < 0 ||
      !file.commit())
  {
    setError(error, tr("井点 GeoJSON 写入失败：%1").arg(path));
    return false;
  }
  return true;
}

// ---- D5 计数缓存 / 索引健康 ----
QHash<QString, int> DataCatalog::entityCountsByType() const
{
  return m_idx.entityCountsByType();
}

bool DataCatalog::indexHealthy(QString *mismatch) const
{
  return m_idx.verifyAgainst(m_entities, m_assets, m_versions, m_links, mismatch);
}
