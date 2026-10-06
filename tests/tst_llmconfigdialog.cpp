// 层：测试壳
#include <QtTest>
#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QStandardPaths>

#include "../src/ai/chat/llmclient.h"
#include "../src/ui/ai/llmconfigdialog.h"
#include "../src/ui/paleotheme.h"

// 方向62：图形化配置对话框——视图契约（表单值/校验阻断/busy 态/密钥掩码/
// 无钥匙串禁用态）。落盘 round-trip 在 tst_aichatcontroller 的 applyConfig
// 用例（对话框不落盘——只发 applyRequested 意图）。
//
// 隔离：QStandardPaths 测试模式 + PALEO_LLM_NO_KEYCHAIN=1（两平台一致的
// 钥匙串缺席路径，不在测试里写真钥匙串）。
class TestLlmConfigDialog : public QObject {
  Q_OBJECT
private slots:
  void initTestCase();
  void cleanupTestCase();
  void validFormEmitsApplyRequestOnceAndGoesBusy();
  void invalidEndpointBlocksApply();
  void missingKeyStillAppliesButKeepsDisabledHint();
  void keyNeverEchoedInPlaintext();
  void keyAreaDisabledWithoutKeychain();
  void captureLedgerScreenshot();
};

void TestLlmConfigDialog::initTestCase() {
  PaleoTheme::pinRenderEnvironment(); // Fusion + 主题 palette 钉死（截图/断言稳定）
  QStandardPaths::setTestModeEnabled(true);
  qputenv("PALEO_LLM_NO_KEYCHAIN", "1"); // 两平台一致的钥匙串缺席路径
  qunsetenv("PALEO_LLM_ENDPOINT");
  qunsetenv("PALEO_LLM_MODEL");
  qunsetenv("PALEO_LLM_API_KEY");
  QFile::remove(LlmConfig::path()); // 沙箱文件从干净态开始
}

void TestLlmConfigDialog::cleanupTestCase() {
  qunsetenv("PALEO_LLM_NO_KEYCHAIN");
  QFile::remove(LlmConfig::path());
}

void TestLlmConfigDialog::validFormEmitsApplyRequestOnceAndGoesBusy() {
  LlmConfigDialog dialog{LlmConfig()};
  auto *endpoint =
    dialog.findChild<QLineEdit *>(QStringLiteral("llmConfigEndpoint"));
  auto *model =
    dialog.findChild<QLineEdit *>(QStringLiteral("llmConfigModel"));
  auto *maxTokens =
    dialog.findChild<QSpinBox *>(QStringLiteral("llmConfigMaxTokens"));
  auto *timeout =
    dialog.findChild<QSpinBox *>(QStringLiteral("llmConfigTimeout"));
  auto *stream =
    dialog.findChild<QCheckBox *>(QStringLiteral("llmConfigStream"));
  auto *buttons = dialog.findChild<QDialogButtonBox *>();
  QVERIFY(endpoint && model && maxTokens && timeout && stream && buttons);
  endpoint->setText(QStringLiteral("https://llm.example.com/v1"));
  model->setText(QStringLiteral("geo-model-7b"));
  maxTokens->setValue(2048);
  timeout->setValue(45000);
  stream->setChecked(false);
  auto *ok = buttons->button(QDialogButtonBox::Ok);
  QVERIFY(ok);

  QSignalSpy applied(&dialog, &LlmConfigDialog::applyRequested);
  ok->click();
  QCOMPARE(applied.size(), 1);
  const LlmConfig form = applied.at(0).at(0).value<LlmConfig>();
  QCOMPARE(form.endpoint.toString(), QStringLiteral("https://llm.example.com/v1"));
  QCOMPARE(form.model, QStringLiteral("geo-model-7b"));
  QCOMPARE(form.maxTokens, 2048);
  QCOMPARE(form.timeoutMs, 45000);
  QCOMPARE(form.stream, false);
  QCOMPARE(applied.at(0).at(1).toByteArray(), QByteArray()); // 未动密钥
  QCOMPARE(applied.at(0).at(2).toBool(), false);

  // busy 态：结果未回流前按钮禁用、重复 OK 不再发意图。
  QVERIFY(!ok->isEnabled());
  ok->click();
  QCOMPARE(applied.size(), 1);

  // 成功回流 → 关窗（Accepted）；失败回流 → 停留并可再试。
  dialog.onApplyResult(false, QStringLiteral("故意失败"));
  QVERIFY(ok->isEnabled());
  QCOMPARE(dialog.result(), 0); // 未接受
  ok->click();
  QCOMPARE(applied.size(), 2); // 失败后可重新提交
  dialog.onApplyResult(true, QString());
  QCOMPARE(dialog.result(), QDialog::Accepted);
}

