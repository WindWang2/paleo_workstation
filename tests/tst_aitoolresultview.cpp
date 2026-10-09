// 层：测试壳
#include <QtTest>
#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTextBrowser>
#include <QTextEdit>
#include <QTimer>
#include <QToolButton>
#include <functional>

#include "../src/ai/chat/chatmessage.h"
#include "../src/ai/chat/llmclient.h"
#include "../src/catalog/datacatalog.h"
#include "../src/ui/ai/aiassistdock.h"
#include "../src/ui/ai/aitoolresultview.h"
#include "../src/ui/pages/derivationgraph.h"
#include "../src/ui/paleotheme.h"
#include "../src/workflow/aichatcontroller.h"

// 方向93：AI 工具结果结构化视图（Oracle 验收面）。
//   · 分派映射：工具/topic → 视图类型；未知/失败/超限/空闭包 → 兜底折叠。
//   · 表格卡：内容与出参一致；复制 TSV 进剪贴板；行点击发导航意图。
//   · 键值对卡：summary 计数 / well_details 井头 + 关联表。
//   · 血缘小图：节点数与出参一致（渲染核 nodeCount 对拍）；「在血缘页打开」
//     发导航意图（assetId + versionId）。
//   · 兜底折叠块：默认收起、展开给全文 JSON；血缘超限降级注记如实。
//   · 端到端：dock 直连 runner::toolFinished（假服务器发 query_project 工具
//     帧 + 真 catalog 夹具）→ 卡内出现结构化视图。
// 只读红线由 tst_aichatprojectquery 的 catalog 快照对拍承载（执行面零改动）。
namespace {
// 与 tst_aiassistdock 相同骨架的假 LLM 端点（127.0.0.1，不发外网）：
// 工具帧一轮（query_project topic=wells）→ 终答一轮。
const char kToolRound[] =
  "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"cq1\","
  "\"type\":\"function\",\"function\":{\"name\":\"paleo.query_project\","
  "\"arguments\":\"{\\\"topic\\\":\\\"wells\\\"}\"}}]}}]}\n\n"
  "data: [DONE]\n\n";
const char kDoneRound[] =
  "data: {\"choices\":[{\"delta\":{\"content\":\"共两口井\"}}]}\n\n"
  "data: [DONE]\n\n";
} // namespace

class TestAiToolResultView : public QObject {
  Q_OBJECT
private slots:
  void initTestCase();
  void dispatchMappingCoversToolsAndFallsBack();
  void tableCardShowsWellsAndCopiesTsv();
  void tableRowClickEmitsNavigationIntents();
  void keyValueCardsRenderCountsAndWellDetails();
  void lineageGraphMatchesPayloadAndNavigates();
  void fallbackFoldStartsCollapsedAndNotesOverCap();
  void dockIntegrationAttachesStructuredView();
  void captureLedgerScreenshots();

private:
  static bool spinUntil(const std::function<bool()> &done, int timeoutMs = 15000) {
    QElapsedTimer clock;
    clock.start();
    while (!done() && clock.elapsed() < timeoutMs)
      QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return done();
  }
  static QJsonDocument parse(const QString &json) {
    return QJsonDocument::fromJson(json.toUtf8());
  }
  static QString wellsPayload();
  static QString assetsPayload();
  static QString summaryPayload();
  static QString wellDetailsPayload();
  static QString lineagePayload();
  static QString overCapLineagePayload();
};

void TestAiToolResultView::initTestCase() {
  PaleoTheme::pinRenderEnvironment(); // 渲染断言稳定（字体/主题钉死）
  QStandardPaths::setTestModeEnabled(true); // 会话存储进 .qttest 沙箱
}

