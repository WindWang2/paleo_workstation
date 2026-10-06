// 层：测试壳
#include <QtTest>
#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <functional>

#include "../src/services/pythonenv.h"
#include "../src/services/pythonrepl.h"
#include "../src/services/scriptrunner.h"
#include "../src/ui/paleotheme.h"
#include "../src/ui/python/pythonconsolepanel.h"
#include "../src/ui/python/pythonreplpanel.h"
#include "../src/workflow/pythonconsolecontroller.h"

// 方向68：Python 脚本面板（视图层）offscreen 验收——运行→输出分色呈现、
// argv 透传、历史、结果区词表命中/未命中、导入意图信号、REPL 面板往返、
// 解释器缺失的诚实禁用态。
namespace {
struct ConsoleFixture {
  ScriptRunnerService runner;
  PythonReplSession repl;
  PythonConsoleController controller{&runner, &repl};
  PythonConsolePanel panel{&controller};
  PythonReplPanel replPanel{&controller};
};
} // namespace

class TestPythonConsole : public QObject
{
  Q_OBJECT
private slots:
  void initTestCase();
  void runEchoesArgsAndRecordsHistory();
  void stderrRendersColoredAndExitCode();
  void stopCancelsRun();
  void protocolDrivesProgressAndResults();
  void unknownResultDisablesImport();
  void interpreterMissingShowsGuidance();
  void replRoundTripThroughPanel();

private:
  static bool spinUntil(const std::function<bool()> &done, int timeoutMs = 20000)
  {
    QElapsedTimer clock;
    clock.start();
    while (!done() && clock.elapsed() < timeoutMs)
      QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return done();
  }
  static QString fixture(const QString &name)
  {
    return QStringLiteral(SCRIPT_FIXTURE_DIR) + QLatin1Char('/') + name;
  }
  QString m_python;
};

void TestPythonConsole::initTestCase()
{
  PaleoTheme::pinRenderEnvironment();
  m_python = PythonEnvService::findBasePython();
  if (m_python.isEmpty())
    QSKIP("no base python on PATH");
}

void TestPythonConsole::runEchoesArgsAndRecordsHistory()
{
  ConsoleFixture f;
  auto *script = f.panel.findChild<QLineEdit *>(QStringLiteral("pythonScriptPath"));
  auto *args = f.panel.findChild<QLineEdit *>(QStringLiteral("pythonArgs"));
  auto *run = f.panel.findChild<QPushButton *>(QStringLiteral("pythonRun"));
  auto *output = f.panel.findChild<QTextBrowser *>(QStringLiteral("pythonOutput"));
  auto *history =
      f.panel.findChild<QListWidget *>(QStringLiteral("pythonHistoryList"));
  QVERIFY(script && args && run && output && history);
  QVERIFY(run->isEnabled()); // 有解释器 → 可用

  script->setText(fixture(QStringLiteral("echo_args.py")));
  args->setText(QStringLiteral("hello world"));
  run->click();
  QVERIFY2(spinUntil([&] { return history->count() == 1; }),
           qPrintable(output->toPlainText()));
  // argv 透传：夹具逐行回显。
  QVERIFY(output->toPlainText().contains(QStringLiteral("ARG:hello")));
  QVERIFY(output->toPlainText().contains(QStringLiteral("ARG:world")));
  // 历史行：成功状态 + 双击回填。
  QCOMPARE(f.controller.history().size(), 1);
  QCOMPARE(f.controller.history().first().exitCode, 0);
  QVERIFY(history->item(0)->text().contains(
      PythonConsoleController::recordStatusText(f.controller.history().first())));
  script->clear();
  args->clear();
  // 双击回填（itemActivated 手势的信号等效路径）。
  emit history->itemActivated(history->item(0));
  QCOMPARE(script->text(), fixture(QStringLiteral("echo_args.py")));
  QCOMPARE(args->text(), QStringLiteral("hello world"));
  QVERIFY(run->isEnabled()); // 运行结束回到可运行态
}

void TestPythonConsole::stderrRendersColoredAndExitCode()
{
  ConsoleFixture f;
  auto *script = f.panel.findChild<QLineEdit *>(QStringLiteral("pythonScriptPath"));
  auto *output = f.panel.findChild<QTextBrowser *>(QStringLiteral("pythonOutput"));
  auto *history =
      f.panel.findChild<QListWidget *>(QStringLiteral("pythonHistoryList"));
  script->setText(fixture(QStringLiteral("fail.py")));
  f.panel.findChild<QPushButton *>(QStringLiteral("pythonRun"))->click();
  QVERIFY2(spinUntil([&] { return history->count() == 1; }),
           qPrintable(output->toPlainText()));
  // stderr 分色：warning token 出现在 html 里；退出码如实呈现。
  const QString html = output->toHtml();
  QVERIFY2(html.contains(PaleoTheme::tokens().warning.name()),
           qPrintable(html));
  QVERIFY(output->toPlainText().contains(QStringLiteral("FAIL_STDERR")));
  QVERIFY(output->toPlainText().contains(QStringLiteral("退出码 3")));
}

