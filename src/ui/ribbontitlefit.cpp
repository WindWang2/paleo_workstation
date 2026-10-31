// 层：视图
// ribbontitlefit — 标题行页签优先（见 ribbontitlefit.h）。
#include "ribbontitlefit.h"

#include "paleoicons.h"

#include <SARibbon.h>

#include <QAction>
#include <QActionEvent>
#include <QApplication>
#include <QEvent>
#include <QLayout>
#include <QLineEdit>
#include <QMenu>
#include <QPointer>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QWidgetAction>

#include <vector>

namespace
{
  const char *kFitter = "paleoTitleRowFitter";

  class TitleRowFitter : public QObject
  {
    public:
      explicit TitleRowFitter(SARibbonBar *bar) : QObject(bar), m_bar(bar)
      {
        setObjectName(QLatin1String(kFitter));
        m_right = bar->rightButtonGroup();
        bar->installEventFilter(this);
        if (m_right)
          m_right->installEventFilter(this);
        if (QWidget *tabs = bar->ribbonTabBar())
          tabs->installEventFilter(this);
        connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *now) {
          if (m_expanded)
            QTimer::singleShot(0, this, [this, now = QPointer<QWidget>(now)] { maybeCollapse(now); });
        });
        schedule();
      }

      bool searchCollapsed() const { return m_searchCollapsed; }

      bool reveal()
      {
        if (!m_searchCollapsed || !m_search)
          return false;
        if (!m_expanded)
          expandSearch();
        return true;
      }

      void fit()
      {
        if (m_fitting || m_expanded || !m_bar || !m_right) // 展开中不重排，收回后再补一次
          return;
        m_fitting = true;
        ensureHelpers();
        restoreAll();
        const QFontMetrics fm(m_right->font());
        const int prefW = fm.averageCharWidth() * 24; // 「搜索井位/层位  Ctrl+K」放得下
        const int minW = fm.averageCharWidth() * 12;
        if (m_search)
          m_search->setFixedWidth(prefW);
        int deficit = groupWidth() - budget();
        // 1. 搜索框收窄
        if (deficit > 0 && m_search)
        {
          m_search->setFixedWidth(qMax(minW, prefW - deficit));
          deficit = groupWidth() - budget();
        }
        // 2. 文字按钮改纯图标
        if (deficit > 0)
        {
          for (QToolButton *b : textButtons())
          {
            m_iconified.push_back(b);
            b->setToolButtonStyle(Qt::ToolButtonIconOnly);
          }
          deficit = groupWidth() - budget();
        }
        // 3. 搜索框收成图标钮
        if (deficit > 0 && m_slotAction && m_search)
        {
          m_slotAction->setVisible(false);
          m_searchBtnAction->setVisible(true);
          m_searchCollapsed = true;
          deficit = groupWidth() - budget();
        }
        // 4. 右侧按钮自右向左折进「更多」
        if (deficit > 0)
        {
          const auto candidates = foldable();
          m_moreAction->setVisible(true);
          for (auto it = candidates.rbegin(); it != candidates.rend() && groupWidth() > budget(); ++it)
          {
            // addAction 进来的按钮以 action 为 defaultAction：action 一改可见性，按钮就从
            // action 回写 whatsThis/statusTip——先把按钮上已回填的说明落到 action 上。
            if (!qobject_cast<QWidgetAction *>(*it))
              if (auto *b = qobject_cast<QToolButton *>(m_right->widgetForAction(*it)))
              {
                if ((*it)->whatsThis().isEmpty() && !b->whatsThis().isEmpty())
                  (*it)->setWhatsThis(b->whatsThis());
                if ((*it)->statusTip().isEmpty() && !b->statusTip().isEmpty())
                  (*it)->setStatusTip(b->statusTip());
              }
            (*it)->setVisible(false);
            m_folded.insert(m_folded.begin(), *it);
          }
          // 5. 仍不够：快速访问栏也让位，其命令并入「更多」
          if (groupWidth() > budget())
            if (QWidget *qab = m_bar->quickAccessBar(); qab && qab->isVisibleTo(m_bar))
            {
              qab->setVisible(false);
              m_qabHidden = true;
            }
        }
        relayoutBar();
        m_fitting = false;
      }

      void schedule()
      {
        if (m_scheduled)
          return;
        m_scheduled = true;
        QTimer::singleShot(0, this, [this] {
          m_scheduled = false;
          fit();
        });
      }

    protected:
      bool eventFilter(QObject *obj, QEvent *ev) override
      {
        if (m_fitting)
          return false;
        switch (ev->type())
        {
          case QEvent::Resize:
            if (obj == m_bar)
              schedule();
            break;
          case QEvent::ActionAdded:
            if (obj == m_right)
            {
              auto *ae = static_cast<QActionEvent *>(ev);
              if (ae->action() != m_moreAction && m_moreAction)
                QTimer::singleShot(0, this, [this] { keepMoreLast(); });
              schedule();
            }
            break;
          case QEvent::FontChange:
          case QEvent::StyleChange:
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
          case QEvent::DevicePixelRatioChange:
#endif
            schedule();
            break;
          case QEvent::LayoutRequest: // 页签文字 / 增删页签改了页签行宽
            if (obj == m_bar->ribbonTabBar())
              schedule();
            break;
          default:
            break;
        }
        return false;
      }

    private:
      // 页签之外的标题行占用（应用按钮、快速访问栏、边距）——取当前几何，
      // 加一个字宽的舍入余量；剩下的才归右侧组。
      int budget() const
      {
        // 右侧组靠右锚定，其右端之外是窗口按钮等 bar 自留区域——直接以当前右端为界，
        // 左边减去页签起点、页签理想宽（同 SARibbonBar::calcMinTabBarWidth 口径）、
        // 夹在中间的快速访问栏，再留一个字宽舍入余量。
        SARibbonTabBar *tb = m_bar->ribbonTabBar();
        if (!tb)
          return m_bar->width();
        const int rightEnd = m_right->geometry().right() + 1;
        const QMargins tm = tb->tabMargin();
        int used = tb->x() + tb->sizeHint().width() + tm.left() + tm.right();
        if (QWidget *qab = m_bar->quickAccessBar(); qab && qab->isVisibleTo(m_bar) && qab->x() >= tb->x())
          used += qab->sizeHint().width();
        const int slack = QFontMetrics(m_bar->font()).averageCharWidth();
        return rightEnd - used - slack;
      }

      int groupWidth() const
      {
        if (QLayout *l = m_right->layout())
          l->invalidate();
        return m_right->sizeHint().width();
      }

      void relayoutBar()
      {
        if (QLayout *l = m_right->layout())
          l->activate();
        m_right->updateGeometry();
        m_right->adjustSize();
        if (QLayout *l = m_bar->layout())
        {
          l->invalidate();
          l->setGeometry(m_bar->rect());
        }
      }

      void ensureHelpers()
      {
        if (m_moreAction)
          return;
        QWidget *slot = m_right->findChild<QWidget *>(QStringLiteral("locatorSlot"));
        for (QAction *a : m_right->actions())
          if (slot && m_right->widgetForAction(a) == slot)
            m_slotAction = a;
        m_slot = slot;
        // 搜索按钮：放在搜索槽前面，收起时替换它。
        auto *sb = new QToolButton(m_right);
        sb->setObjectName(QStringLiteral("titleSearchButton"));
        sb->setIcon(PaleoIcons::qgisTheme(QStringLiteral("search.svg")));
        if (sb->icon().isNull())
          sb->setIcon(sb->style()->standardIcon(QStyle::SP_FileDialogContentsView));
        sb->setText(tr("搜索"));
        sb->setToolTip(tr("搜索井位 / 层位"));
        sb->setAccessibleName(tr("展开搜索"));
        sb->setToolButtonStyle(Qt::ToolButtonIconOnly);
        connect(sb, &QToolButton::clicked, this, [this] { expandSearch(); });
        m_searchBtnAction = m_slotAction ? m_right->insertWidget(m_slotAction, sb) : m_right->addWidget(sb);
        m_searchBtnAction->setVisible(false);
        // 「更多」：折叠掉的右侧按钮的菜单入口。
        auto *more = new QToolButton(m_right);
        more->setObjectName(QStringLiteral("titleMoreButton"));
        more->setText(tr("更多"));
        more->setToolTip(tr("更多命令"));
        more->setAccessibleName(tr("更多命令"));
        more->setArrowType(Qt::DownArrow);
        more->setToolButtonStyle(Qt::ToolButtonIconOnly);
        more->setPopupMode(QToolButton::InstantPopup);
        auto *menu = new QMenu(more);
        menu->setObjectName(QStringLiteral("titleMoreMenu"));
        connect(menu, &QMenu::aboutToShow, this, [this, menu] { fillMoreMenu(menu); });
        more->setMenu(menu);
        m_moreAction = m_right->addWidget(more);
        m_moreAction->setVisible(false);
      }

      void keepMoreLast()
      {
        if (!m_moreAction || m_right->actions().constLast() == m_moreAction)
          return;
        m_fitting = true;
        m_right->removeAction(m_moreAction);
        m_right->addAction(m_moreAction);
        m_fitting = false;
        schedule();
      }

      QWidget *searchWidget() const
      {
        if (!m_slot || !m_slot->layout())
          return m_search;
        for (int i = 0; i < m_slot->layout()->count(); ++i)
          if (QWidget *w = m_slot->layout()->itemAt(i)->widget())
            return w;
        return m_search;
      }

      std::vector<QToolButton *> textButtons() const
      {
        std::vector<QToolButton *> out;
        for (QAction *a : m_right->actions())
          if (a->isVisible())
            if (auto *b = qobject_cast<QToolButton *>(m_right->widgetForAction(a)))
              if (b->toolButtonStyle() != Qt::ToolButtonIconOnly && !b->icon().isNull())
                out.push_back(b);
        return out;
      }

      std::vector<QAction *> foldable() const
      {
        std::vector<QAction *> out;
        for (QAction *a : m_right->actions())
          if (a != m_slotAction && a != m_searchBtnAction && a != m_moreAction && a->isVisible() &&
              qobject_cast<QToolButton *>(m_right->widgetForAction(a)))
            out.push_back(a);
        return out;
      }

      void fillMoreMenu(QMenu *menu)
      {
        menu->clear();
        if (m_qabHidden)
          if (QWidget *qab = m_bar->quickAccessBar())
          {
            for (QAction *a : qab->actions())
              if (a->isVisible() && !a->isSeparator() && !qobject_cast<QWidgetAction *>(a))
                menu->addAction(a);
            menu->addSeparator();
          }
        for (QAction *a : m_folded)
        {
          auto *b = qobject_cast<QToolButton *>(m_right->widgetForAction(a));
          if (!b)
            continue;
          if (!qobject_cast<QWidgetAction *>(a))
          {
            menu->addAction(a); // addAction 进来的（如 dock 显隐）：原 action 直接复用
            continue;
          }
          QString text = b->text();
          if (text.isEmpty())
            text = b->accessibleName();
          QAction *proxy = menu->addAction(b->icon(), text);
          proxy->setToolTip(b->toolTip());
          if (b->menu())
            proxy->setMenu(b->menu());
          else
            connect(proxy, &QAction::triggered, b, [b = QPointer<QToolButton>(b)] { if (b) b->click(); });
        }
      }

      void restoreAll()
      {
        collapseSearch();
        if (m_qabHidden)
          if (QWidget *qab = m_bar->quickAccessBar())
            qab->setVisible(true);
        m_qabHidden = false;
        for (QAction *a : m_folded)
          if (a)
            a->setVisible(true);
        m_folded.clear();
        for (QToolButton *b : m_iconified)
          if (b)
            b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        m_iconified.clear();
        if (m_slotAction)
          m_slotAction->setVisible(true);
        if (m_searchBtnAction)
          m_searchBtnAction->setVisible(false);
        if (m_moreAction)
          m_moreAction->setVisible(false);
        m_searchCollapsed = false;
        m_search = searchWidget();
      }

      // 展开：搜索框临时挂到 bar 上，盖在页签行右端（同窗内，QGIS 结果浮层照常定位），
      // 失焦（且焦点不在结果列表里）即收回槽里。
      void expandSearch()
      {
        m_search = searchWidget();
        if (!m_search || m_expanded)
          return;
        QWidget *btn = m_right->widgetForAction(m_searchBtnAction);
        if (!btn)
          return;
        const QFontMetrics fm(m_right->font());
        const QRect br(btn->mapTo(m_bar, QPoint(0, 0)), btn->size());
        int left = br.right() - fm.averageCharWidth() * 24;
        if (QWidget *tb = m_bar->ribbonTabBar())
          left = qMax(left, tb->x());
        m_expanded = true;
        m_search->setParent(m_bar);
        m_search->setFixedWidth(br.right() - left);
        m_search->setGeometry(left, br.y(), br.right() - left, br.height());
        m_search->show();
        m_search->raise();
        if (auto *edit = m_search->findChild<QLineEdit *>())
          edit->setFocus(Qt::ShortcutFocusReason);
        else
          m_search->setFocus(Qt::ShortcutFocusReason);
      }

      static bool inResults(QWidget *w)
      {
        for (; w; w = w->parentWidget())
          if (w->inherits("QgsFloatingWidget") || w->inherits("QgsLocatorResultsView"))
            return true;
        return false;
      }

      void maybeCollapse(QWidget *now)
      {
        if (!m_expanded || !m_search)
          return;
        if (now && (now == m_search || m_search->isAncestorOf(now) || inResults(now)))
          return;
        collapseSearch();
      }

      void collapseSearch()
      {
        if (!m_expanded)
          return;
        m_expanded = false;
        if (m_search && m_slot && m_slot->layout())
        {
          m_search->setParent(m_slot);
          m_slot->layout()->addWidget(m_search);
          m_search->show();
        }
        schedule();
      }

      QPointer<SARibbonBar> m_bar;
      QPointer<SARibbonButtonGroupWidget> m_right;
      QPointer<QWidget> m_slot;
      QPointer<QWidget> m_search;
      QPointer<QAction> m_slotAction, m_searchBtnAction, m_moreAction;
      std::vector<QPointer<QAction>> m_folded;
      std::vector<QPointer<QToolButton>> m_iconified;
      bool m_fitting = false, m_scheduled = false, m_searchCollapsed = false, m_expanded = false, m_qabHidden = false;
  };

  TitleRowFitter *fitterOf(SARibbonBar *bar)
  {
    return bar ? dynamic_cast<TitleRowFitter *>(bar->findChild<QObject *>(QLatin1String(kFitter), Qt::FindDirectChildrenOnly)) : nullptr;
  }
} // namespace

namespace PaleoRibbon
{
  void enableTitleRowFit(SARibbonBar *bar)
  {
    if (!bar || fitterOf(bar) || qEnvironmentVariable("PALEO_RIBBON_COLLAPSE") == QLatin1String("0"))
      return;
    new TitleRowFitter(bar);
  }

  bool revealTitleSearch(SARibbonBar *bar)
  {
    TitleRowFitter *f = fitterOf(bar);
    return f && f->reveal();
  }

  void refitTitleRow(SARibbonBar *bar)
  {
    if (TitleRowFitter *f = fitterOf(bar))
      f->fit();
  }
} // namespace PaleoRibbon