// ---- Oracle 1：分派映射 ----
void TestAiToolResultView::dispatchMappingCoversToolsAndFallsBack() {
  const QString query = QStringLiteral("paleo.query_project");
  const QJsonObject ok = parse(QStringLiteral(R"({"tool":"x","topic":"wells"})")).object();
  QCOMPARE(aiToolResultViewKind(query, true,
                                parse(QStringLiteral(R"({"topic":"wells"})")).object()),
           AiToolResultViewKind::Table);
  QCOMPARE(aiToolResultViewKind(query, true,
                                parse(QStringLiteral(R"({"topic":"horizons"})")).object()),
           AiToolResultViewKind::Table);
  QCOMPARE(aiToolResultViewKind(query, true,
                                parse(QStringLiteral(R"({"topic":"assets"})")).object()),
           AiToolResultViewKind::Table);
  QCOMPARE(aiToolResultViewKind(query, true,
                                parse(QStringLiteral(R"({"topic":"summary"})")).object()),
           AiToolResultViewKind::KeyValue);
  QCOMPARE(aiToolResultViewKind(query, true,
                                parse(QStringLiteral(R"({"topic":"well_details"})")).object()),
           AiToolResultViewKind::KeyValue);
  QCOMPARE(aiToolResultViewKind(QStringLiteral("paleo.asset_lineage"), true,
                                parse(lineagePayload()).object()),
           AiToolResultViewKind::LineageGraph);
  // 空闭包 / 超上限 / 失败 / 未知工具 / 未知 topic：兜底折叠。
  QCOMPARE(aiToolResultViewKind(QStringLiteral("paleo.asset_lineage"), true,
                                parse(QStringLiteral(R"({"nodes":[]})")).object()),
           AiToolResultViewKind::Fallback);
  QCOMPARE(aiToolResultViewKind(QStringLiteral("paleo.asset_lineage"), true,
                                parse(overCapLineagePayload()).object()),
           AiToolResultViewKind::Fallback);
  // 精确边界：恰好 40 节点仍走小图（>40 才降级）。
  {
    QString nodes;
    for (int i = 0; i < kAiLineageMiniNodeCap; ++i) {
      if (i)
        nodes += QLatin1Char(',');
      nodes += QStringLiteral(R"({"version_id":"v%1"})").arg(i);
    }
    QCOMPARE(aiToolResultViewKind(
               QStringLiteral("paleo.asset_lineage"), true,
               parse(QStringLiteral(R"({"nodes":[%1]})").arg(nodes)).object()),
             AiToolResultViewKind::LineageGraph);
  }
  QCOMPARE(aiToolResultViewKind(query, false, ok), AiToolResultViewKind::Fallback);
  QCOMPARE(aiToolResultViewKind(QStringLiteral("paleo.tile_classification"), true,
                                parse(QStringLiteral(R"({"rows":1})")).object()),
           AiToolResultViewKind::Fallback);
  QCOMPARE(aiToolResultViewKind(QStringLiteral("paleo.no_such_tool"), true, ok),
           AiToolResultViewKind::Fallback);
  QCOMPARE(aiToolResultViewKind(query, true,
                                parse(QStringLiteral(R"({"topic":"???"})")).object()),
           AiToolResultViewKind::Fallback);
}

// ---- Oracle 3：表格卡内容 + 剪贴板 ----
QString TestAiToolResultView::wellsPayload() {
  return QStringLiteral(
    R"({"tool":"paleo.query_project","topic":"wells","project_dir":"p","count":2,)"
    R"("wells":[{"name":"A-1","id":"ent-a1","type":"well","td":2450.5},)"
    R"({"name":"B-2","id":"ent-b2","type":"planned","td":0}]})");
}

QString TestAiToolResultView::assetsPayload() {
  return QStringLiteral(
    R"({"tool":"paleo.query_project","topic":"assets","project_dir":"p","count":2,)"
    R"("assets":[{"name":"H1","id":"asset-h1","type":"horizon","format":"grd",)"
    R"("version_id":"v1","stage":"DERIVED","version_number":2,"path":"h1.grd"},)"
    R"({"name":"W1-log","id":"asset-w1l","type":"well_log","format":"las",)"
    R"("version_id":"v2","stage":"RAW","version_number":1,"path":"w1.las"}]})");
}

