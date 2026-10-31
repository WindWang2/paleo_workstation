// #860 续：标题行页签优先——窄窗 + scale 2 下每个页签都完整可见；搜索框先收窄、
// 再收成图标钮（点击展开、失焦收回），右侧按钮折进「更多」；放宽后全部恢复。
#include <QtTest>
#include <QApplication>
#include <QLineEdit>
#include <QMenu>
#include <QHBoxLayout>
#include <QStyle>
#include <QToolButton>

#include <SARibbon.h>

#include "../src/ui/paleoribbon.h"
#include "../src/ui/ribbontitlefit.h"

struct HintTabBar : QTabBar
{
  using QTabBar::tabSizeHint;
};

class TestRibbonTitleFit : public QObject
{
    Q_OBJECT
  private:
    SARibbonMainWindow *m_win = nullptr;
    QLineEdit *m_edit = nullptr;
    QToolButton *m_proc = nullptr;

    SARibbonBar *bar() const { return m_win->ribbonBar(); }
    void setWidth(int w)
    {
      m_win->resize(w, 500);
      for (int i = 0; i < 3; ++i)
      {
        QApplication::processEvents();
        PaleoRibbon::refitTitleRow(bar());
      }
      QApplication::processEvents();
    }
    template <class T> T *named(const char *n) const { return bar()->findChild<T *>(QLatin1String(n)); }
    QString tabsProblem() const
    {
      SARibbonTabBar *tb = bar()->ribbonTabBar();
      QWidget *right = bar()->rightButtonGroup();
      const QRect tbr = tb->geometry();
      if (tbr.right() >= right->geometry().left())
        return QStringLiteral("tab bar %1 overlaps right group at %2").arg(tbr.right()).arg(right->x());
      for (int i = 0; i < tb->count(); ++i)
      {
        const QRect r = tb->tabRect(i);
        if (r.width() < static_cast<HintTabBar *>(static_cast<QTabBar *>(tb))->tabSizeHint(i).width() || r.right() > tb->width() || r.left() < 0)
          return QStringLiteral("tab %1 '%2' clipped: %3..%4 of %5").arg(i).arg(tb->tabText(i))
              .arg(r.left()).arg(r.right()).arg(tb->width());
      }
      return {};
    }

  private slots:
    void initTestCase()
    {
      QVERIFY2(qFuzzyCompare(qApp->devicePixelRatio(), 2.0), "test must run at scale 2");
      PaleoRibbon::prepareLibrary();
      m_win = new SARibbonMainWindow;
      SARibbonBar *b = bar();
      b->setRibbonStyle(SARibbonBar::RibbonStyleCompactThreeRow);
      for (const char *t : {"数据管理", "地层对比", "单井相分析", "地震相分析", "古地理编图"})
        b->addCategoryPage(QString::fromUtf8(t))->addPanel(QStringLiteral("组"))->addLargeAction(
            new QAction(QStringLiteral("命令"), b));
      if (auto *app = qobject_cast<QToolButton *>(b->applicationButton()))
        app->setText(QStringLiteral("文件"));
      const QIcon ic = m_win->style()->standardIcon(QStyle::SP_DialogSaveButton);
      for (int i = 0; i < 3; ++i)
        b->quickAccessBar()->addAction(new QAction(ic, QStringLiteral("快速%1").arg(i), b));
      SARibbonButtonGroupWidget *right = b->rightButtonGroup();
      auto *slot = new QWidget(right);
      slot->setObjectName(QStringLiteral("locatorSlot"));
      auto *lay = new QHBoxLayout(slot);
      lay->setContentsMargins(0, 0, 0, 0);
      m_edit = new QLineEdit(slot);
      m_edit->setPlaceholderText(QStringLiteral("搜索井位/层位  Ctrl+K"));
      lay->addWidget(m_edit);
      right->addWidget(slot);
      m_proc = new QToolButton(right);
      m_proc->setObjectName(QStringLiteral("processingButton"));
      m_proc->setText(QStringLiteral("处理算法"));
      m_proc->setIcon(ic);
      m_proc->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
      m_proc->setMenu(new QMenu(m_proc));
      m_proc->setPopupMode(QToolButton::InstantPopup);
      right->addWidget(m_proc);
      for (const char *n : {"布局", "帮助"})
      {
        auto *t = new QToolButton(right);
        t->setText(QString::fromUtf8(n));
        t->setIcon(ic);
        t->setToolButtonStyle(Qt::ToolButtonIconOnly);
        right->addWidget(t);
      }
      right->addAction(new QAction(ic, QStringLiteral("Web 服务"), right));
      PaleoRibbon::enableTitleRowFit(b);
      m_win->show();
      QVERIFY(QTest::qWaitForWindowExposed(m_win));
    }

