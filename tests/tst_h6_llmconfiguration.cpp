// 层：测试壳
#include <QtTest>
#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <functional>

#include "../src/ai/chat/llmclient.h"
#include "../src/ui/ai/aiassistdock.h"
#include "../src/ui/ai/aiconfigurationwiring.h"
#include "../src/ui/ai/llmconfigdialog.h"
#include "../src/ui/ai/llmdiagnosticspanel.h"
#include "../src/ui/paleotheme.h"
#include "../src/workflow/aichatcontroller.h"
#include "../src/workflow/llmconnectiondiagnostics.h"

// H6 配置面回归：真实 LlmConfigDialog + openAiConfiguration 闭环（视图意图
// 与功能层协调器对接），覆盖 W1 表单往返（totalTimeoutMs 保留、maxTokens
// 域守卫）、W2 凭据来源判定（不改 applyConfig 提交合同）、W7 诊断接缝
// （只填草稿、零 HTTP 于打开/编辑/关闭）、目录缺席→手填模型→文本探测→
// 仅原 apply 生效的完整用户路径。
// 密钥红线：全程 PALEO_LLM_NO_KEYCHAIN=1；明文密钥不出现在任何 label。

namespace {

constexpr const char kCurrentKey[] = "sk-current-123456789";

bool spinUntil(const std::function<bool()> &predicate, int timeoutMs = 8000) {
  QElapsedTimer clock;
  clock.start();
  while (!predicate() && clock.elapsed() < timeoutMs)
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  return predicate();
}

// 最小脚本服务器（同 tst_h6_modeldiagnostics 口径：记录原始请求）。
class ScriptServer : public QObject {
  Q_OBJECT
public:
  struct Request {
    QByteArray method;
    QByteArray path;
    QByteArray auth;
    QByteArray body;
  };
  struct Response {
    int status = 200;
    QByteArray contentType = "application/json";
    QList<QByteArray> writes;
  };
  using Handler = std::function<Response(const Request &)>;

  bool start(Handler handler) {
    m_handler = std::move(handler);
    connect(&m_server, &QTcpServer::newConnection, this, [this] {
      while (m_server.hasPendingConnections())
        serve(m_server.nextPendingConnection());
    });
    return m_server.listen(QHostAddress::LocalHost, 0);
  }
  QUrl endpoint() const {
    return QUrl(QStringLiteral("http://127.0.0.1:%1/v1").arg(m_server.serverPort()));
  }
  int requestCount() const { return m_requests.size(); }
  const QList<Request> &requests() const { return m_requests; }

private:
  void serve(QTcpSocket *sock) {
    connect(sock, &QTcpSocket::disconnected, sock, &QObject::deleteLater);
    auto buffer = std::make_shared<QByteArray>();
    connect(sock, &QTcpSocket::readyRead, sock, [this, sock, buffer] {
      buffer->append(sock->readAll());
      const int headerEnd = buffer->indexOf("\r\n\r\n");
      if (headerEnd < 0)
        return;
      qsizetype length = 0;
      const QList<QByteArray> lines = buffer->left(headerEnd).split('\n');
      Request request;
      for (const QByteArray &line : lines) {
        if (line.toLower().startsWith("content-length:"))
          length = line.mid(15).trimmed().toLongLong();
        else if (line.toLower().startsWith("authorization:"))
          request.auth = line.mid(14).trimmed();
      }
      if (buffer->size() < headerEnd + 4 + length)
        return;
      request.method = lines.first().trimmed().split(' ').value(0);
      request.path = lines.first().trimmed().split(' ').value(1);
      request.body = buffer->mid(headerEnd + 4, length);
      m_requests.append(request);
      const Response response = m_handler(request);
      sock->write("HTTP/1.1 " + QByteArray::number(response.status)
                  + " OK\r\nConnection: close\r\nContent-Type: "
                  + response.contentType + "\r\n\r\n");
      for (const QByteArray &chunk : response.writes)
        sock->write(chunk);
      sock->disconnectFromHost();
    });
  }