void TestAiToolResultView::tableCardShowsWellsAndCopiesTsv() {
  AiToolResultView view(QStringLiteral("paleo.query_project"), true, wellsPayload());
  QCOMPARE(view.kind(), AiToolResultViewKind::Table);
  auto *table = view.findChild<QTableWidget *>(QStringLiteral("aiResultTable"));
  QVERIFY(table);
  // 内容与出参一致（Oracle 3：对拍 wells 数组）。
  QCOMPARE(table->rowCount(), 2);
  QCOMPARE(table->columnCount(), 3);
  QCOMPARE(table->item(0, 0)->text(), QStringLiteral("A-1"));
  QCOMPARE(table->item(0, 2)->text(), QStringLiteral("2450.5"));
  QCOMPARE(table->item(1, 0)->text(), QStringLiteral("B-2"));
  QVERIFY(table->item(1, 1)->text().contains(QStringLiteral("计划")));
  // 复制到剪贴板：表头 + 行，TSV（Oracle 3）。
  auto *copy =
    view.findChild<QPushButton *>(QStringLiteral("aiResultCopyTable"));
  QVERIFY(copy);
  copy->click();
  const QString tsv = QApplication::clipboard()->text();
  QVERIFY2(tsv.contains(QStringLiteral("井名")), qPrintable(tsv));
  QVERIFY2(tsv.contains(QStringLiteral("A-1\t")), qPrintable(tsv));
  QVERIFY2(tsv.contains(QStringLiteral("2450.5")), qPrintable(tsv));
  QVERIFY(tsv.contains(QLatin1Char('\n')));
}

// ---- Oracle 3：行点击 → 导航意图（资产定位 / 井实体定位）----
void TestAiToolResultView::tableRowClickEmitsNavigationIntents() {
  {
    AiToolResultView view(QStringLiteral("paleo.query_project"), true,
                          assetsPayload());
    auto *table = view.findChild<QTableWidget *>(QStringLiteral("aiResultTable"));
    QVERIFY(table);
    QSignalSpy spy(&view, &AiToolResultView::assetNavigateRequested);
    QTest::mouseClick(table->viewport(), Qt::LeftButton, Qt::NoModifier,
                      table->visualRect(table->model()->index(0, 0)).center());
    QCOMPARE(spy.size(), 1);
    QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("asset-h1"));
  }
  {
    AiToolResultView view(QStringLiteral("paleo.query_project"), true,
                          wellsPayload());
    auto *table = view.findChild<QTableWidget *>(QStringLiteral("aiResultTable"));
    QVERIFY(table);
    QSignalSpy spy(&view, &AiToolResultView::entityNavigateRequested);
    QTest::mouseClick(table->viewport(), Qt::LeftButton, Qt::NoModifier,
                      table->visualRect(table->model()->index(1, 0)).center());
    QCOMPARE(spy.size(), 1);
    QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("ent-b2"));
  }
}

// ---- 键值对卡：summary 计数 + well_details 井头/关联 ----
QString TestAiToolResultView::summaryPayload() {
  return QStringLiteral(
    R"({"tool":"paleo.query_project","topic":"summary","project_dir":"p",)"
    R"("entities":{"well":2,"planned":1},"assets_by_type":{"horizon":3},)"
    R"("revision":7})");
}

QString TestAiToolResultView::wellDetailsPayload() {
  return QStringLiteral(
    R"({"tool":"paleo.query_project","topic":"well_details","project_dir":"p",)"
    R"("well":"A-1","well_id":"ent-a1","links":[{"role":"well_log",)"
    R"("asset":"A-1 主测井","asset_id":"asset-w1l","primary":true,)"
    R"("version_id":"v2","stage":"RAW","version_number":1,"path":"w1.las"}]})");
}