    void wideKeepsEverything()
    {
      setWidth(1600);
      QVERIFY(m_edit->isVisible());
      QCOMPARE(m_proc->toolButtonStyle(), Qt::ToolButtonTextBesideIcon);
      QVERIFY(!named<QToolButton>("titleMoreButton")->isVisible());
      QVERIFY(!named<QToolButton>("titleSearchButton")->isVisible());
      QVERIFY2(tabsProblem().isEmpty(), qPrintable(tabsProblem()));
    }

    // 1280x720@2 ≈ 640 逻辑像素宽；再往下压到页签本身放得下的极限附近。
    void narrowKeepsEveryTabVisible_data()
    {
      QTest::addColumn<int>("width");
      for (int w : {1100, 900, 760, 700, 640})
        QTest::newRow(qPrintable(QString::number(w))) << w;
    }
    void narrowKeepsEveryTabVisible()
    {
      QFETCH(int, width);
      setWidth(width);
      QVERIFY2(tabsProblem().isEmpty(), qPrintable(QStringLiteral("w=%1: %2").arg(width).arg(tabsProblem())));
    }

    void foldsProgressively()
    {
      setWidth(640);
      QVERIFY2(!m_edit->isVisible(), "search box should collapse to an icon at 640 logical px");
      QVERIFY(named<QToolButton>("titleSearchButton")->isVisible());
    }

    void searchExpandsOnClickAndCollapsesOnBlur()
    {
      setWidth(640);
      auto *sb = named<QToolButton>("titleSearchButton");
      QVERIFY(sb && sb->isVisible());
      m_win->activateWindow();
      sb->click();
      QApplication::processEvents();
      QVERIFY(m_edit->isVisible());
      QTRY_VERIFY(m_edit->hasFocus());
      const QRect er(m_edit->mapTo(bar(), QPoint()), m_edit->size());
      QVERIFY2(er.width() >= m_edit->fontMetrics().averageCharWidth() * 8, "expanded search is too narrow");
      QVERIFY(er.right() <= bar()->width());
      QVERIFY(PaleoRibbon::revealTitleSearch(bar())); // Ctrl+K 路径：已展开也安全
      m_proc->setFocusPolicy(Qt::StrongFocus);
      if (m_proc->isVisible())
        m_proc->setFocus();
      else
        bar()->ribbonTabBar()->setFocus();
      QTRY_VERIFY(!m_edit->isVisible());
      QCOMPARE(m_edit->parentWidget()->objectName(), QStringLiteral("locatorSlot"));
    }

    void moreMenuHoldsFoldedButtons()
    {
      setWidth(640);
      auto *more = named<QToolButton>("titleMoreButton");
      QVERIFY(more);
      if (!more->isVisible())
        QSKIP("nothing folded at this width with this font");
      QMenu *menu = more->menu();
      QMetaObject::invokeMethod(menu, "aboutToShow");
      QVERIFY(!menu->actions().isEmpty());
      for (QAction *a : menu->actions())
        if (!a->isSeparator())
          QVERIFY2(!a->text().isEmpty(), "folded entry without a label");
    }

    void restoresWhenWideAgain()
    {
      setWidth(640);
      setWidth(1600);
      QVERIFY(m_edit->isVisible());
      QVERIFY(bar()->quickAccessBar()->isVisible());
      QCOMPARE(m_proc->toolButtonStyle(), Qt::ToolButtonTextBesideIcon);
      QVERIFY(m_proc->isVisible());
      QVERIFY(!named<QToolButton>("titleMoreButton")->isVisible());
    }

    // R7 #884：成品在 enableTitleRowFit 之后才往快速访问栏加带文字的保存/撤销/重做；
    // 仅靠事件循环（不手动 refit）也必须让页签不进横滚。
    void quickAccessGrowthAfterFitRefits()
    {
      setWidth(1600);
      auto *qab = bar()->quickAccessBar();
      QVERIFY(qab);
      qab->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
      const QIcon ic = QApplication::style()->standardIcon(QStyle::SP_DialogSaveButton);
      for (const char *n : {"保存工程", "撤销", "重做"})
        qab->addAction(new QAction(ic, QString::fromUtf8(n), qab));
      m_win->resize(864, m_win->height());
      QTest::qWait(50);
      QApplication::processEvents();
      QTest::qWait(50);
      QVERIFY2(tabsProblem().isEmpty(), qPrintable(tabsProblem()));
      // 还原，免得影响后续用例
      for (QAction *a : qab->actions())
        qab->removeAction(a);
      qab->setToolButtonStyle(Qt::ToolButtonIconOnly);
      setWidth(1600);
    }

    void cleanupTestCase() { delete m_win; }
};

int main(int argc, char **argv)
{
  qputenv("QT_SCALE_FACTOR", "2"); // 本用例专测 scale 2，忽略外部设置
  QApplication app(argc, argv);
  TestRibbonTitleFit t;
  return QTest::qExec(&t, argc, argv);
}
#include "tst_ribbontitlefit.moc"