void TestPythonConsole::stopCancelsRun()
{
  ConsoleFixture f;
  auto *script = f.panel.findChild<QLineEdit *>(QStringLiteral("pythonScriptPath"));
  auto *stop = f.panel.findChild<QPushButton *>(QStringLiteral("pythonStop"));
  auto *progress =
      f.panel.findChild<QProgressBar *>(QStringLiteral("pythonProgress"));
  auto *output = f.panel.findChild<QTextBrowser *>(QStringLiteral("pythonOutput"));
  auto *history =
      f.panel.findChild<QListWidget *>(QStringLiteral("pythonHistoryList"));
  script->setText(fixture(QStringLiteral("sleep.py")));
  f.panel.findChild<QLineEdit *>(QStringLiteral("pythonArgs"))
      ->setText(QStringLiteral("30"));
  f.panel.findChild<QPushButton *>(QStringLiteral("pythonRun"))->click();
  QVERIFY(spinUntil([&] { return f.controller.busy(); }));
  // R1 回归：直接启动路径的 runStarted 必须到达面板（进度条解除显式隐藏
  // + 系统行）。offscreen 下面板未 show()，用 isHidden() 查显式标志。
  QVERIFY2(!progress->isHidden(), "runStarted 未触达面板（进度条仍隐藏）");
  QVERIFY(output->toPlainText().contains(QStringLiteral("开始运行")));
  QVERIFY(stop->isEnabled());
  stop->click();
  QVERIFY2(spinUntil([&] { return history->count() == 1; }), "取消必须了结");
  QVERIFY(f.controller.history().first().cancelled);
  QVERIFY(!f.controller.busy());
}

void TestPythonConsole::protocolDrivesProgressAndResults()
{
  ConsoleFixture f;
  QTemporaryDir outDir;
  QVERIFY(outDir.isValid());
  auto *script = f.panel.findChild<QLineEdit *>(QStringLiteral("pythonScriptPath"));
  auto *workdir =
      f.panel.findChild<QLineEdit *>(QStringLiteral("pythonWorkdir"));
  auto *progress =
      f.panel.findChild<QProgressBar *>(QStringLiteral("pythonProgress"));
  auto *results =
      f.panel.findChild<QListWidget *>(QStringLiteral("pythonResultsList"));
  auto *importBtn =
      f.panel.findChild<QPushButton *>(QStringLiteral("pythonImportResult"));
  auto *output = f.panel.findChild<QTextBrowser *>(QStringLiteral("pythonOutput"));
  auto *history =
      f.panel.findChild<QListWidget *>(QStringLiteral("pythonHistoryList"));
  QVERIFY(script && workdir && progress && results && importBtn && output &&
          history);
  QSignalSpy importSpy(&f.panel, &PythonConsolePanel::importRequested);

  script->setText(fixture(QStringLiteral("protocol.py")));
  workdir->setText(outDir.path());
  f.panel.findChild<QPushButton *>(QStringLiteral("pythonRun"))->click();
  QVERIFY2(spinUntil([&] { return history->count() == 1; }),
           qPrintable(output->toPlainText()));
  // 协议 progress → 进度条顶格。
  QCOMPARE(progress->value(), 100);
  // 两条 result 进结果区；非协议行按纯文本呈现。
  QCOMPARE(results->count(), 2);
  QVERIFY(output->toPlainText().contains(QStringLiteral("PLAIN_NOT_PROTOCOL")));
  QVERIFY(QFileInfo(outDir.filePath(QStringLiteral("result.geojson"))).isFile());
  QVERIFY(QFileInfo(outDir.filePath(QStringLiteral("result.csv"))).isFile());
  // 词表命中：geojson 与 csv（方向68 词表扩位）都可导入——选中即启用。
  for (int row = 0; row < results->count(); ++row)
  {
    results->setCurrentRow(row);
    const QString path =
        results->item(row)->data(Qt::UserRole).toString();
    QVERIFY2(importBtn->isEnabled(), qPrintable(path));
    QVERIFY2(results->item(row)->toolTip().contains(QStringLiteral("词表命中")),
             qPrintable(results->item(row)->toolTip()));
  }
  // 导入意图：面板只发信号（组装根接 DataImportService），路径原样透传。
  results->setCurrentRow(0);
  importBtn->click();
  QCOMPARE(importSpy.count(), 1);
  QVERIFY(importSpy.takeFirst().at(0).toString().endsWith(
      QStringLiteral("result.csv")));
}

