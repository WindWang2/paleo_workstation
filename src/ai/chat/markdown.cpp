// 层：功能
#include "markdown.h"

#include <QHash>
#include <QRegularExpression>
#include <QStringList>
#include <QUrl>

#include <algorithm>

namespace AiMarkdown {
namespace {

// Index runs once so an unmatched code delimiter never rescans the line.
QHash<qsizetype, qsizetype> codeSpans(const QString &text) {
  QHash<qsizetype, qsizetype> next;
  QHash<qsizetype, qsizetype> previousByLength;
  static const QRegularExpression backticks(QStringLiteral("`+"));
  auto runs = backticks.globalMatch(text);
  while (runs.hasNext()) {
    const auto run = runs.next();
    const auto previous = previousByLength.constFind(run.capturedLength());
    if (previous != previousByLength.cend())
      next.insert(previous.value(), run.capturedStart());
    previousByLength.insert(run.capturedLength(), run.capturedStart());
  }
  return next;
}

struct OpenMark {
  QString delimiter;
  qsizetype part;
};

bool wordCharacter(QChar ch) {
  return ch.isLetterOrNumber() || ch == QLatin1Char('_');
}

// Opening delimiters stay literal until a closing partner turns them into a
// matched pair. At most one pending delimiter per supported type is kept.
void appendMark(QStringList &parts, QList<OpenMark> &open, const QString &mark,
                QChar before, QChar after) {
  const bool underscore = mark.front() == QLatin1Char('_');
  const bool close = !before.isNull() && !before.isSpace() && !(underscore && wordCharacter(after));
  const bool start = !after.isNull() && !after.isSpace() && !(underscore && wordCharacter(before));
  auto found = std::find_if(open.begin(), open.end(), [&](const OpenMark &entry) {
    return entry.delimiter == mark;
  });
  if (found != open.end() && close) {
    const QString tag = mark == QStringLiteral("~~") ? QStringLiteral("s")
                      : mark.size() == 2 ? QStringLiteral("b") : QStringLiteral("i");
    parts[found->part] = QStringLiteral("<%1>").arg(tag);
    parts << QStringLiteral("</%1>").arg(tag);
    open.erase(found, open.end());
    return;
  }
  if (start) {
    if (found != open.end())
      open.erase(found);
    open.append({mark, parts.size()});
  }
  parts << mark;
}

QString inlineMarkdown(const QString &text) {
  static const QRegularExpression tokens(QStringLiteral(
    R"md(\\([\\`*~\[\]|_]))md"                            // 1: escaped punctuation
    R"md(|(`+))md"                                       // 2: code delimiter
    R"md(|(!?)\[([^\[\]\n]*)\]\(([^\s()]*(?:\([^\s()]*\)[^\s()]*)?)\))md" // 3–5: link
    R"md(|(\*\*|\*|~~|__|_))md"));                      // 6: emphasis
  const auto nextCode = codeSpans(text);
  QStringList parts;
  QList<OpenMark> open;
  qsizetype pos = 0;
  auto matches = tokens.globalMatch(text);
  while (matches.hasNext()) {
    const auto match = matches.next();
    const auto start = match.capturedStart();
    if (start < pos) { // An opaque code span already consumed this token.
      if (match.capturedEnd() > pos)
        matches = tokens.globalMatch(text, pos);
      continue;
    }
    parts << text.mid(pos, start - pos).toHtmlEscaped();
    pos = match.capturedEnd();
    if (match.hasCaptured(1)) {
      parts << match.captured(1).toHtmlEscaped();
    } else if (match.hasCaptured(2)) {
      const auto closing = nextCode.constFind(start);
      if (closing == nextCode.cend()) {
        parts << match.captured(2);
      } else {
        const QString code = text.mid(pos, closing.value() - pos).toHtmlEscaped();
        parts << QStringLiteral("<code>%1</code>").arg(code);
        pos = closing.value() + match.capturedLength(2);
      }
    } else if (match.hasCaptured(4)) {
      const QUrl url(match.captured(5), QUrl::StrictMode);
      if (match.captured(3).isEmpty() && url.isValid() && !url.host().isEmpty()
          && (url.scheme() == QStringLiteral("http") || url.scheme() == QStringLiteral("https"))) {
        // The label cannot itself contain brackets, so recursion ends here.
        parts << QStringLiteral("<a href=\"%1\">%2</a>")
                   .arg(url.toString(QUrl::FullyEncoded).toHtmlEscaped(),
                        inlineMarkdown(match.captured(4)));
      } else {
        parts << match.captured().toHtmlEscaped();
      }
    } else {
      appendMark(parts, open, match.captured(6), start ? text[start - 1] : QChar(),
                 pos < text.size() ? text[pos] : QChar());
    }
  }
  parts << text.mid(pos).toHtmlEscaped();
  return parts.join(QString());
}

QStringList tableCells(const QString &line) {
  QString text = line.trimmed();
  if (text.startsWith(QLatin1Char('|')))
    text.remove(0, 1);
  QStringList cells;
  QString cell;
  bool escaped = false;
  for (const QChar c : text) {
    if (c == QLatin1Char('|') && !escaped) {
      cells.append(cell.trimmed());
      cell.clear();
    } else {
      cell += c;
    }
    escaped = !escaped && c == QLatin1Char('\\');
  }
  if (!cell.isEmpty() || !text.endsWith(QLatin1Char('|')))
    cells.append(cell.trimmed());
  return cells;
}

QString tableRow(const QStringList &cells, const QStringList &alignments, const QString &tag) {
  QString html = QStringLiteral("<tr>");
  for (qsizetype i = 0; i < qMax(cells.size(), alignments.size()); ++i) {
    html += QStringLiteral("<%1%2>%3</%1>")
              .arg(tag, alignments.value(i), inlineMarkdown(cells.value(i)));
  }
  return html + QStringLiteral("</tr>");
}

QString takeTable(const QStringList &lines, qsizetype &i) {
  if (i + 1 >= lines.size() || !lines[i].contains(QLatin1Char('|')))
    return {};
  static const QRegularExpression separator(QStringLiteral("^:?-+:?$"));
  const QStringList header = tableCells(lines[i]);
  const QStringList separators = tableCells(lines[i + 1]);
  if (header.isEmpty() || separators.size() != header.size()
      || !lines[i + 1].contains(QLatin1Char('|')))
    return {};
  QStringList alignments;
  for (const QString &cell : separators) {
    if (!separator.match(cell).hasMatch())
      return {};
    const bool left = cell.startsWith(QLatin1Char(':'));
    const bool right = cell.endsWith(QLatin1Char(':'));
    alignments << (left && right ? QStringLiteral(" align=\"center\"")
                   : right ? QStringLiteral(" align=\"right\"")
                   : left ? QStringLiteral(" align=\"left\"") : QString());
  }
  QString html = QStringLiteral("<table border=\"1\" cellspacing=\"0\" cellpadding=\"4\">");
  html += tableRow(header, alignments, QStringLiteral("th"));
  i += 2;
  while (i < lines.size() && lines[i].contains(QLatin1Char('|'))) {
    const QStringList cells = tableCells(lines[i]);
    if (!lines[i].trimmed().startsWith(QLatin1Char('|')) && cells.size() < 2)
      break;
    html += tableRow(cells, alignments, QStringLiteral("td"));
    ++i;
  }
  return html + QStringLiteral("</table>");
}

QRegularExpressionMatch listMatch(const QString &line) {
  static const QRegularExpression marker(
    QStringLiteral(R"(^([ \t]*)(?:[-*+]|([0-9]{1,9})[.)])[ \t]+(.+)$)"));
  return marker.match(line);
}

QString takeList(const QStringList &lines, qsizetype &i, int depth) {
  const auto first = listMatch(lines[i]);
  const qsizetype indent = first.capturedLength(1);
  const bool ordered = first.hasCaptured(2);
  const QString tag = ordered ? QStringLiteral("ol") : QStringLiteral("ul");
  const QString start = ordered && first.captured(2).toInt() != 1
    ? QStringLiteral(" start=\"%1\"").arg(first.captured(2).toInt()) : QString();
  QString html = QStringLiteral("<%1%2>").arg(tag, start);
  static const QRegularExpression task(QStringLiteral(R"(^\[([xX ])\]\s+(.*)$)"));
  while (i < lines.size()) {
    const auto marker = listMatch(lines[i]);
    if (!marker.hasMatch() || marker.capturedLength(1) < indent)
      break;
    if (marker.hasCaptured(2) != ordered)
      break;
    QString body = marker.captured(3);
    const auto check = task.match(body);
    QString attr;
    if (check.hasMatch()) {
      attr = check.captured(1) == QStringLiteral(" ") ? QStringLiteral(" class=\"unchecked\"")
                                                      : QStringLiteral(" class=\"checked\"");
      body = check.captured(2);
    }
    html += QStringLiteral("<li%1>%2").arg(attr, inlineMarkdown(body));
    ++i;
    while (i < lines.size() && !lines[i].trimmed().isEmpty()) {
      const auto next = listMatch(lines[i]);
      if (next.hasMatch()) {
        if (depth >= 16 || next.capturedLength(1) <= indent)
          break;
        html += takeList(lines, i, depth + 1);
        continue;
      }
      const QString bodyLine = lines[i].trimmed();
      const qsizetype leadingSpace = lines[i].indexOf(bodyLine.front());
      if (leadingSpace < marker.capturedStart(3))
        break;
      html += QStringLiteral("<br>") + inlineMarkdown(bodyLine);
      ++i;
    }
    html += QStringLiteral("</li>");
  }
  return html + QStringLiteral("</%1>").arg(tag);
}

QString blocks(const QStringList &lines, int quoteDepth = 0) {
  static const QRegularExpression heading(QStringLiteral(R"(^(#{1,6})\s+(.+?)\s*#*\s*$)"));
  static const QRegularExpression fence(QStringLiteral(R"(^(\x60{3,}|~{3,})(.*)$)"));
  static const QRegularExpression rule(QStringLiteral(R"(^([-*_])\s*\1\s*\1(?:\s*\1)*$)"));
  QString html;
  QStringList paragraph;
  const auto flush = [&] {
    if (!paragraph.isEmpty()) {
      html += QStringLiteral("<p>%1</p>").arg(paragraph.join(QStringLiteral("<br>")));
      paragraph.clear();
    }
  };
  qsizetype i = 0;
  while (i < lines.size()) {
    const QString line = lines[i].trimmed();
    const auto code = fence.match(line);
    if (code.hasMatch()) {
      flush();
      const QString delimiter = code.captured(1);
      QStringList content;
      for (++i; i < lines.size(); ++i) {
        const QString closing = lines[i].trimmed();
        if (closing.startsWith(delimiter) && closing.count(delimiter.front()) == closing.size()) {
          ++i;
          break;
        }
        content << lines[i].toHtmlEscaped();
      }
      html += QStringLiteral("<pre>%1</pre>").arg(content.join(QStringLiteral("<br>")));
      continue;
    }
    if (line.isEmpty()) {
      flush();
    } else if (const auto title = heading.match(line); title.hasMatch()) {
      flush();
      html += QStringLiteral("<h%1>%2</h%1>")
                .arg(QString::number(title.capturedLength(1)), inlineMarkdown(title.captured(2)));
    } else if (rule.match(line).hasMatch()) {
      flush();
      html += QStringLiteral("<hr>");
    } else if (line.startsWith(QLatin1Char('>')) && quoteDepth < 16) {
      flush();
      QStringList quoted;
      while (i < lines.size() && lines[i].trimmed().startsWith(QLatin1Char('>'))) {
        QString body = lines[i++].trimmed().mid(1);
        if (body.startsWith(QLatin1Char(' ')))
          body.remove(0, 1);
        quoted << body;
      }
      html += QStringLiteral("<blockquote>%1</blockquote>").arg(blocks(quoted, quoteDepth + 1));
      continue;
    } else if (listMatch(lines[i]).hasMatch()) {
      flush();
      html += takeList(lines, i, 0);
      continue;
    } else if (const QString table = takeTable(lines, i); !table.isEmpty()) {
      flush();
      html += table;
      continue;
    } else {
      paragraph << inlineMarkdown(line);
    }
    ++i;
  }
  flush();
  return html;
}
} // namespace

QString toHtml(const QString &markdown) {
  QString normalized = markdown;
  normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
  return blocks(normalized.split(QLatin1Char('\n')));
}
} // namespace AiMarkdown
