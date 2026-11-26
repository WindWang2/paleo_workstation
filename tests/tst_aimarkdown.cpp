// 层：测试壳
#include <QtTest>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextList>
#include <QTextTable>

#include "../src/ai/chat/markdown.h"

// 方向62：AiMarkdown 转换器单测——方言子集各块级/行内形态 + 安全断言
// （一切输入按不可信文本：`<script>` 等字面量必须以转义态出现在输出里，
// 不存在裸 HTML 通路）。
class TestAiMarkdown : public QObject {
  Q_OBJECT
private slots:
  void paragraphAndLineBreaks();
  void headings();
  void inlineMarks();
  void lists();
  void tables();
  void codeFence();
  void quoteTaskAndNestedList();
  void alignedTableInDocument();
  void webLinksAndStrikethrough();
  void delimitersAndLiteralIdentifiers();
  void unknownSyntaxFallsBackToPlainText();
  void scriptLiteralIsEscaped();
  void imgOnerrorLiteralIsEscaped();
  void injectionInsideTableAndCode();
};

void TestAiMarkdown::paragraphAndLineBreaks() {
  const QString html = AiMarkdown::toHtml(
    QStringLiteral("第一行\n第二行\n\n第二段"));
  QVERIFY(html.contains(QStringLiteral("<p>第一行<br>第二行</p>")));
  QVERIFY(html.contains(QStringLiteral("<p>第二段</p>")));
}

void TestAiMarkdown::headings() {
  const QString html = AiMarkdown::toHtml(
    QStringLiteral("# 对比结论\n### 岩性分段 ##\n"));
  QVERIFY2(html.contains(QStringLiteral("<h1>对比结论</h1>")),
           qPrintable(html));
  QVERIFY2(html.contains(QStringLiteral("<h3>岩性分段</h3>")),
           qPrintable(html));
  // `#` 后无内容 → 不是标题，按普通文本。
  QVERIFY(AiMarkdown::toHtml(QStringLiteral("#"))
            .contains(QStringLiteral("#")));
}

void TestAiMarkdown::inlineMarks() {
  const QString html = AiMarkdown::toHtml(
    QStringLiteral("这段是**关键证据**，*次要*，命令 `paleo.run` 执行。"));
  QVERIFY(html.contains(QStringLiteral("<b>关键证据</b>")));
  QVERIFY(html.contains(QStringLiteral("<i>次要</i>")));
  QVERIFY(html.contains(QStringLiteral("<code>paleo.run</code>")));
}

void TestAiMarkdown::lists() {
  const QString html = AiMarkdown::toHtml(QStringLiteral(
    "- 砂岩\n- 泥岩\n\n1. 首选\n2. 次选"));
  QVERIFY(html.contains(QStringLiteral("<ul><li>砂岩</li><li>泥岩</li></ul>")));
  QVERIFY(html.contains(
    QStringLiteral("<ol><li>首选</li><li>次选</li></ol>")));
}

void TestAiMarkdown::tables() {
  const QString html = AiMarkdown::toHtml(QStringLiteral(
    "| 井号 | 顶深 | 岩性 |\n|---|---|---|\n| A1 | 1200 | 砂岩 |\n"
    "| B2 | 1350 | **灰岩** |"));
  QVERIFY(html.contains(QStringLiteral("<table")));
  QVERIFY(html.contains(QStringLiteral("<tr><th>井号</th>")));
  QVERIFY(html.contains(QStringLiteral("<td>A1</td>")));
  QVERIFY(html.contains(QStringLiteral("<td><b>灰岩</b></td>")));
  // 只有表头没有分隔行 → 不是表格，原样进段落。
  const QString lone = AiMarkdown::toHtml(QStringLiteral("| a | b |"));
  QVERIFY(!lone.contains(QStringLiteral("<table")));
  QVERIFY(lone.contains(QStringLiteral("| a | b |")));
}

void TestAiMarkdown::codeFence() {
  const QString html = AiMarkdown::toHtml(QStringLiteral(
    "```python\nimport gpkg\n  load(1)\n```\n"));
  QVERIFY(html.contains(QStringLiteral("<pre>import gpkg<br>  load(1)</pre>")));
  // 围栏内不解释 markdown（** 保持原样）。
  const QString raw = AiMarkdown::toHtml(QStringLiteral("```\n**not bold**\n```"));
  QVERIFY(raw.contains(QStringLiteral("**not bold**")));
  QVERIFY(!raw.contains(QStringLiteral("<b>")));
}

void TestAiMarkdown::unknownSyntaxFallsBackToPlainText() {
  // 文件、相对链接及图片不触发浏览器导航或资源请求。
  const QString html = AiMarkdown::toHtml(QStringLiteral(
    "[井位](file:///D:/wells.csv) [其他](../map) "
    "![附件](https://example.com/preview.png)"));
  QVERIFY(!html.contains(QStringLiteral("<a ")));
  QVERIFY(!html.contains(QStringLiteral("<img")));
  QVERIFY(html.contains(QStringLiteral("[井位](file:///D:/wells.csv)")));
  QVERIFY(html.contains(QStringLiteral("![附件](https://example.com/preview.png)")));
}