void TestPythonConsole::unknownResultDisablesImport()
{
  ConsoleFixture f;
  QTemporaryDir outDir;
  QVERIFY(outDir.isValid());
  auto *script = f.panel.findChild<QLineEdit *>(QStringLiteral("pythonScriptPath"));
  auto *results =
      f.panel.findChild<QListWidget *>(QStringLiteral("pythonResultsList"));
  auto *importBtn =
      f.panel.findChild<QPushButton *>(QStringLiteral("pythonImportResult"));
  auto *history =
      f.panel.findChild<QListWidget *>(QStringLiteral("pythonHistoryList"));
  script->setText(fixture(QStringLiteral("unknown_result.py")));
  f.panel.findChild<QLineEdit *>(QStringLiteral("pythonWorkdir"))
      ->setText(outDir.path());
  f.panel.findChild<QPushButton *>(QStringLiteral("pythonRun"))->click();
  QVERIFY2(spinUntil([&] { return history->count() == 1; }), "运行必须了结");
  QCOMPARE(results->count(), 1);
  // 诚实面：词表外类型不禁则已，禁就要写清原因。
  results->setCurrentRow(0);
  QVERIFY(!importBtn->isEnabled());
  QVERIFY(results->item(0)->toolTip().contains(QStringLiteral("未识别")));
}

void TestPythonConsole::interpreterMissingShowsGuidance()
{
  // 掐掉 PATH + PALEO_PYTHON 模拟无解释器环境（findBasePython 发现序全空）。
  const QByteArray savedPath = qgetenv("PATH");
  const QByteArray savedPaleo = qgetenv("PALEO_PYTHON");
  qputenv("PATH", "/nonexistent-paleo-dir");
  qputenv("PALEO_PYTHON", "/nonexistent-paleo-python");
  {
    ConsoleFixture f;
    QVERIFY(f.controller.interpreter().isEmpty());
    auto *run = f.panel.findChild<QPushButton *>(QStringLiteral("pythonRun"));
    auto *label =
        f.panel.findChild<QLabel *>(QStringLiteral("pythonInterpreterLabel"));
    auto *replStart =
        f.replPanel.findChild<QPushButton *>(QStringLiteral("pythonReplStart"));
    QVERIFY(!run->isEnabled()); // 禁用态而非空按钮
    QVERIFY(label->text().contains(QStringLiteral("PALEO_PYTHON")));
    QVERIFY(!replStart->isEnabled());
  }
  qputenv("PATH", savedPath);
  qputenv("PALEO_PYTHON", savedPaleo);
}

void TestPythonConsole::replRoundTripThroughPanel()
{
  ConsoleFixture f;
  auto *start =
      f.replPanel.findChild<QPushButton *>(QStringLiteral("pythonReplStart"));
  auto *stop = f.replPanel.findChild<QPushButton *>(QStringLiteral("pythonReplStop"));
  auto *input = f.replPanel.findChild<QLineEdit *>(QStringLiteral("pythonReplInput"));
  auto *output =
      f.replPanel.findChild<QPlainTextEdit *>(QStringLiteral("pythonReplOutput"));
  auto *status =
      f.replPanel.findChild<QLabel *>(QStringLiteral("pythonReplStatus"));
  QVERIFY(start && stop && input && output && status);
  QVERIFY(!input->isEnabled()); // 未启动 → 输入禁用
  start->click();
  // replRunning 在 start() 后即真（进程已挂接管）；输入启用要等 started
  // 信号链（QProcess::started 异步）走完——按输入态等。
  QVERIFY2(spinUntil([&] { return input->isEnabled(); }),
           "REPL 会话必须启动并解锁输入");
  QVERIFY(f.controller.replRunning());
  QVERIFY(status->text().contains(QStringLiteral("运行中")));
  input->setText(QStringLiteral("print(1+1)"));
  emit input->returnPressed();
  QVERIFY2(spinUntil([&] {
             return output->toPlainText().contains(QStringLiteral("2"));
           }),
           qPrintable(output->toPlainText()));
  // 本地回显 + 结果回显都在。
  QVERIFY(output->toPlainText().contains(QStringLiteral(">>> print(1+1)")));
  stop->click();
  QVERIFY2(spinUntil([&] { return !f.controller.replRunning(); }),
           "REPL 会话必须干净退出");
  QVERIFY(status->text().contains(QStringLiteral("未启动")));
}

QTEST_MAIN(TestPythonConsole)
#include "tst_pythonconsole.moc"
