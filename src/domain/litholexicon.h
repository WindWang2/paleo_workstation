// 层：数据
#pragma once

#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantMap>

class DataCatalog;

namespace paleo::domain
{

enum class LithoGroup
{
  Unknown = 0,
  Sand,         // 砂族（砂岩/粉砂岩等）
  Mud,          // 泥族（泥岩/页岩等）
  Carbonate,    // 碳酸盐族（灰岩/白云岩等）
  Conglomerate, // 砾岩族
  Other         // 其他
};

enum class MatchType
{
  Contains,
  Exact
};

struct LithoRule
{
  QString pattern;
  MatchType matchType = MatchType::Contains;
  LithoGroup group = LithoGroup::Unknown;
  int priority = 0; // 越大越优先

  QVariantMap toMap() const;
  static LithoRule fromMap(const QVariantMap &m);
};

struct LithoClassification
{
  LithoGroup group = LithoGroup::Unknown;
  bool isSand = false;
  bool isKnown = false;
  QString matchedPattern;
  int rulePriority = 0;
  MatchType matchType = MatchType::Contains;
  QString groupName;
};

class LithoLexicon
{
public:
  LithoLexicon();

  static QString groupDisplayName(LithoGroup group);
  static LithoGroup parseGroup(const QString &str);
  static QString matchTypeName(MatchType matchType);
  static MatchType parseMatchType(const QString &str);

  // 默认权威词表（等价于原 master 硬编码逻辑 + 歧义词优先级定案）
  static LithoLexicon defaultLexicon();

  // 从工程读取扩展词表（优先工程目录 litho_lexicon.json、project_area.json litho_lexicon 节点，无则回退 defaultLexicon）。
  // 坏 JSON（语法错）如实报错拒用：*error 置原因、返回 defaultLexicon()，
  // 调用方决定怎么呈现——不静默回落（同 project_area.json 纪律，arearules.cpp）。
  static LithoLexicon fromProject(const QString &projectDir, QString *error = nullptr,
                                  const DataCatalog *catalog = nullptr);

  // 归类判词
  LithoClassification classify(const QString &rawLitho) const;
  bool isSand(const QString &rawLitho) const;
  bool isKnown(const QString &rawLitho) const;

  // 规则管理
  const QList<LithoRule> &rules() const { return m_rules; }
  void addRule(const LithoRule &rule);
  void prependRule(const LithoRule &rule);
  void clearRules();
  void extend(const LithoLexicon &other);

  // 序列化
  QVariantMap toMap() const;
  static LithoLexicon fromMap(const QVariantMap &map, QString *error = nullptr);
  QByteArray toJson(bool indented = true) const;
  // 解析失败：*error 置原因并返回 defaultLexicon()——返回空表会让「坏词表」
  // 与「无词表」不可分辨；是否拒用由调用方按 error 决定。
  static LithoLexicon fromJson(const QByteArray &json, QString *error = nullptr);

  QString name() const { return m_name; }
  void setName(const QString &name) { m_name = name; }

private:
  QString m_name = QStringLiteral("default");
  QList<LithoRule> m_rules;
};

} // namespace paleo::domain