void TestAiToolResultView::keyValueCardsRenderCountsAndWellDetails() {
  {
    AiToolResultView view(QStringLiteral("paleo.query_project"), true,
                          summaryPayload());
    QCOMPARE(view.kind(), AiToolResultViewKind::KeyValue);
    auto *kv = view.findChild<QTableWidget *>(QStringLiteral("aiResultKeyValue"));
    QVERIFY(kv);
    QString all;
    for (int r = 0; r < kv->rowCount(); ++r)
      all += kv->item(r, 0)->text() + QLatin1Char('=') +
             kv->item(r, 1)->text() + QLatin1Char('\n');
    QVERIFY2(all.contains(QStringLiteral("well=2")), qPrintable(all));
    QVERIFY2(all.contains(QStringLiteral("horizon=3")), qPrintable(all));
    QVERIFY2(all.contains(QStringLiteral("7")), qPrintable(all));
  }
  {
    AiToolResultView view(QStringLiteral("paleo.query_project"), true,
                          wellDetailsPayload());
    QCOMPARE(view.kind(), AiToolResultViewKind::KeyValue);
    auto *kv = view.findChild<QTableWidget *>(QStringLiteral("aiResultKeyValue"));
    auto *links = view.findChild<QTableWidget *>(QStringLiteral("aiResultTable"));
    QVERIFY(kv && links);
    QCOMPARE(links->rowCount(), 1);
    QCOMPARE(links->item(0, 1)->text(), QStringLiteral("A-1 主测井"));
    QSignalSpy spy(&view, &AiToolResultView::assetNavigateRequested);
    QTest::mouseClick(links->viewport(), Qt::LeftButton, Qt::NoModifier,
                      links->visualRect(links->model()->index(0, 0)).center());
    QCOMPARE(spy.size(), 1);
    QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("asset-w1l"));
  }
}

// ---- Oracle 2：血缘小图节点数对拍 + 跳转信号 ----
QString TestAiToolResultView::lineagePayload() {
  return QStringLiteral(
    R"({"tool":"paleo.asset_lineage","asset":"H1","asset_id":"asset-h1",)"
    R"("version_id":"ver-3","available":true,)"
    R"("nodes":[)"
    R"({"version_id":"ver-1","asset":"W1 井数据","asset_id":"asset-w1","stage":"RAW",)"
    R"("kind":"raw","stale":false,"column":0,"version_name":"v1","path":"w1"},)"
    R"({"version_id":"ver-2","asset":"H1 中间面","asset_id":"asset-h1i","stage":"DERIVED",)"
    R"("kind":"derived","stale":false,"column":1,"version_name":"v1","path":"m"},)"
    R"({"version_id":"ver-3","asset":"H1","asset_id":"asset-h1","stage":"DERIVED",)"
    R"("kind":"derived","stale":false,"column":2,"version_name":"v2","path":"h"})"
    R"(],"edges":[{"parent":"ver-1","child":"ver-2"},)"
    R"({"parent":"ver-2","child":"ver-3"}],)"
    R"("selection":{"count":3,"version_ids":["ver-1","ver-2","ver-3"]},)"
    R"("collapsed_upstream":0,"collapsed_downstream":0,"collapsed_seeds":0})");
}

QString TestAiToolResultView::overCapLineagePayload() {
  QString nodes;
  for (int i = 0; i <= kAiLineageMiniNodeCap; ++i) { // 上限 + 1 = 41 节点
    if (i)
      nodes += QLatin1Char(',');
    nodes += QStringLiteral(
      R"({"version_id":"v%1","asset":"a%1","asset_id":"a%1","stage":"RAW",)"
      R"("kind":"raw","stale":false,"column":%1,"version_name":"v%1","path":"p"})")
      .arg(i);
  }
  return QStringLiteral(
    R"({"tool":"paleo.asset_lineage","asset":"X","asset_id":"ax",)"
    R"("version_id":"v0","available":true,"nodes":[%1],"edges":[],)"
    R"("selection":{"count":41,"version_ids":[]}})")
    .arg(nodes);
}

void TestAiToolResultView::lineageGraphMatchesPayloadAndNavigates() {
  AiToolResultView view(QStringLiteral("paleo.asset_lineage"), true,
                        lineagePayload());
  QCOMPARE(view.kind(), AiToolResultViewKind::LineageGraph);
  auto *graph =
    view.findChild<DerivationGraph *>(QStringLiteral("aiResultLineageGraph"));
  QVERIFY(graph);
  // 节点数与出参一致（Oracle 2：3 节点闭包）。
  QCOMPARE(graph->nodeCount(), 3);
  // 「在血缘页打开」→ 导航意图带种子资产 + 版本（Oracle 2）。
  auto *open =
    view.findChild<QPushButton *>(QStringLiteral("aiResultOpenLineage"));
  QVERIFY(open);
  QSignalSpy spy(&view, &AiToolResultView::lineageNavigateRequested);
  open->click();
  QCOMPARE(spy.size(), 1);
  QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("asset-h1"));
  QCOMPARE(spy.at(0).at(1).toString(), QStringLiteral("ver-3"));
}