  Handler m_handler;
  QTcpServer m_server;
  QList<Request> m_requests;
};

QByteArray completionBody(const QString &content) {
  QJsonObject choice;
  choice.insert("index", 0);
  choice.insert("finish_reason", QStringLiteral("stop"));
  choice.insert("message", QJsonObject{{"content", content}});
  QJsonObject body;
  body.insert("choices", QJsonArray{choice});
  return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

QByteArray configBytes() {
  QFile file(LlmConfig::path());
  if (!file.open(QIODevice::ReadOnly))
    return QByteArray();
  return file.readAll();
}

LlmConfig currentConfigFor(const QUrl &endpoint) {
  LlmConfig config;
  config.endpoint = endpoint;
  config.model = QStringLiteral("geo-7b");
  config.timeoutMs = 45000;
  config.totalTimeoutMs = 1234567;
  config.maxTokens = 2048;
  config.apiKey = QByteArray(kCurrentKey);
  return config;
}

} // namespace

class TestH6LlmConfiguration : public QObject {
  Q_OBJECT
private slots:
  void initTestCase();
  void cleanupTestCase();
  void openEditCloseZeroHttp();
  void totalTimeoutMsPreservedAcrossFormRoundTrip();
  void maxTokensOverDomainVisibleAndApplyBlocked();
  void candidateKeyRulesInDialog();
  void modelSelectionFillsDraftOnlyNoApply();
  void diagnosticsEndpointChangeWithoutKeyLocalReject();
  void realUiClosedLoopViaOpenAiConfiguration();
};

void TestH6LlmConfiguration::initTestCase() {
  PaleoTheme::pinRenderEnvironment(); // Fusion + 主题 palette 钉死
  QStandardPaths::setTestModeEnabled(true);
  qputenv("PALEO_LLM_NO_KEYCHAIN", "1");
  qunsetenv("PALEO_LLM_ENDPOINT");
  qunsetenv("PALEO_LLM_MODEL");
  qunsetenv("PALEO_LLM_API_KEY");
  QFile::remove(LlmConfig::path());
}

void TestH6LlmConfiguration::cleanupTestCase() {
  qunsetenv("PALEO_LLM_NO_KEYCHAIN");
  QFile::remove(LlmConfig::path());
}

void TestH6LlmConfiguration::openEditCloseZeroHttp() {
  ScriptServer server;
  QVERIFY(server.start([](const ScriptServer::Request &) {
    return ScriptServer::Response{200, "application/json", {completionBody("x")}};
  }));
  const QByteArray before = configBytes();
  {
    LlmConfigDialog dialog(currentConfigFor(server.endpoint()));
    dialog.findChild<QLineEdit *>(QStringLiteral("llmConfigEndpoint"))
      ->setText(server.endpoint().toString());
    dialog.findChild<QLineEdit *>(QStringLiteral("llmConfigModel"))
      ->setText(QStringLiteral("edited-model"));
    dialog.findChild<QSpinBox *>(QStringLiteral("llmConfigMaxTokens"))
      ->setValue(4096);
    dialog.findChild<QSpinBox *>(QStringLiteral("llmConfigTimeout"))
      ->setValue(60000);
    dialog.findChild<QCheckBox *>(QStringLiteral("llmConfigStream"))
      ->setChecked(false);
    // 诊断面板可见但零点击：打开/编辑/关闭零联网（W2 生命周期锚点）。
    QVERIFY(dialog.findChild<QPushButton *>(QStringLiteral("llmDiagCatalog")));
  }
  QCOMPARE(server.requestCount(), 0);
  QCOMPARE(configBytes(), before); // 零配置保存
}

void TestH6LlmConfiguration::totalTimeoutMsPreservedAcrossFormRoundTrip() {
  LlmConfig current = currentConfigFor(QUrl(QStringLiteral("https://llm.example.com/v1")));
  LlmConfigDialog dialog(current);
  dialog.findChild<QLineEdit *>(QStringLiteral("llmConfigModel"))
    ->setText(QStringLiteral("geo-7b-edited"));
  QSignalSpy applied(&dialog, &LlmConfigDialog::applyRequested);
  auto *buttons = dialog.findChild<QDialogButtonBox *>();
  QVERIFY(buttons);
  buttons->button(QDialogButtonBox::Ok)->click();
  QCOMPARE(applied.size(), 1);
  const LlmConfig form = applied.at(0).at(0).value<LlmConfig>();
  // H6-W1：无 UI 项的 totalTimeoutMs 原样流转——不被默认 300000 覆盖；
  // 其余字段保持打开时快照（只改了模型名）。
  QCOMPARE(form.totalTimeoutMs, 1234567);
  QCOMPARE(form.model, QStringLiteral("geo-7b-edited"));
  QCOMPARE(form.maxTokens, 2048);
  QCOMPARE(form.timeoutMs, 45000);
  QCOMPARE(form.endpoint, current.endpoint);
}

void TestH6LlmConfiguration::maxTokensOverDomainVisibleAndApplyBlocked() {
  LlmConfig current = currentConfigFor(QUrl(QStringLiteral("https://llm.example.com/v1")));
  current.maxTokens = 40000; // 超持久化读取域 [1, 32768]（来源：手改 JSON 等）
  LlmConfigDialog dialog(current);
  // 打开时保留显示原值（不无声裁剪）。
  QCOMPARE(dialog.findChild<QSpinBox *>(QStringLiteral("llmConfigMaxTokens"))
             ->value(),
           40000);
  auto *note = dialog.findChild<QLabel *>(QStringLiteral("llmConfigNote"));
  QVERIFY(note);
  QVERIFY(note->text().contains(QStringLiteral("32768"))); // 打开即诚实提示
  QSignalSpy applied(&dialog, &LlmConfigDialog::applyRequested);
  auto *buttons = dialog.findChild<QDialogButtonBox *>();
  dialog.findChild<QSpinBox *>(QStringLiteral("llmConfigMaxTokens"))
    ->setValue(200000);
  buttons->button(QDialogButtonBox::Ok)->click();
  QCOMPARE(applied.size(), 0); // 超域可见拒绝
  QVERIFY(note->text().contains(QStringLiteral("32768")));
  QCOMPARE(dialog.findChild<QSpinBox *>(QStringLiteral("llmConfigMaxTokens"))
             ->value(),
           200000); // 不静默改值
  // 域内值恢复可应用（不抬高 load 上限、不放宽校验）。
  dialog.findChild<QSpinBox *>(QStringLiteral("llmConfigMaxTokens"))
    ->setValue(5000);
  buttons->button(QDialogButtonBox::Ok)->click();
  QCOMPARE(applied.size(), 1);
  QCOMPARE(applied.at(0).at(0).value<LlmConfig>().maxTokens, 5000);
}

void TestH6LlmConfiguration::candidateKeyRulesInDialog() {
  const QUrl endpointA(QStringLiteral("https://llm.example.com/v1"));
  LlmConfig current = currentConfigFor(endpointA);
  LlmConfigDialog dialog(current);
  auto *endpoint = dialog.findChild<QLineEdit *>(QStringLiteral("llmConfigEndpoint"));
  auto *keyInput = dialog.findChild<QLineEdit *>(QStringLiteral("llmConfigKeyInput"));
  QVERIFY(endpoint && keyInput);
  endpoint->setText(endpointA.toString());

  // a) 本次显式输入：仅用于诊断候选（来源诚实标注）。
  keyInput->setText(QStringLiteral("sk-typed-987654321"));
  paleo::chat::LlmDiagnosticCandidate typed = dialog.buildDiagnosticCandidate();
  QCOMPARE(typed.keySource, paleo::chat::LlmDiagnosticCandidate::KeySource::ExplicitInput);
  QCOMPARE(typed.key, QByteArrayLiteral("sk-typed-987654321"));

  // b) 端点身份一致（含 base path）：沿用当前内存值。
  keyInput->clear();
  paleo::chat::LlmDiagnosticCandidate reuse = dialog.buildDiagnosticCandidate();
  QCOMPARE(reuse.keySource, paleo::chat::LlmDiagnosticCandidate::KeySource::InMemoryCurrent);
  QCOMPARE(reuse.key, QByteArray(kCurrentKey));

  // c) 端点 base path 变更且未输入目标凭据：无凭据——绝不静默搬运旧 key。
  endpoint->setText(QStringLiteral("https://other-tenant.example.com/v2"));
  paleo::chat::LlmDiagnosticCandidate moved = dialog.buildDiagnosticCandidate();
  QCOMPARE(moved.keySource, paleo::chat::LlmDiagnosticCandidate::KeySource::None);
  QVERIFY(moved.key.isEmpty());
  // host 相同但路径不同同样不等（「不能仅比较 host」口径）。
  endpoint->setText(QStringLiteral("https://llm.example.com/v2"));
  QVERIFY(dialog.buildDiagnosticCandidate().key.isEmpty());
}

void TestH6LlmConfiguration::modelSelectionFillsDraftOnlyNoApply() {
  ScriptServer server;
  QVERIFY(server.start([](const ScriptServer::Request &) {
    return ScriptServer::Response{200, "application/json", {completionBody("x")}};
  }));
  LlmConfigDialog dialog(currentConfigFor(server.endpoint()));
  const QByteArray before = configBytes();
  QSignalSpy applied(&dialog, &LlmConfigDialog::applyRequested);
  dialog.setModelCandidates(QStringList{QStringLiteral("geo-7b"),
                                        QStringLiteral("geo-13b")});
  auto *list = dialog.findChild<QListWidget *>(QStringLiteral("llmDiagModelList"));
  auto *use = dialog.findChild<QPushButton *>(QStringLiteral("llmDiagUseModel"));
  QVERIFY(list && use);
  QVERIFY(use->isEnabled());
  list->setCurrentRow(1);
  use->click();
  // 只改草稿：模型字段 = 精确条目；零 apply 意图、零配置保存、零网络。
  QCOMPARE(dialog.findChild<QLineEdit *>(QStringLiteral("llmConfigModel"))->text(),
           QStringLiteral("geo-13b"));
  QCOMPARE(applied.size(), 0);
  QCOMPARE(server.requestCount(), 0);
  QCOMPARE(configBytes(), before);
}

void TestH6LlmConfiguration::diagnosticsEndpointChangeWithoutKeyLocalReject() {
  // 两个本地端点：草稿切到 serverB 且未输入目标凭据 → 本地拒绝零网络；
  // 切回 serverA（身份一致）→ 沿用当前内存 key 真实探测成功。
  ScriptServer serverA;
  QVERIFY(serverA.start([](const ScriptServer::Request &) {
    return ScriptServer::Response{200, "application/json", {completionBody("ok")}};
  }));
  ScriptServer serverB;
  QVERIFY(serverB.start([](const ScriptServer::Request &) {
    return ScriptServer::Response{200, "application/json", {completionBody("B")}};
  }));
  LlmConfigDialog dialog(currentConfigFor(serverA.endpoint()));
  paleo::chat::LlmConnectionDiagnostics diagnostics(&dialog);
  QObject::connect(&dialog, &LlmConfigDialog::diagnosticsActionRequested, &dialog,
                   [&](int action) {
    if (action == static_cast<int>(LlmDiagnosticsPanel::Action::Cancel))
      diagnostics.cancel();
    else
      diagnostics.runAction(
        static_cast<paleo::chat::LlmDiagnosticsAction>(action),
        dialog.buildDiagnosticCandidate());
  });
  QObject::connect(&diagnostics,
                   &paleo::chat::LlmConnectionDiagnostics::candidateRejected,
                   &dialog, [&dialog](const QString &reason) {
    dialog.diagnosticsPanel()->showNotice(reason);
  });
  auto *endpoint = dialog.findChild<QLineEdit *>(QStringLiteral("llmConfigEndpoint"));
  endpoint->setText(serverB.endpoint().toString());
  auto *text = dialog.findChild<QPushButton *>(QStringLiteral("llmDiagText"));
  text->click();
  QVERIFY(spinUntil([&] { return !diagnostics.busy(); }));
  // 本地拒绝呈现于面板提示；serverB 零请求（旧 key 不发往新目标）。
  QVERIFY(!dialog.diagnosticsPanel()
               ->findChild<QLabel *>(QStringLiteral("llmDiagNotice"))
               ->text()
               .isEmpty());
  QCOMPARE(serverB.requestCount(), 0);

  endpoint->setText(serverA.endpoint().toString());
  text->click();
  QVERIFY(spinUntil([&] { return diagnostics.report().text.status
                              == paleo::chat::LlmDiagnosticStatus::Passed; }));
  QCOMPARE(serverA.requestCount(), 1);
  QCOMPARE(serverA.requests().first().auth, QByteArray("Bearer ") + QByteArray(kCurrentKey));
  // 诊断不写存储、不改配置快照（诊断面零落盘）。
  QVERIFY(!QFile::exists(LlmConfig::path()));
}

void TestH6LlmConfiguration::realUiClosedLoopViaOpenAiConfiguration() {
  // 完整用户路径（真实 openAiConfiguration 装配）：目录 404 → 手填模型 →
  // 文本探测成功 → 仅原显式 apply 提交生效（沿用当前密钥）。
  ScriptServer server;
  QVERIFY(server.start([](const ScriptServer::Request &request) {
    if (request.method == "GET")
      return ScriptServer::Response{404, "application/json", {}};
    return ScriptServer::Response{200, "application/json",
                                  {completionBody(QStringLiteral("诊断正常"))}};
  }));
  LlmConfig current;
  current.endpoint = server.endpoint();
  current.model = QString(); // 当前无模型（助手禁用态）
  current.apiKey = QByteArray(kCurrentKey);
  AiChatController controller;
  controller.setConfig(current);
  AiAssistDock dock(&controller);
  const QByteArray beforeConfig = configBytes();

  QTimer::singleShot(0, &dock, [&] {
    auto *dialog = dock.findChild<LlmConfigDialog *>();
    QVERIFY(dialog);
    auto *report =
      dialog->findChild<QPlainTextEdit *>(QStringLiteral("llmDiagReport"));
    auto *notice =
      dialog->findChild<QLabel *>(QStringLiteral("llmDiagNotice"));
    auto *catalog = dialog->findChild<QPushButton *>(QStringLiteral("llmDiagCatalog"));
    auto *text = dialog->findChild<QPushButton *>(QStringLiteral("llmDiagText"));
    QVERIFY(report && catalog && text);
    // 1) 目录缺席：分项如实 Unsupported；无候选列表。
    catalog->click();
    // 完成判据 = 按钮恢复可用（busy 结束）且报告出现目录分项——「进行中」
    // 阶段按钮同样禁用，不能当终态。
    const bool catalogDone = spinUntil([&] {
      return report->toPlainText().contains(QStringLiteral("模型目录："))
             && catalog->isEnabled();
    });
    if (!catalogDone)
      QWARN(qPrintable(QStringLiteral("catalog 未完成：notice=[%1] report=[%2] "
                                        "requests=%3 busy=%4")
                         .arg(notice->text(),
                              report->toPlainText().left(300))
                         .arg(server.requestCount())
                         .arg(catalog->isEnabled())));
    QVERIFY(catalogDone);
    QVERIFY2(report->toPlainText().contains(QStringLiteral("不支持")),
             qPrintable(QStringLiteral("report=[%1] requests=%2")
                          .arg(report->toPlainText())
                          .arg(server.requestCount())));
    QCOMPARE(dialog->findChild<QListWidget *>(QStringLiteral("llmDiagModelList"))
               ->count(),
             0);
    // 2) 未填模型时文本探测：本地拒绝（零网络）。
    text->click();
    QVERIFY(spinUntil([&] { return notice->isVisible(); }));
    QCOMPARE(server.requestCount(), 1);
    // 3) 手填模型 → 文本探测成功（目录缺席不影响文本能力）。
    dialog->findChild<QLineEdit *>(QStringLiteral("llmConfigModel"))
      ->setText(QStringLiteral("geo-7b"));
    text->click();
    QVERIFY(spinUntil([&] {
      return server.requestCount() == 2 && text->isEnabled()
             && report->toPlainText().contains(QStringLiteral("文本请求：通过"));
    }));
    // 诊断面零保存：探测后配置文件仍与打开时一致。
    QCOMPARE(configBytes(), beforeConfig);
    // 4) 原显式 apply：表单提交（沿用当前密钥），成功关窗。
    dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)
      ->click();
  });
  openAiConfiguration(&dock, &dock); // 模态直到 apply 成功关窗
  // 仅原 apply 生效：controller 配置 = 表单模型 + 沿用密钥；落盘模型已更新。
  QCOMPARE(controller.config().model, QStringLiteral("geo-7b"));
  QCOMPARE(controller.config().apiKey, QByteArray(kCurrentKey));
  QVERIFY(controller.enabled());
  QCOMPARE(LlmConfig::load().model, QStringLiteral("geo-7b"));
  QCOMPARE(server.requestCount(), 2); // 1 GET + 1 POST（零多余请求）
}

QTEST_MAIN(TestH6LlmConfiguration)
#include "tst_h6_llmconfiguration.moc"
