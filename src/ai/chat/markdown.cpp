// 层：功能
#include "markdown.h"

#include <QChar>
#include <QRegularExpression>
#include <QStringList>

// ai/chat — Markdown 转换实现（方言子集见 markdown.h 头注释）。
//
// 管线纪律：escape-all-first——先把整段输入按 HTML 转义，再在转义后的
// 文本上识别标记。转义不改动 `#`/`|`/`*`/`` ` ``/`-` 这些 ASCII 标记
// 字符，识别不受影响；而任何 `<script>`/`<img onerror>` 字面量从进入
// 本函数起就已经是 `&lt;script&gt;` 文本，不存在裸 HTML 通路。

namespace AiMarkdown {
namespace {

// 行内标记：在已转义文本上替换。code 先于粗体再先于斜体（避免
// `**bold**` 被 `*...*` 抢先吃掉一半）。
QString inlineMarkdown(QString text) {
  static const QRegularExpression code(
    QStringLiteral("`([^`\\n]+?)`"));
  text.replace(code, QStringLiteral("<code>\\1</code>"));
  static const QRegularExpression bold(
    QStringLiteral("\\*\\*([^*\\n]+?)\\*\\*"));
  text.replace(bold, QStringLiteral("<b>\\1</b>"));
  static const QRegularExpression italic(QStringLiteral("\\*([^*\\n]+?)\\*"));
  text.replace(italic, QStringLiteral("<i>\\1</i>"));
  return text;
}

// 表格行拆格：剥外缘竖线后按 `|` 均分（转义文本里不会出现被引入的竖线）。
QStringList tableCells(const QString &line) {
  QString text = line.trimmed();
  if (text.startsWith(QLatin1Char('|')))
    text.remove(0, 1);
  if (text.endsWith(QLatin1Char('|')))
    text.chop(1);
  QStringList cells;
  for (const QString &cell : text.split(QLatin1Char('|')))
    cells.append(inlineMarkdown(cell.trimmed()));
  return cells;
}

// 分隔行：整行只允许 `|` `:` `-` 与空白，且至少一个 `-`。
bool isTableSeparator(const QString &line) {
  const QString text = line.trimmed();
  if (!text.contains(QLatin1Char('-')))
    return false;
  for (const QChar &c : text) {
    if (c != QLatin1Char('|') && c != QLatin1Char('-') &&
        c != QLatin1Char(':') && c != QLatin1Char(' ') &&
        c != QLatin1Char('\t'))
      return false;
  }
  return true;
}

// ATX 标题：`#` ~ `######`，尾随 `#` 装饰忽略；`#` 后必须有内容。
// 级别数字 = 前缀 `#` 的个数（captured(1) 本身是井号串，不是数字）。
QString headingHtml(const QString &line) {
  static const QRegularExpression pattern(
    QStringLiteral("^(#{1,6})\\s+(.+?)\\s*#*\\s*$"));
  const auto match = pattern.match(line);
  if (!match.hasMatch())
    return QString();
  const QString level =
    QString::number(match.captured(1).length());
  return QStringLiteral("<h%1>%2</h%1>")
    .arg(level, inlineMarkdown(match.captured(2)));
}

// 列表行返回内容并标注有序/无序；非列表行返回空串。
QString listItemHtml(const QString &line, bool *ordered) {
  static const QRegularExpression bullet(
    QStringLiteral("^(?:-|\\*)\\s+(.+)$"));
  static const QRegularExpression numbered(
    QStringLiteral("^\\d{1,9}\\.\\s+(.+)$"));
  const auto b = bullet.match(line);
  if (b.hasMatch()) {
    *ordered = false;
    return inlineMarkdown(b.captured(1));
  }
  const auto n = numbered.match(line);
  if (n.hasMatch()) {
    *ordered = true;
    return inlineMarkdown(n.captured(1));
  }
  return QString();
}

// 表格块：首行表头 + 分隔行 + 数据行。不足两行或第二行不是分隔行 →
// 返回空串（调用方按普通段落原样显示，宁小勿大：识别不了不猜）。
QString tableHtml(const QStringList &rows) {
  if (rows.size() < 2 || !isTableSeparator(rows.at(1)))
    return QString();
  QStringList out;
  out << QStringLiteral(
           "<table border=\"1\" cellspacing=\"0\" cellpadding=\"3\">");
  out << QStringLiteral("<tr>");
  for (const QString &cell : tableCells(rows.at(0)))
    out << QStringLiteral("<th>%1</th>").arg(cell);
  out << QStringLiteral("</tr>");
  for (int i = 2; i < rows.size(); ++i) {
    out << QStringLiteral("<tr>");
    for (const QString &cell : tableCells(rows.at(i)))
      out << QStringLiteral("<td>%1</td>").arg(cell);
    out << QStringLiteral("</tr>");
  }
  out << QStringLiteral("</table>");
  return out.join(QString());
}

} // namespace

QString toHtml(const QString &markdown) {
  // 1) 整体转义：之后的一切操作都在「已转义」文本上进行。
  QStringList lines = markdown.toHtmlEscaped().split(QLatin1Char('\n'));
  for (QString &line : lines) {
    if (line.endsWith(QLatin1Char('\r')))
      line.chop(1);
  }

  QStringList out;        // 输出块
  QStringList paragraph;  // 段落累积（段内换行用 <br> 保持）
  bool inCode = false;
  QStringList codeLines;
  int listOrdered = -1; // -1 = 不在列表；0 = 无序；1 = 有序
  QStringList listItems;

  auto flushParagraph = [&out, &paragraph]() {
    if (paragraph.isEmpty())
      return;
    out << QStringLiteral("<p>%1</p>")
               .arg(paragraph.join(QStringLiteral("<br>")));
    paragraph.clear();
  };
  auto flushList = [&out, &listOrdered, &listItems]() {
    if (listOrdered < 0)
      return;
    const QString tag =
      listOrdered == 1 ? QStringLiteral("ol") : QStringLiteral("ul");
    QStringList rendered;
    for (const QString &item : listItems)
      rendered << QStringLiteral("<li>%1</li>").arg(item);
    out << QStringLiteral("<%1>%2</%1>").arg(tag, rendered.join(QString()));
    listOrdered = -1;
    listItems.clear();
  };

  for (int i = 0; i < lines.size(); ++i) {
    const QString line = lines.at(i).trimmed();
    if (line.startsWith(QStringLiteral("```"))) {
      // 围栏切换是独立块边界：先收口列表/段落。
      flushParagraph();
      flushList();
      if (!inCode) {
        inCode = true;
      } else {
        out << QStringLiteral("<pre>%1</pre>")
                   .arg(codeLines.join(QStringLiteral("<br>")));
        codeLines.clear();
        inCode = false;
      }
      continue;
    }
    if (inCode) {
      // 代码块内保留原始行（含缩进；<pre> 原样呈现，不做行内替换）。
      codeLines << lines.at(i);
      continue;
    }
    if (line.isEmpty()) {
      flushParagraph();
      flushList();
      continue;
    }
    const QString heading = headingHtml(line);
    if (!heading.isEmpty()) {
      flushParagraph();
      flushList();
      out << heading;
      continue;
    }
    bool ordered = false;
    const QString item = listItemHtml(line, &ordered);
    if (!item.isEmpty()) {
      flushParagraph();
      if (listOrdered != (ordered ? 1 : 0)) {
        flushList();
        listOrdered = ordered ? 1 : 0;
      }
      listItems << item;
      continue;
    }
    if (line.startsWith(QLatin1Char('|'))) {
      // 表格候选簇：连续 `|` 起头行一次吃掉，簇内自判定。
      QStringList rows;
      int j = i;
      while (j < lines.size()) {
        const QString candidate = lines.at(j).trimmed();
        if (candidate.isEmpty() || !candidate.startsWith(QLatin1Char('|')))
          break;
        rows << candidate;
        ++j;
      }
      const QString table = tableHtml(rows);
      if (!table.isEmpty()) {
        flushParagraph();
        flushList();
        out << table;
      } else {
        // 不是合法表格：按普通文本行原样进段落（不猜结构）。
        flushList();
        for (const QString &row : rows)
          paragraph << inlineMarkdown(row);
      }
      i = j - 1;
      continue;
    }
    // 普通文本行。
    flushList();
    paragraph << inlineMarkdown(line);
  }
  flushParagraph();
  flushList();
  if (inCode) { // 未闭合的围栏：按代码块收尾（诚实渲染已收内容）
    out << QStringLiteral("<pre>%1</pre>")
               .arg(codeLines.join(QStringLiteral("<br>")));
  }

  return out.join(QString());
}

} // namespace AiMarkdown