void TestLlmConfigDialog::invalidEndpointBlocksApply() {
  QFile::remove(LlmConfig::path());
  LlmConfigDialog dialog{LlmConfig()};
  dialog.findChild<QLineEdit *>(QStringLiteral("llmConfigEndpoint"))
    ->setText(QStringLiteral("not-a-url"));
  dialog.findChild<QLineEdit *>(QStringLiteral("llmConfigModel"))
    ->setText(QStringLiteral("m"));
  QSignalSpy applied(&dialog, &LlmConfigDialog::applyRequested);
  dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)
    ->click();
  QCOMPARE(applied.size(), 0); // 结构非法：不发应用意图
  auto *note = dialog.findChild<QLabel *>(QStringLiteral("llmConfigNote"));
  QVERIFY(note && !note->text().isEmpty()); // 校验原因给到用户
}

void TestLlmConfigDialog::missingKeyStillAppliesButKeepsDisabledHint() {
  // 密钥缺失不阻断表单（助手保持禁用态由编排器/状态行如实呈现）；
  // 对话框只负责照发意图。
  LlmConfigDialog dialog{LlmConfig()};
  dialog.findChild<QLineEdit *>(QStringLiteral("llmConfigEndpoint"))
    ->setText(QStringLiteral("https://llm.example.com/v1"));
  dialog.findChild<QLineEdit *>(QStringLiteral("llmConfigModel"))
    ->setText(QStringLiteral("geo-model"));
  QSignalSpy applied(&dialog, &LlmConfigDialog::applyRequested);
  dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)
    ->click();
  QCOMPARE(applied.size(), 1);
}

void TestLlmConfigDialog::keyNeverEchoedInPlaintext() {
  LlmConfig current = LlmConfig::fromParts(
    QUrl(QStringLiteral("https://llm.example.com/v1")),
    QStringLiteral("geo-model"), QByteArrayLiteral("sk-super-secret-123"));
  LlmConfigDialog dialog(current);
  // 明文红线：整个对话框任何 label/输入框都不含密钥明文。
  const auto labels = dialog.findChildren<QLabel *>();
  QVERIFY(!labels.isEmpty());
  for (const QLabel *label : labels)
    QVERIFY2(!label->text().contains(QStringLiteral("sk-super-secret-123")),
             qPrintable(label->text()));
  const auto edits = dialog.findChildren<QLineEdit *>();
  for (const QLineEdit *edit : edits)
    QVERIFY2(edit->text() != QStringLiteral("sk-super-secret-123"),
             "密钥字段不得回填明文");
  // 状态行不出现密钥值（本文件全程 PALEO_LLM_NO_KEYCHAIN=1：状态文案是
  // 降级提示而非「已设置」——两个分支都不许带明文）。
  auto *state =
    dialog.findChild<QLabel *>(QStringLiteral("llmConfigKeyState"));
  QVERIFY(state);
  QVERIFY(!state->text().contains(QStringLiteral("sk-super-secret-123")));
}

void TestLlmConfigDialog::keyAreaDisabledWithoutKeychain() {
  LlmConfigDialog dialog{LlmConfig()};
  auto *keyArea =
    dialog.findChild<QWidget *>(QStringLiteral("llmConfigKeyArea"));
  auto *state =
    dialog.findChild<QLabel *>(QStringLiteral("llmConfigKeyState"));
  QVERIFY(keyArea && state);
  QVERIFY(!keyArea->isEnabled()); // 缺席如实禁用，不降级明文文件
  QVERIFY2(state->text().contains(QStringLiteral("PALEO_LLM_API_KEY")),
           qPrintable(state->text()));
}

// 方向62 ledger 截图：默认跳过；设 PALEO_AI_SHOT_DIR 落表单全貌 PNG。
void TestLlmConfigDialog::captureLedgerScreenshot() {
  const QString dir = QString::fromUtf8(qgetenv("PALEO_AI_SHOT_DIR"));
  if (dir.isEmpty())
    QSKIP("截图按需采集（PALEO_AI_SHOT_DIR 未设）");
  LlmConfig current = LlmConfig::fromParts(
    QUrl(QStringLiteral("https://llm.example.com/v1")),
    QStringLiteral("geo-model-7b"), QByteArrayLiteral("sk-masked-key"));
  LlmConfigDialog dialog(current);
  dialog.resize(460, 340);
  QVERIFY2(dialog.grab().save(dir + QStringLiteral("/ai-ux-llmconfig.png")),
           "配置对话框截图落盘失败");
}

QTEST_MAIN(TestLlmConfigDialog)
#include "tst_llmconfigdialog.moc"