// ---- Oracle 1 兜底：折叠块默认收起；超限降级注记如实 ----
void TestAiToolResultView::fallbackFoldStartsCollapsedAndNotesOverCap() {
  {
    const QString json = QStringLiteral(
      R"({"tool":"paleo.well_facies_prediction","well":"A-1","intervals":12})");
    AiToolResultView view(QStringLiteral("paleo.well_facies_prediction"), true,
                          json);
    QCOMPARE(view.kind(), AiToolResultViewKind::Fallback);
    auto *body = view.findChild<QTextBrowser *>(QStringLiteral("aiResultJsonBody"));
    auto *toggle = view.findChild<QToolButton *>(QStringLiteral("aiResultJsonToggle"));
    QVERIFY(body && toggle);
    QVERIFY(!body->isVisibleTo(&view)); // 默认收起（isVisibleTo 防御未 show 的父窗口）
    toggle->toggle();
    QVERIFY(body->isVisibleTo(&view));
    QVERIFY2(body->toPlainText().contains(QStringLiteral("\"intervals\": 12")),
             qPrintable(body->toPlainText()));
    toggle->toggle();
    QVERIFY(!body->isVisibleTo(&view));
  }
  {
    AiToolResultView view(QStringLiteral("paleo.asset_lineage"), true,
                          overCapLineagePayload());
    QCOMPARE(view.kind(), AiToolResultViewKind::Fallback);
    // 超限注记如实（含上限数字），完整事实在折叠 JSON 里。
    const QString all =
      [&view] {
        QString text;
        for (QLabel *label : view.findChildren<QLabel *>())
          text += label->text() + QLatin1Char('\n');
        return text;
      }();
    QVERIFY2(all.contains(QString::number(kAiLineageMiniNodeCap)),
             qPrintable(all));
  }
}

// ---- 端到端：dock 直连 runner::toolFinished → 卡内结构化视图 ----

namespace {
class FakeToolServer : public QObject {
  Q_OBJECT
public:
  bool start() {
    connect(&m_server, &QTcpServer::newConnection, this,
            [this] { serve(m_server.nextPendingConnection()); });
    return m_server.listen(QHostAddress::LocalHost, 0);
  }
  QUrl endpoint() const {
    return QUrl(QStringLiteral("http://127.0.0.1:%1/v1")
                  .arg(m_server.serverPort()));
  }

private:
  void serve(QTcpSocket *sock) {
    if (!sock)
      return;
    connect(sock, &QTcpSocket::disconnected, sock, &QObject::deleteLater);
    auto buf = std::make_shared<QByteArray>();
    connect(sock, &QTcpSocket::readyRead, sock, [this, sock, buf] {
      buf->append(sock->readAll());
      const int headerEnd = buf->indexOf("\r\n\r\n");
      if (headerEnd < 0)
        return;
      const QString header = QString::fromUtf8(buf->left(headerEnd));
      const int contentLength =
        header.section("Content-Length:", 1, 1).section("\r", 0, 0).trimmed().toInt();
      if (buf->mid(headerEnd + 4).size() < contentLength)
        return;
      *buf = buf->mid(headerEnd + 4 + contentLength);
      const bool toolRound = m_requestCount++ == 0;
      const QByteArray body = toolRound ? QByteArray(kToolRound)
                                        : QByteArray(kDoneRound);
      QTimer::singleShot(20, sock, [sock, body] {
        sock->write("HTTP/1.1 200 OK\r\nConnection: close\r\n"
                    "Content-Type: text/event-stream\r\n\r\n");
        sock->write(body);
        sock->disconnectFromHost();
      });
    });
  }
  QTcpServer m_server;
  int m_requestCount = 0;
};
} // namespace