void TestAiMarkdown::quoteTaskAndNestedList() {
  const QString html = AiMarkdown::toHtml(QStringLiteral(
    "> **依据**\n> 相邻井\n\n- [x] 核对井位\n  先查井头\n  1. 对照岩性\n- [ ] 复测\n\n---"));
  QVERIFY(html.contains(QStringLiteral("<blockquote><p><b>依据</b><br>相邻井</p></blockquote>")));
  QVERIFY(html.contains(QStringLiteral("<hr>")));
  QVERIFY(html.contains(QStringLiteral("核对井位<br>先查井头<ol>")));
  QTextDocument doc;
  doc.setHtml(html);
  const auto checked = doc.find(QStringLiteral("核对井位")).block();
  const auto nested = doc.find(QStringLiteral("对照岩性")).block();
  const auto unchecked = doc.find(QStringLiteral("复测")).block();
  QVERIFY(checked.textList() && nested.textList());
  QCOMPARE(checked.blockFormat().marker(), QTextBlockFormat::MarkerType::Checked);
  QCOMPARE(unchecked.blockFormat().marker(), QTextBlockFormat::MarkerType::Unchecked);
  QVERIFY(nested.textList()->format().indent() > checked.textList()->format().indent());
}

void TestAiMarkdown::alignedTableInDocument() {
  QTextDocument doc;
  doc.setHtml(AiMarkdown::toHtml(QStringLiteral(
    "井号 | 顶深 (m)\r\n:--- | ---:\r\nA\\|1 | 1250.2")));
  const auto cursor = doc.find(QStringLiteral("1250.2"));
  const auto *table = cursor.currentTable();
  QVERIFY(table);
  QCOMPARE(table->columns(), 2);
  QCOMPARE(table->rows(), 2);
  QCOMPARE(table->cellAt(1, 0).firstCursorPosition().block().text(), QStringLiteral("A|1"));
  QVERIFY(cursor.blockFormat().alignment().testFlag(Qt::AlignRight));
}

void TestAiMarkdown::webLinksAndStrikethrough() {
  const QString html = AiMarkdown::toHtml(QStringLiteral(
    "[**资料**](https://example.com/well_(A)?x=1&y=2) ~~旧结论~~ `**原始**`"));
  QVERIFY(html.contains(QStringLiteral("x=1&amp;y=2")));
  QTextDocument doc;
  doc.setHtml(html);
  auto link = doc.find(QStringLiteral("资料")).charFormat();
  QCOMPARE(link.anchorHref(), QStringLiteral("https://example.com/well_(A)?x=1&y=2"));
  QVERIFY(doc.find(QStringLiteral("旧结论")).charFormat().fontStrikeOut());
  QVERIFY(doc.toPlainText().contains(QStringLiteral("**原始**")));
  QCOMPARE(AiMarkdown::toHtml(QStringLiteral("~~~~\na\n~~~\nb\n~~~~")),
           QStringLiteral("<pre>a<br>~~~<br>b</pre>"));
}

void TestAiMarkdown::delimitersAndLiteralIdentifiers() {
  const QString html = AiMarkdown::toHtml(QStringLiteral(
    "_可选_ __井号__ well_log_id **命令 `**` 结束** ``x`y`` \\*原文\\*"));
  QVERIFY(html.contains(QStringLiteral("<i>可选</i> <b>井号</b> well_log_id")));
  QVERIFY(html.contains(QStringLiteral("<b>命令 <code>**</code> 结束</b>")));
  QVERIFY(html.contains(QStringLiteral("<code>x`y</code> *原文*")));

  const QString incomplete = QStringLiteral("*open ").repeated(12000).trimmed();
  QCOMPARE(AiMarkdown::toHtml(incomplete), QStringLiteral("<p>%1</p>").arg(incomplete));
}

void TestAiMarkdown::scriptLiteralIsEscaped() {
  const QString html =
    AiMarkdown::toHtml(QStringLiteral("<script>alert(1)</script>"));
  QVERIFY(!html.contains(QStringLiteral("<script")));
  QVERIFY(html.contains(QStringLiteral("&lt;script&gt;")));
  QVERIFY(html.contains(QStringLiteral("&lt;/script&gt;")));
}

void TestAiMarkdown::imgOnerrorLiteralIsEscaped() {
  const QString html = AiMarkdown::toHtml(
    QStringLiteral("<img src=x onerror=alert(1)>"));
  QVERIFY(!html.contains(QStringLiteral("<img")));
  QVERIFY(html.contains(QStringLiteral("&lt;img")));
  QVERIFY(html.contains(QStringLiteral("&gt;")));
}

void TestAiMarkdown::injectionInsideTableAndCode() {
  const QString table = AiMarkdown::toHtml(QStringLiteral(
    "| a | b |\n|---|---|\n| <script> | <img onerror=x> |"));
  QVERIFY(!table.contains(QStringLiteral("<script")));
  QVERIFY(!table.contains(QStringLiteral("<img")));
  QVERIFY(table.contains(QStringLiteral("&lt;script&gt;")));

  const QString code =
    AiMarkdown::toHtml(QStringLiteral("```\n<b>bold</b>\n```"));
  QVERIFY(!code.contains(QStringLiteral("<b>")));
  QVERIFY(code.contains(QStringLiteral("&lt;b&gt;")));

  // 行内 code 里的标记同样先转义。
  const QString inlineCode =
    AiMarkdown::toHtml(QStringLiteral("看 `<b>` 标签"));
  QVERIFY(inlineCode.contains(QStringLiteral("<code>&lt;b&gt;</code>")));
}

QTEST_MAIN(TestAiMarkdown)
#include "tst_aimarkdown.moc"
