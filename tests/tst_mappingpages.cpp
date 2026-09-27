#include <QtTest>
#include <QApplication>
#include <QComboBox>
#include <QListWidget>
#include <QTableWidget>

#include "../src/ui/pages/pagepanels.h"

// m2/mapping-pages — 三个编图页（预测/单因素/智能编图）的贯通测试。
// 约定与 tst_panels 相同：面板只发意图信号，测试用 nullptr/null 服务栈 +
// QSignalSpy + objectName 子件查找（plain QApplication，无 QgisRuntime）。
// 文件内三个测试类共用一个 main（多类 qExec）；每页一个类，各任务段
// 只改自己的类：
//   PredictPageTests   —— 预测编图页（任务 A）
//   FactorPageTests    —— 单因素图页（任务 B；ConstraintPage 重定位）
//   ComposePageTests   —— 智能编图页（任务 C）

class PredictPageTests : public QObject
{
  Q_OBJECT
  private slots:
    void constructsWithNullServices(); // 基座冒烟：拆分后类仍可构造
};

void PredictPageTests::constructsWithNullServices()
{
  PredictPage page(nullptr, nullptr);
  QVERIFY(page.findChild<QComboBox *>(QStringLiteral("horizonCombo")));
  QVERIFY(page.findChild<QComboBox *>(QStringLiteral("algoCombo")));
}

class FactorPageTests : public QObject
{
  Q_OBJECT
  private slots:
    void constructsWithNullServices(); // 基座冒烟：拆分后类仍可构造
};

void FactorPageTests::constructsWithNullServices()
{
  ConstraintPage page(nullptr);
  QVERIFY(page.findChild<QTableWidget *>(QStringLiteral("thicknessTable")));
}

class ComposePageTests : public QObject
{
  Q_OBJECT
  private slots:
    void constructsWithNullServices(); // 基座冒烟：拆分后类仍可构造
};

void ComposePageTests::constructsWithNullServices()
{
  ComposePage page(nullptr, nullptr);
  QVERIFY(page.findChild<QListWidget *>(QStringLiteral("factorList")));
}

// 多测试类单可执行体：QTEST_MAIN 只支持单类，这里手动跑三个类。
int main(int argc, char *argv[])
{
  QApplication app(argc, argv);
  int status = 0;
  {
    PredictPageTests t;
    status |= QTest::qExec(&t, argc, argv);
  }
  {
    FactorPageTests t;
    status |= QTest::qExec(&t, argc, argv);
  }
  {
    ComposePageTests t;
    status |= QTest::qExec(&t, argc, argv);
  }
  return status;
}

#include "tst_mappingpages.moc"