void TestAiToolResultView::dockIntegrationAttachesStructuredView() {
  QTemporaryDir dir;
  DataCatalog catalog;
  QVERIFY(dir.isValid() && catalog.open(dir.path()));
  CatalogEntity well;
  well.id = QStringLiteral("ent-a1");
  well.entityType = QStringLiteral("well");
  well.name = QStringLiteral("A-1");
  well.td = 2450.5;
  well.hasSurface = false;
  catalog.addEntity(well);

  FakeToolServer server;
  QVERIFY(server.start());
  AiChatController controller;
  LlmConfig config;
  config.endpoint = server.endpoint();
  config.model = QStringLiteral("test-model");
  config.stream = true;
  config.timeoutMs = 5000;
  config.apiKey = "sk-test-fixture";
  controller.setConfig(config);
  AiChatToolRunner::Context context;
  context.catalog = &catalog;
  context.projectDir = dir.path();
  controller.toolRunner()->setContext(context);
  AiAssistDock dock(&controller);

  dock.findChild<QTextEdit *>(QStringLiteral("aiAssistantInput"))
    ->setPlainText(QStringLiteral("工程里有哪些井？"));
  dock.findChild<QPushButton *>(QStringLiteral("aiAssistantSend"))->click();
  QVERIFY2(spinUntil([&controller] { return !controller.streaming(); }, 20000),
           "工具轮 + 终答轮必须收敛");
  // 卡内出现表格视图（wells → Table），内容来自真实 catalog 枚举。
  auto *table = dock.findChild<QTableWidget *>(QStringLiteral("aiResultTable"));
  QVERIFY2(table, "端到端：工具结果必须落成结构化表格视图");
  QCOMPARE(table->item(0, 0)->text(), QStringLiteral("A-1"));
  // 导航意图透传到面板级信号（宿主接线面）。
  QSignalSpy spy(&dock, &AiAssistDock::entityNavigateRequested);
  QTest::mouseClick(table->viewport(), Qt::LeftButton, Qt::NoModifier,
                    table->visualRect(table->model()->index(0, 0)).center());
  QCOMPARE(spy.size(), 1);
  QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("ent-a1"));
  // 协议面零变更证据：role=tool 消息照常回灌（控制器历史含工具应答）。
  bool hasToolMessage = false;
  for (const ChatMessage &message : controller.messages())
    if (message.role == ChatRole::Tool && message.toolCallId ==
                                            QStringLiteral("cq1"))
      hasToolMessage = true;
  QVERIFY(hasToolMessage);
  QString error;
  ChatSessionStore::remove(controller.session().id, &error);
}

// 方向93 ledger 截图（无人值守证据）：默认跳过；设 PALEO_AI_RESULT_SHOT_DIR
// 时落三张 PNG（表格卡 / 键值对+关联卡 / 血缘小图卡）。产物提交
// docs/progress/ai-resultviz-shots/（先例：ai-ux-shots）。
void TestAiToolResultView::captureLedgerScreenshots() {
  const QString dir = QString::fromUtf8(qgetenv("PALEO_AI_RESULT_SHOT_DIR"));
  if (dir.isEmpty())
    QSKIP("截图按需采集（PALEO_AI_RESULT_SHOT_DIR 未设）");
  struct Shot {
    const char *name;
    AiToolResultView *view;
  } shots[] = {
    {"ai-resultviz-table",
     new AiToolResultView(QStringLiteral("paleo.query_project"), true,
                          wellsPayload(), nullptr)},
    {"ai-resultviz-welldetails",
     new AiToolResultView(QStringLiteral("paleo.query_project"), true,
                          wellDetailsPayload(), nullptr)},
    {"ai-resultviz-lineage",
     new AiToolResultView(QStringLiteral("paleo.asset_lineage"), true,
                          lineagePayload(), nullptr)},
    {"ai-resultviz-fallback",
     new AiToolResultView(QStringLiteral("paleo.well_facies_prediction"), true,
                          QStringLiteral(
                            R"({"tool":"paleo.well_facies_prediction",)"
                            R"("well":"A-1","model":"f1","intervals":12})"),
                          nullptr)},
  };
  for (const Shot &shot : shots) {
    shot.view->resize(420, 300);
    shot.view->show();
    QVERIFY2(shot.view->grab().save(
               dir + QStringLiteral("/") + shot.name + QStringLiteral(".png")),
             "截图落盘失败");
    shot.view->deleteLater();
  }
  QCoreApplication::processEvents(); // grab 后清事件队列（延迟 fit 也在其中）
}

QTEST_MAIN(TestAiToolResultView)
#include "tst_aitoolresultview.moc"