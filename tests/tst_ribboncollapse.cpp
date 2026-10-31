// #860：Office 式 ribbon 分组自适应折叠——缩放 ribbon，断言各组折叠级别、
// 折叠顺序、无溢出（滚动箭头只在全部组都收成下拉后才出现）、下拉弹框与恢复。
#include <QtTest>
#include <QApplication>
#include <QComboBox>
#include <QFrame>
#include <QSignalSpy>
#include <QStyle>
#include <QToolButton>

#include <SARibbon.h>

#include "../src/ui/paleoribbon.h"
#include "../src/ui/ribboncollapse.h"

namespace
{
  bool scrollArrowShown(SARibbonCategory *cat)
  {
    for (auto *tb : cat->findChildren<QToolButton *>())
      if (tb->inherits("SARibbonCategoryScrollButton") && tb->isVisibleTo(cat))
        return true;
    return false;
  }
} // namespace

class TestRibbonCollapse : public QObject
{
    Q_OBJECT
  private:
    SARibbonMainWindow *m_win = nullptr;
    SARibbonCategory *m_cat = nullptr;
    QList<SARibbonPanel *> m_panels;
    QAction *m_probe = nullptr;

    void setWidth(int w)
    {
      m_win->resize(w, 600);
      QApplication::processEvents();
      QApplication::processEvents();
      PaleoRibbon::relayoutCollapse(m_cat);
      QApplication::processEvents();
    }
    QList<int> levels() const
    {
      QList<int> l;
      for (auto *p : m_panels)
        l << PaleoRibbon::collapseLevel(p);
      return l;
    }

  private slots:
    void initTestCase()
    {
      PaleoRibbon::prepareLibrary();
      m_win = new SARibbonMainWindow;
      SARibbonBar *bar = m_win->ribbonBar();
      bar->setRibbonStyle(SARibbonBar::RibbonStyleCompactThreeRow);
      bar->setEnableWordWrap(true);
      m_cat = bar->addCategoryPage(QStringLiteral("测试页签"));
      const QIcon ic = m_win->style()->standardIcon(QStyle::SP_DialogOpenButton);
      for (int g = 0; g < 5; ++g)
      {
        auto *p = m_cat->addPanel(QStringLiteral("分组%1").arg(g));
        p->setObjectName(QStringLiteral("panel%1").arg(g));
        for (int i = 0; i < 3; ++i)
        {
          auto *a = new QAction(ic, QStringLiteral("大命令按钮%1-%2").arg(g).arg(i), p);
          p->addLargeAction(a);
          if (g == 4 && i == 0)
            m_probe = a;
        }
        for (int i = 0; i < 6; ++i)
          p->addSmallAction(new QAction(ic, QStringLiteral("小命令%1-%2").arg(g).arg(i), p));
        if (g == 2)
        {
          auto *combo = new QComboBox;
          combo->setObjectName(QStringLiteral("probeCombo"));
          combo->addItems({QStringLiteral("选项一"), QStringLiteral("选项二")});
          p->addSmallWidget(combo);
        }
        m_panels << p;
      }
      PaleoRibbon::enableAdaptiveCollapse(bar);
      m_win->resize(2600, 600);
      m_win->show();
      QVERIFY(QTest::qWaitForWindowExposed(m_win));
    }

    void cleanupTestCase() { delete m_win; }

    // 诊断（不判失败）：QGIS 主题图标资源是否可用。
    void diagThemeIconResource()
    {
      qInfo() << "qgis theme resource present:"
              << QFile::exists(QStringLiteral(":/images/themes/default/mActionSelectRectangle.svg"));
    }

    void wideIsFull()
    {
      setWidth(2600);
      QCOMPARE(levels(), QList<int>({0, 0, 0, 0, 0}));
      QVERIFY(!scrollArrowShown(m_cat));
    }

    // 逐步收窄：级别单调不减、右侧先折、未全部下拉前不出滚动箭头。
    void progressiveCollapse()
    {
      int prevSum = 0;
      bool sawSmall = false, sawIcon = false, sawPopup = false;
      for (int w = 2600; w >= 260; w -= 20)
      {
        setWidth(w);
        const QList<int> l = levels();
        int sum = 0;
        for (int v : l)
          sum += v;
        QVERIFY2(sum >= prevSum, qPrintable(QStringLiteral("w=%1 levels not monotonic").arg(w)));
        prevSum = sum;
        for (int i = 0; i + 1 < l.size(); ++i)
          QVERIFY2(l[i] <= l[i + 1], qPrintable(QStringLiteral("w=%1 left group collapsed before right").arg(w)));
        sawSmall |= l.contains(PaleoRibbon::CollapseSmall);
        sawIcon |= l.contains(PaleoRibbon::CollapseIconOnly);
        sawPopup |= l.contains(PaleoRibbon::CollapsePopup);
        const bool allPopup = l.count(PaleoRibbon::CollapsePopup) == l.size();
        if (!allPopup)
          QVERIFY2(!scrollArrowShown(m_cat), qPrintable(QStringLiteral("w=%1 overflow before all collapsed").arg(w)));
      }
      QVERIFY(sawSmall);
      QVERIFY(sawIcon);
      QVERIFY(sawPopup);
    }

    void smallLevelConvertsLargeButtons()
    {
      for (int w = 2600; w >= 260; w -= 10)
      {
        setWidth(w);
        if (PaleoRibbon::collapseLevel(m_panels[4]) == PaleoRibbon::CollapseSmall)
          break;
      }
      QCOMPARE(PaleoRibbon::collapseLevel(m_panels[4]), int(PaleoRibbon::CollapseSmall));
      SARibbonToolButton *b = m_panels[4]->actionToRibbonToolButton(m_probe);
      QVERIFY(b);
      QVERIFY(b->isSmallRibbonButton());
      QVERIFY(b->toolButtonStyle() != Qt::ToolButtonIconOnly);
    }

