// 层：数据
#include "arearules.h"

#include "projectclassifier.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QMutex>
#include <QMutexLocker>

#include <cmath>

namespace AreaRules
{
namespace
{
const char kConfigFileName[] = "project_area.json";
constexpr int kMaxTraceHeaderOffset = 236; // i32 字完整落在 240 字节道头内

QString errAt(const QString &where, const QString &what)
{
  return QStringLiteral("%1: %2").arg(where, what);
}

// 未知键 = 拒用：配置拼错键名（boundaries vs sequence_boundaries）若被静默
// 忽略，等于带病运行默认值——与「坏 JSON 不静默回退」同一纪律。
bool checkKeys(const QJsonObject &o, std::initializer_list<const char *> allowed,
               const QString &where, QString *error)
{
  for (auto it = o.begin(); it != o.end(); ++it)
  {
    bool known = false;
    for (const char *a : allowed)
    {
      if (it.key() == QLatin1String(a))
      {
        known = true;
        break;
      }
    }
    if (!known)
    {
      *error = errAt(where, QStringLiteral("unknown key '%1'").arg(it.key()));
      return false;
    }
  }
  return true;
}

bool toStringList(const QJsonArray &arr, const QString &where, QStringList *out, QString *error)
{
  for (const QJsonValue &v : arr)
  {
    if (!v.isString() || v.toString().isEmpty())
    {
      *error = errAt(where, QStringLiteral("expects non-empty strings"));
      return false;
    }
    out->append(v.toString());
  }
  return true;
}

// 缺键 = 保持传入默认值；类型错/非整数/越界 = 拒用。
bool intField(const QJsonObject &o, const char *key, int minV, int maxV, int *out,
              const QString &where, QString *error)
{
  const QJsonValue v = o.value(QLatin1String(key));
  if (v.isUndefined())
    return true;
  if (!v.isDouble() || std::floor(v.toDouble()) != v.toDouble())
  {
    *error = errAt(where, QStringLiteral("'%1' expects an integer").arg(QLatin1String(key)));
    return false;
  }
  const int val = static_cast<int>(v.toDouble());
  if (val < minV || val > maxV)
  {
    *error = errAt(where, QStringLiteral("'%1' %2 out of range [%3, %4]")
                                     .arg(QLatin1String(key))
                                     .arg(val)
                                     .arg(minV)
                                     .arg(maxV));
    return false;
  }
  *out = val;
  return true;
}

bool applyRuleObject(const QJsonObject &o, DatPathRule *rule, QString *error)
{
  if (!checkKeys(o, {"exact_segments", "segment_keywords", "filename_keywords", "type"},
                 QStringLiteral("classifier.dat_path_rules[]"), error))
    return false;
  const QJsonValue t = o.value(QLatin1String("type"));
  if (!t.isString() || t.toString().isEmpty())
  {
    *error = errAt(QStringLiteral("classifier.dat_path_rules[]"),
                   QStringLiteral("'type' is required"));
    return false;
  }
  rule->type = t.toString();
  if (!isClassifierType(rule->type))
  {
    *error = errAt(QStringLiteral("classifier.dat_path_rules[]"),
                   QStringLiteral("unknown classifier type '%1'").arg(rule->type));
    return false;
  }
  const struct
  {
    const char *key;
    QStringList *out;
  } lists[] = {{"exact_segments", &rule->exactSegments},
               {"segment_keywords", &rule->segmentKeywords},
               {"filename_keywords", &rule->filenameKeywords}};
  for (const auto &l : lists)
  {
    const QJsonValue v = o.value(QLatin1String(l.key));
    if (v.isUndefined())
      continue;
    if (!v.isArray())
    {
      *error = errAt(QStringLiteral("classifier.dat_path_rules[]"),
                     QStringLiteral("'%1' expects an array").arg(QLatin1String(l.key)));
      return false;
    }
    if (!toStringList(v.toArray(), QStringLiteral("classifier.dat_path_rules[]"), l.out, error))
      return false;
  }
  if (rule->exactSegments.isEmpty() && rule->segmentKeywords.isEmpty() &&
      rule->filenameKeywords.isEmpty())
  {
    *error = errAt(QStringLiteral("classifier.dat_path_rules[]"),
                   QStringLiteral("rule has no matcher (segments/keywords)"));
    return false;
  }
  return true;
}

bool applyClassifier(const QJsonObject &o, ClassifierRules *out, QString *error)
{
  if (!checkKeys(o, {"dat_path_rules", "reference_dir_names", "fixed_auxiliary_name_stem"},
                 QStringLiteral("classifier"), error))
    return false;
  const QJsonValue rules = o.value(QLatin1String("dat_path_rules"));
  if (!rules.isUndefined())
  {
    if (!rules.isArray())
    {
      *error = errAt(QStringLiteral("classifier"), QStringLiteral("'dat_path_rules' expects an array"));
      return false;
    }
    QVector<DatPathRule> table;
    for (const QJsonValue &v : rules.toArray())
    {
      if (!v.isObject())
      {
        *error = errAt(QStringLiteral("classifier.dat_path_rules"),
                       QStringLiteral("expects objects"));
        return false;
      }
      DatPathRule rule;
      if (!applyRuleObject(v.toObject(), &rule, error))
        return false;
      table.append(rule);
    }
    out->datPathRules = table; // 整表替换：优先级序是表语义，不与默认表合并
  }
  const QJsonValue refDirs = o.value(QLatin1String("reference_dir_names"));
  if (!refDirs.isUndefined())
  {
    if (!refDirs.isArray())
    {
      *error = errAt(QStringLiteral("classifier"), QStringLiteral("'reference_dir_names' expects an array"));
      return false;
    }
    QStringList names;
    if (!toStringList(refDirs.toArray(), QStringLiteral("classifier.reference_dir_names"),
                      &names, error))
      return false;
    out->referenceDirNames = names;
  }
  const QJsonValue stem = o.value(QLatin1String("fixed_auxiliary_name_stem"));
  if (!stem.isUndefined())
  {
    if (!stem.isString())
    {
      *error = errAt(QStringLiteral("classifier"), QStringLiteral("'fixed_auxiliary_name_stem' expects a string"));
      return false;
    }
    out->fixedAuxiliaryNameStem = stem.toString(); // 空串 = 显式关闭固定辅助
  }
  return true;
}

bool applyConfigObject(const QJsonObject &o, Rules *out, QString *error)
{
  if (!checkKeys(o, {"schema_version", "sequence_boundaries", "target_horizon", "classifier",
                     "segy_indexing", "onnx_grid",
                     // roles（DataCatalog::open）与 litho_lexicon（LithoLexicon::
                     // fromProject）各自从本文件读自己的节——白名单放行，
                     // 本解析器不消费；否则任一节在场会让整文件被拒、
                     // AreaRules 静默退回内置默认。
                     "roles", "litho_lexicon"},
                 QStringLiteral("project_area.json"), error))
    return false;
  const QJsonValue sv = o.value(QLatin1String("schema_version"));
  if (!sv.isUndefined() && (!sv.isDouble() || sv.toInt() != 1))
  {
    *error = errAt(QStringLiteral("project_area.json"),
                   QStringLiteral("unsupported schema_version %1 (supported: 1)").arg(sv.toVariant().toString()));
    return false;
  }
  const QJsonValue boundaries = o.value(QLatin1String("sequence_boundaries"));
  if (!boundaries.isUndefined())
  {
    if (!boundaries.isArray())
    {
      *error = errAt(QStringLiteral("project_area.json"),
                     QStringLiteral("'sequence_boundaries' expects an array"));
      return false;
    }
    QStringList names;
    if (!toStringList(boundaries.toArray(), QStringLiteral("sequence_boundaries"), &names, error))
      return false;
    for (QString &n : names)
      n = n.toUpper(); // isKnownSequenceBoundary 对 stem 取大写比较
    out->sequenceBoundaries = names;
  }
  const QJsonValue target = o.value(QLatin1String("target_horizon"));
  if (!target.isUndefined())
  {
    if (!target.isString() || target.toString().isEmpty())
    {
      *error = errAt(QStringLiteral("project_area.json"),
                     QStringLiteral("'target_horizon' expects a non-empty string"));
      return false;
    }
    out->targetHorizon = target.toString().toUpper();
  }
  // target_horizon 必须是有序集合的成员——标定一个集合外层位 = 配置错误，
  // 如实拒绝而不是带着一个永远不会命中的目标跑。
  if (!out->sequenceBoundaries.contains(out->targetHorizon))
  {
    *error = errAt(QStringLiteral("project_area.json"),
                   QStringLiteral("'target_horizon' %1 is not a member of "
                                  "sequence_boundaries")
                       .arg(out->targetHorizon));
    return false;
  }
  const QJsonValue classifier = o.value(QLatin1String("classifier"));
  if (!classifier.isUndefined())
  {
    if (!classifier.isObject() ||
        !applyClassifier(classifier.toObject(), &out->classifier, error))
    {
      if (error->isEmpty())
        *error = errAt(QStringLiteral("project_area.json"), QStringLiteral("'classifier' expects an object"));
      return false;
    }
  }
  const QJsonValue segy = o.value(QLatin1String("segy_indexing"));
  if (!segy.isUndefined())
  {
    if (!segy.isObject())
    {
      *error = errAt(QStringLiteral("project_area.json"), QStringLiteral("'segy_indexing' expects an object"));
      return false;
    }
    const QJsonObject so = segy.toObject();
    const QString where = QStringLiteral("segy_indexing");
    if (!checkKeys(so, {"inline_word_offset", "crossline_word_offset", "field_record_offset",
                        "cdp_xline_offset"},
                   where, error))
      return false;
    if (!intField(so, "inline_word_offset", 0, kMaxTraceHeaderOffset, &out->segy.inlineWordOffset, where, error) ||
        !intField(so, "crossline_word_offset", 0, kMaxTraceHeaderOffset, &out->segy.crosslineWordOffset, where, error) ||
        !intField(so, "field_record_offset", 0, kMaxTraceHeaderOffset, &out->segy.fieldRecordOffset, where, error) ||
        !intField(so, "cdp_xline_offset", 0, kMaxTraceHeaderOffset, &out->segy.cdpXlineOffset, where, error))
      return false;
  }
  const QJsonValue grid = o.value(QLatin1String("onnx_grid"));
  if (!grid.isUndefined())
  {
    if (!grid.isObject())
    {
      *error = errAt(QStringLiteral("project_area.json"), QStringLiteral("'onnx_grid' expects an object"));
      return false;
    }
    const QJsonObject go = grid.toObject();
    const QString where = QStringLiteral("onnx_grid");
    if (!checkKeys(go, {"rows", "cols"}, where, error))
      return false;
    if (!intField(go, "rows", 1, 1 << 20, &out->onnxGrid.rows, where, error) ||
        !intField(go, "cols", 1, 1 << 20, &out->onnxGrid.cols, where, error))
      return false;
  }
  return true;
}

// 进程级 active 状态。导入/索引在任务池线程上读（D1），工程切换在主线程写，
// 故全部经互斥锁取快照（Rules 是浅拷贝几个 string list，代价可忽略）。
QMutex g_mutex;
QString g_projectDir;
QString g_lastError;
Rules g_rules = defaults();
} // namespace

Rules defaults()
{
  Rules r;
  // plan §1：8 层序界面（体系域字段 LST/TST/HST 无对应数据，不在名单）。
  r.sequenceBoundaries = {QStringLiteral("C3"),  QStringLiteral("C6"),  QStringLiteral("D53"),
                          QStringLiteral("D61"), QStringLiteral("D62"), QStringLiteral("D63"),
                          QStringLiteral("D71"), QStringLiteral("D72")};
  r.targetHorizon = QStringLiteral("D61"); // 本工区编图/验证的标定层位

  // projectclassifier.cpp 原 .dat 段表（优先级序，先中先得）。
  DatPathRule timeDepth;
  timeDepth.exactSegments = {QStringLiteral("td")};
  timeDepth.segmentKeywords = {QString::fromUtf8("时深")};
  timeDepth.type = QStringLiteral("time_depth");
  DatPathRule horizon;
  horizon.segmentKeywords = {QString::fromUtf8("层位")};
  horizon.type = QStringLiteral("horizon");
  DatPathRule strat;
  strat.segmentKeywords = {QString::fromUtf8("井分层")};
  strat.type = QStringLiteral("well_stratification");
  DatPathRule wellHead;
  wellHead.segmentKeywords = {QString::fromUtf8("井位")};
  wellHead.filenameKeywords = {QStringLiteral("wellhead"), QStringLiteral("well_head")};
  wellHead.type = QStringLiteral("well_head");
  // goal/well-trajectory：井斜站表 .dat（目录段 dev/测斜/井斜，或文件名
  // deviation/trajectory/井斜）。表序尾部追加——与既有四规则的判据无交集，
  // 既有分类优先级零扰动。
  DatPathRule deviation;
  deviation.exactSegments = {QStringLiteral("dev")};
  deviation.segmentKeywords = {QString::fromUtf8("测斜"), QString::fromUtf8("井斜")};
  deviation.filenameKeywords = {QStringLiteral("deviation"), QStringLiteral("trajectory"),
                                QString::fromUtf8("井斜")};
  deviation.type = QStringLiteral("well_deviation");
  r.classifier.datPathRules = {timeDepth, horizon, strat, wellHead, deviation};

  r.classifier.referenceDirNames = {QString::fromUtf8("参考资料")};
  r.classifier.fixedAuxiliaryNameStem = QStringLiteral("HZ28-6-1");

  // segyreader.cpp 原道号索引约定（本工区 200P_seismic.sgy：inline 字 188 恒 0，
  // field record = inlineMin 1315，CDP 在偏移 20）。
  r.segy = SegyIndexing{};

  r.onnxGrid = OnnxGrid{};
  return r;
}

QString configFilePath(const QString &projectDir)
{
  return QDir(projectDir).filePath(QString::fromLatin1(kConfigFileName));
}

bool loadFromProjectDir(const QString &projectDir, Rules *out, QString *error)
{
  QString sink;
  QString *err = error ? error : &sink;
  err->clear();
  *out = defaults();
  const QString path = configFilePath(projectDir);
  QFile f(path);
  if (!f.exists())
    return true; // 缺文件 = 默认（读取顺序契约第一档，不是错误）
  if (!f.open(QIODevice::ReadOnly))
  {
    *err = errAt(path, QStringLiteral("cannot open for reading"));
    return false;
  }
  QJsonParseError parseError{};
  const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &parseError);
  if (parseError.error != QJsonParseError::NoError)
  {
    *err = errAt(path, QStringLiteral("JSON parse error at offset %1: %2")
                            .arg(parseError.offset)
                            .arg(parseError.errorString()));
    return false;
  }
  if (!doc.isObject())
  {
    *err = errAt(path, QStringLiteral("expects a JSON object"));
    return false;
  }
  if (!applyConfigObject(doc.object(), out, err))
  {
    if (err->isEmpty())
      *err = errAt(path, QStringLiteral("invalid configuration"));
    return false;
  }
  return true;
}

void setProjectDir(const QString &projectDir)
{
  QMutexLocker lock(&g_mutex);
  g_lastError.clear();
  if (projectDir.isEmpty())
  {
    g_rules = defaults();
    g_projectDir.clear();
    return;
  }
  Rules loaded;
  QString err;
  if (!loadFromProjectDir(projectDir, &loaded, &err))
  {
    // 坏 JSON 拒用：active 保持默认值，错误如实外露（调用方决定怎么呈现）。
    g_rules = defaults();
    g_projectDir = projectDir;
    g_lastError = err;
    return;
  }
  g_rules = loaded;
  g_projectDir = projectDir;
}

void reset()
{
  setProjectDir(QString());
}

Rules active()
{
  QMutexLocker lock(&g_mutex);
  return g_rules;
}

QString activeProjectDir()
{
  QMutexLocker lock(&g_mutex);
  return g_projectDir;
}

QString lastError()
{
  QMutexLocker lock(&g_mutex);
  return g_lastError;
}
} // namespace AreaRules
