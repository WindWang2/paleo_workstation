// 层：数据
#include "litholexicon.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>

namespace paleo::domain
{

QVariantMap LithoRule::toMap() const
{
  QVariantMap m;
  m.insert(QStringLiteral("pattern"), pattern);
  m.insert(QStringLiteral("matchType"), LithoLexicon::matchTypeName(matchType));
  m.insert(QStringLiteral("group"), LithoLexicon::groupDisplayName(group));
  m.insert(QStringLiteral("priority"), priority);
  return m;
}

LithoRule LithoRule::fromMap(const QVariantMap &m)
{
  LithoRule r;
  r.pattern = m.value(QStringLiteral("pattern")).toString();
  r.matchType = LithoLexicon::parseMatchType(m.value(QStringLiteral("matchType")).toString());
  r.group = LithoLexicon::parseGroup(m.value(QStringLiteral("group")).toString());
  r.priority = m.value(QStringLiteral("priority"), 0).toInt();
  return r;
}

LithoLexicon::LithoLexicon() = default;

QString LithoLexicon::groupDisplayName(LithoGroup group)
{
  switch (group)
  {
  case LithoGroup::Sand:
    return QStringLiteral("砂族");
  case LithoGroup::Mud:
    return QStringLiteral("泥族");
  case LithoGroup::Carbonate:
    return QStringLiteral("碳酸盐族");
  case LithoGroup::Conglomerate:
    return QStringLiteral("砾岩族");
  case LithoGroup::Other:
    return QStringLiteral("其他");
  case LithoGroup::Unknown:
  default:
    return QStringLiteral("未知");
  }
}

LithoGroup LithoLexicon::parseGroup(const QString &str)
{
  const QString s = str.trimmed().toLower();
  if (s == QStringLiteral("sand") || s == QStringLiteral("砂族") || s == QStringLiteral("砂岩") || s == QStringLiteral("砂"))
    return LithoGroup::Sand;
  if (s == QStringLiteral("mud") || s == QStringLiteral("泥族") || s == QStringLiteral("泥岩") || s == QStringLiteral("页岩") || s == QStringLiteral("shale"))
    return LithoGroup::Mud;
  if (s == QStringLiteral("carbonate") || s == QStringLiteral("碳酸盐族") || s == QStringLiteral("灰岩") || s == QStringLiteral("白云岩") || s == QStringLiteral("limestone") || s == QStringLiteral("dolomite"))
    return LithoGroup::Carbonate;
  if (s == QStringLiteral("conglomerate") || s == QStringLiteral("砾岩族") || s == QStringLiteral("砾岩"))
    return LithoGroup::Conglomerate;
  if (s == QStringLiteral("other") || s == QStringLiteral("其他"))
    return LithoGroup::Other;
  return LithoGroup::Unknown;
}

QString LithoLexicon::matchTypeName(MatchType matchType)
{
  return matchType == MatchType::Exact ? QStringLiteral("exact") : QStringLiteral("contains");
}

MatchType LithoLexicon::parseMatchType(const QString &str)
{
  return str.trimmed().toLower() == QStringLiteral("exact") ? MatchType::Exact : MatchType::Contains;
}

LithoLexicon LithoLexicon::defaultLexicon()
{
  // 只构造一次（函数级 static + COW 拷贝）：fromProject 每次都以默认词表
  // 打底（井因素表等热路径此前每口井重建一遍 30 条 addRule）。规则表是
  // 不可变 QVector——跨线程拷贝共享安全，改动自有副本即 detach。
  static const LithoLexicon built = [] {
    LithoLexicon lex;
    lex.setName(QStringLiteral("default"));

  // 1. 歧义复合词（优先级 100）：主要岩性由词尾定名决定
  lex.addRule({QStringLiteral("粉砂质泥岩"), MatchType::Contains, LithoGroup::Mud, 100});
  lex.addRule({QStringLiteral("砂质泥岩"), MatchType::Contains, LithoGroup::Mud, 100});
  lex.addRule({QStringLiteral("灰质泥岩"), MatchType::Contains, LithoGroup::Mud, 100});
  lex.addRule({QStringLiteral("泥质粉砂岩"), MatchType::Contains, LithoGroup::Sand, 100});
  lex.addRule({QStringLiteral("泥质砂岩"), MatchType::Contains, LithoGroup::Sand, 100});
  lex.addRule({QStringLiteral("灰质砂岩"), MatchType::Contains, LithoGroup::Sand, 100});
  lex.addRule({QStringLiteral("泥质灰岩"), MatchType::Contains, LithoGroup::Carbonate, 100});
  lex.addRule({QStringLiteral("砂质灰岩"), MatchType::Contains, LithoGroup::Carbonate, 100});
  lex.addRule({QStringLiteral("silty mudstone"), MatchType::Contains, LithoGroup::Mud, 100});
  lex.addRule({QStringLiteral("argillaceous sandstone"), MatchType::Contains, LithoGroup::Sand, 100});

  // 2. 砂岩族基础判词（优先级 50）
  lex.addRule({QStringLiteral("砂岩"), MatchType::Contains, LithoGroup::Sand, 50});
  lex.addRule({QStringLiteral("sandstone"), MatchType::Contains, LithoGroup::Sand, 50});
  lex.addRule({QStringLiteral("砂"), MatchType::Exact, LithoGroup::Sand, 50});
  lex.addRule({QStringLiteral("sand"), MatchType::Exact, LithoGroup::Sand, 50});

  // 3. 其他已知岩性基础判词（优先级 40）
  lex.addRule({QStringLiteral("泥"), MatchType::Contains, LithoGroup::Mud, 40});
  lex.addRule({QStringLiteral("页岩"), MatchType::Contains, LithoGroup::Mud, 40});
  lex.addRule({QStringLiteral("shale"), MatchType::Contains, LithoGroup::Mud, 40});
  lex.addRule({QStringLiteral("mudstone"), MatchType::Contains, LithoGroup::Mud, 40});
  lex.addRule({QStringLiteral("灰岩"), MatchType::Contains, LithoGroup::Carbonate, 40});
  lex.addRule({QStringLiteral("白云岩"), MatchType::Contains, LithoGroup::Carbonate, 40});
  lex.addRule({QStringLiteral("limestone"), MatchType::Contains, LithoGroup::Carbonate, 40});
  lex.addRule({QStringLiteral("dolomite"), MatchType::Contains, LithoGroup::Carbonate, 40});
  lex.addRule({QStringLiteral("砾岩"), MatchType::Contains, LithoGroup::Conglomerate, 40});
  lex.addRule({QStringLiteral("conglomerate"), MatchType::Contains, LithoGroup::Conglomerate, 40});

    return lex;
  }();
  return built;
}

LithoClassification LithoLexicon::classify(const QString &rawLitho) const
{
  LithoClassification res;
  const QString s = rawLitho.trimmed().toLower();
  if (s.isEmpty())
    return res;

  // 按优先级从高到低匹配
  for (const LithoRule &r : m_rules)
  {
    const QString pat = r.pattern.toLower();
    bool match = false;
    if (r.matchType == MatchType::Exact)
      match = (s == pat);
    else
      match = s.contains(pat);

    if (match)
    {
      res.group = r.group;
      res.isSand = (r.group == LithoGroup::Sand);
      res.isKnown = (r.group != LithoGroup::Unknown);
      res.matchedPattern = r.pattern;
      res.rulePriority = r.priority;
      res.matchType = r.matchType;
      res.groupName = groupDisplayName(r.group);
      return res;
    }
  }

  return res;
}

bool LithoLexicon::isSand(const QString &rawLitho) const
{
  return classify(rawLitho).isSand;
}

bool LithoLexicon::isKnown(const QString &rawLitho) const
{
  return classify(rawLitho).isKnown;
}

void LithoLexicon::addRule(const LithoRule &rule)
{
  m_rules.append(rule);
  std::stable_sort(m_rules.begin(), m_rules.end(), [](const LithoRule &a, const LithoRule &b) {
    return a.priority > b.priority;
  });
}

void LithoLexicon::prependRule(const LithoRule &rule)
{
  m_rules.prepend(rule);
  std::stable_sort(m_rules.begin(), m_rules.end(), [](const LithoRule &a, const LithoRule &b) {
    return a.priority > b.priority;
  });
}

void LithoLexicon::clearRules()
{
  m_rules.clear();
}

void LithoLexicon::extend(const LithoLexicon &other)
{
  for (const LithoRule &r : other.rules())
    m_rules.append(r);
  std::stable_sort(m_rules.begin(), m_rules.end(), [](const LithoRule &a, const LithoRule &b) {
    return a.priority > b.priority;
  });
}

QVariantMap LithoLexicon::toMap() const
{
  QVariantMap m;
  m.insert(QStringLiteral("name"), m_name);
  QVariantList list;
  for (const LithoRule &r : m_rules)
    list.append(r.toMap());
  m.insert(QStringLiteral("rules"), list);
  return m;
}

LithoLexicon LithoLexicon::fromMap(const QVariantMap &map, QString *error)
{
  LithoLexicon lex;
  lex.setName(map.value(QStringLiteral("name"), QStringLiteral("custom")).toString());
  const QVariantList list = map.value(QStringLiteral("rules")).toList();
  for (const QVariant &item : list)
  {
    const QVariantMap rm = item.toMap();
    if (!rm.isEmpty())
      lex.addRule(LithoRule::fromMap(rm));
  }
  return lex;
}

QByteArray LithoLexicon::toJson(bool indented) const
{
  const QJsonObject obj = QJsonObject::fromVariantMap(toMap());
  const QJsonDocument doc(obj);
  return doc.toJson(indented ? QJsonDocument::Indented : QJsonDocument::Compact);
}

LithoLexicon LithoLexicon::fromJson(const QByteArray &json, QString *error)
{
  QJsonParseError parseErr;
  const QJsonDocument doc = QJsonDocument::fromJson(json, &parseErr);
  if (doc.isNull())
  {
    if (error)
      *error = parseErr.errorString();
    return defaultLexicon();
  }
  return fromMap(doc.object().toVariantMap(), error);
}

LithoLexicon LithoLexicon::fromProject(const QString &projectDir, QString *error,
                                        const DataCatalog * /*catalog*/)
{
  if (error)
    error->clear();
  LithoLexicon lex = defaultLexicon();
  if (projectDir.isEmpty())
    return lex;

  const QString jsonPath = QDir(projectDir).filePath(QStringLiteral("litho_lexicon.json"));
  if (QFile::exists(jsonPath))
  {
    QFile f(jsonPath);
    if (f.open(QIODevice::ReadOnly))
    {
      QString err;
      const LithoLexicon custom = fromJson(f.readAll(), &err);
      if (err.isEmpty())
      {
        // 存在工程级自定义词表：把自定义词表的高优规则与默认词表合并
        LithoLexicon merged = defaultLexicon();
        merged.setName(custom.name().isEmpty() ? QStringLiteral("project_custom") : custom.name());
        for (const LithoRule &r : custom.rules())
          merged.addRule(r);
        return merged;
      }
      // 坏 JSON 拒用并如实外露（同 project_area.json 纪律，arearules.cpp
      // loadFromProjectDir）：此前这里把 err 丢掉、静默下探 project_area.json
      // 后回落默认词表，用户在词表损坏时零提示。现在返回默认词表 + *error，
      // 由调用方决定怎么呈现——不再继续下探别的来源（已有一个来源明示损坏）。
      if (error)
        *error = err;
      return lex;
    }
    // 词表在但读不开（权限/占用）：同样不静默当作没有自定义词表。
    if (error)
      *error = f.errorString();
    return lex;
  }

  // 检查 project_area.json 是否内嵌 litho_lexicon
  const QString areaJsonPath = QDir(projectDir).filePath(QStringLiteral("project_area.json"));
  if (QFile::exists(areaJsonPath))
  {
    QFile f(areaJsonPath);
    if (f.open(QIODevice::ReadOnly))
    {
      QJsonParseError parseErr;
      const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &parseErr);
      if (doc.isNull())
      {
        // 坏 JSON 不外露就等于把 project_area.json 里坏掉的内嵌词表当成没有
        // 内嵌词表——用户改坏了却零提示（同 275-282 行 litho_lexicon.json 路径
        // 的「不静默回落」纪律）。
        if (error)
          *error = parseErr.errorString();
        return lex;
      }
      if (doc.isObject())
      {
        const QJsonObject obj = doc.object();
        if (obj.contains(QStringLiteral("litho_lexicon")))
        {
          const QJsonValue lexValue = obj.value(QStringLiteral("litho_lexicon"));
          if (!lexValue.isObject())
          {
            // 节点非对象（数组/标量）：同样不当成空表静默吞掉。
            if (error)
              *error = QStringLiteral("project_area.json 的 litho_lexicon 不是对象");
            return lex;
          }
          LithoLexicon custom = fromMap(lexValue.toObject().toVariantMap());
          LithoLexicon merged = defaultLexicon();
          merged.setName(QStringLiteral("area_custom"));
          for (const LithoRule &r : custom.rules())
            merged.addRule(r);
          return merged;
        }
      }
    }
    else
    {
      // 文件在但读不开（权限/占用）：不外露就会静默回落默认词表。
      if (error)
        *error = f.errorString();
      return lex;
    }
  }

  return lex;
}

} // namespace paleo::domain