    void popupShowsFullGroup()
    {
      for (int w = 2600; w >= 260; w -= 10)
      {
        setWidth(w);
        if (PaleoRibbon::collapseLevel(m_panels[4]) == PaleoRibbon::CollapsePopup)
          break;
      }
      SARibbonPanel *real = m_panels[4];
      QCOMPARE(PaleoRibbon::collapseLevel(real), int(PaleoRibbon::CollapsePopup));
      SARibbonPanel *stub = PaleoRibbon::collapsedStub(real);
      QVERIFY(stub);
      QVERIFY(m_cat->panelList().contains(stub));
      QVERIFY(!m_cat->panelList().contains(real));
      QCOMPARE(m_cat->panelIndex(stub), 4);
      QVERIFY(stub->isVisible());
      const auto stubButtons = stub->ribbonToolButtons();
      QCOMPARE(stubButtons.size(), 1);
      QVERIFY(stubButtons.first()->text().contains(real->panelName()));
      QVERIFY(!stubButtons.first()->icon().isNull());

      stubButtons.first()->click();
      QApplication::processEvents();
      QWidget *popup = real->window();
      QVERIFY(popup != m_win);
      QVERIFY(popup->isVisible());
      // 弹框里是完整组：大按钮仍是大按钮
      SARibbonToolButton *b = real->actionToRibbonToolButton(m_probe);
      QVERIFY(b && b->isLargeRibbonButton() && b->isVisible());
      QSignalSpy spy(m_probe, &QAction::triggered);
      b->click();
      QApplication::processEvents();
      QCOMPARE(spy.count(), 1);
      QVERIFY(!popup->isVisible()); // 执行命令后收起
    }

    void comboSurvivesPopup()
    {
      setWidth(260);
      QCOMPARE(PaleoRibbon::collapseLevel(m_panels[2]), int(PaleoRibbon::CollapsePopup));
      auto *combo = m_win->findChild<QComboBox *>(QStringLiteral("probeCombo"));
      QVERIFY(combo);
      QCOMPARE(combo->parentWidget(), m_panels[2]);
    }

    void expandsBack()
    {
      setWidth(260);
      setWidth(2600);
      QCOMPARE(levels(), QList<int>({0, 0, 0, 0, 0}));
      QCOMPARE(m_cat->panelList(), m_panels);
      for (auto *p : m_panels)
        QCOMPARE(p->window(), static_cast<QWidget *>(m_win));
      SARibbonToolButton *b = m_panels[4]->actionToRibbonToolButton(m_probe);
      QVERIFY(b->isLargeRibbonButton());
      QVERIFY(b->toolButtonStyle() != Qt::ToolButtonIconOnly);
      QVERIFY(!scrollArrowShown(m_cat));
    }

    void priorityOverridesPosition()
    {
      m_panels[4]->setProperty("paleoCollapsePriority", 10);
      m_panels[0]->setProperty("paleoCollapsePriority", -10);
      for (int w = 2600; w >= 260; w -= 10)
      {
        setWidth(w);
        if (PaleoRibbon::collapseLevel(m_panels[0]) > 0 || PaleoRibbon::collapseLevel(m_panels[4]) > 0)
          break;
      }
      QVERIFY(PaleoRibbon::collapseLevel(m_panels[0]) > 0);
      QCOMPARE(PaleoRibbon::collapseLevel(m_panels[4]), 0);
      m_panels[4]->setProperty("paleoCollapsePriority", QVariant());
      m_panels[0]->setProperty("paleoCollapsePriority", QVariant());
    }

    // 组内动作增加（插件/镜像动态加按钮）会重测，不残留旧宽度导致溢出。
    void textChangeRemeasures()
    {
      setWidth(2600);
      int w = 2600;
      for (; w >= 260; w -= 10)
      {
        setWidth(w);
        if (PaleoRibbon::collapseLevel(m_panels[4]) > 0)
          break;
      }
      setWidth(w + 30); // 刚好放下全部
      const int before = m_panels[0]->sizeHint().width();
      QList<QAction *> extra;
      for (int i = 0; i < 3; ++i)
      {
        extra << new QAction(m_probe->icon(), QStringLiteral("新增命令%1").arg(i), m_panels[0]);
        m_panels[0]->addLargeAction(extra.last());
      }
      QApplication::processEvents();
      QApplication::processEvents();
      QVERIFY(!scrollArrowShown(m_cat));
      const QList<int> l = levels();
      QVERIFY2(l[4] > 0 || l[3] > 0, qPrintable(QStringLiteral("w=%1 levels=%2,%3,%4,%5,%6 p0w=%7 catw=%8")
                                                    .arg(w).arg(l[0]).arg(l[1]).arg(l[2]).arg(l[3]).arg(l[4])
                                                    .arg(m_panels[0]->sizeHint().width()).arg(m_cat->width())));
      QVERIFY(PaleoRibbon::collapseLevel(m_panels[0]) == 0 && m_panels[0]->sizeHint().width() > before);
      qDeleteAll(extra);
      QApplication::processEvents();
      QApplication::processEvents();
      QCOMPARE(levels(), QList<int>({0, 0, 0, 0, 0}));
    }
};

QTEST_MAIN(TestRibbonCollapse)
#include "tst_ribboncollapse.moc"
