// 层：数据
// 工作台定位器与 Agent 目录共用的操作名称和关键词匹配。
#pragma once

#include <QStringList>

namespace paleo {
class OperationSearch {
public:
  explicit OperationSearch(const QString &text)
      : phrase(text.simplified().toCaseFolded()), words(phrase.split(u' ', Qt::SkipEmptyParts)) {}

  double score(const QStringList &names, const QStringList &details) const {
    if (words.isEmpty())
      return 0.5;
    double best = 0;
    for (const QString &name : names) {
      if (name.compare(phrase, Qt::CaseInsensitive) == 0)
        return 1;
      if (name.startsWith(phrase, Qt::CaseInsensitive))
        best = 0.9;
      else if (best < 0.8 && name.contains(phrase, Qt::CaseInsensitive))
        best = 0.8;
    }
    if (best > 0)
      return best;
    for (const QString &detail : details)
      if (detail.contains(phrase, Qt::CaseInsensitive))
        return 0.6;
    if (words.size() < 2)
      return 0;
    const QString fields = (names + details).join(u' ');
    for (const QString &word : words)
      if (!fields.contains(word, Qt::CaseInsensitive))
        return 0;
    return 0.55;
  }

  const QString phrase;
private:
  QStringList words;
};
} // namespace paleo
