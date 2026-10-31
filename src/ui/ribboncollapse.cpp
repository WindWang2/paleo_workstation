// 层：视图
#include "ribboncollapse.h"

#include "paleoribbon.h"
#include "paleotheme.h"

#include <SARibbon.h>

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QFrame>
#include <QGuiApplication>
#include <QPointer>
#include <QScreen>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <vector>

// 设计见 ribboncollapse.h。实现要点：
// - 各级宽度按组测一次缓存（组内 action 增删改 / 字体样式变化才重测），
//   缩放时只做纯算术选级，再把与当前不同的级别落到部件上——拖窗不抖。
// - 1/2 级只改 SARibbonPanelItem::rowProportion + 按钮类型/样式，命令按钮、
//   action、镜像（PaleoRibbon::mirror）都不动。
// - 3 级把真组从页签布局 takePanel 出来，放进以页签为父的 Qt::Popup 框；
//   原位插一个占位组（单颗大按钮：组图标 + 组名 ▾）。真组仍是页签的子孙，
//   findChild / 镜像 / 门控 / 键盘可达照旧。
namespace
{
  const char *kLevel = "paleoCollapseLevel";
  const char *kPriority = "paleoCollapsePriority";
  const char *kStub = "paleoCollapseStub";
  const char *kOrigRp = "paleoCollapseOrigRp";
  const char *kOrigStyle = "paleoCollapseOrigStyle";
  const char *kCollapser = "paleoCollapser";

  void setVisualLevel(SARibbonPanel *panel, int level)
  {
    bool changed = false;
    for (SARibbonPanelItem *item : panel->ribbonPanelItem())
    {
      auto *btn = item ? qobject_cast<SARibbonToolButton *>(item->widget()) : nullptr;
      if (!btn || item->customWidget)
        continue;
      if (!btn->property(kOrigRp).isValid())
      {
        btn->setProperty(kOrigRp, int(item->rowProportion));
        btn->setProperty(kOrigStyle, int(btn->toolButtonStyle()));
      }
      const auto origRp = SARibbonPanelItem::RowProportion(btn->property(kOrigRp).toInt());
      const auto origStyle = Qt::ToolButtonStyle(btn->property(kOrigStyle).toInt());
      const bool shrink = level >= PaleoRibbon::CollapseSmall &&
                          (origRp == SARibbonPanelItem::Large || origRp == SARibbonPanelItem::Medium);
      const auto rp = shrink ? SARibbonPanelItem::Small : origRp;
      const auto type = rp == SARibbonPanelItem::Large ? SARibbonToolButton::LargeButton
                                                       : SARibbonToolButton::SmallButton;
      const auto style = (level >= PaleoRibbon::CollapseIconOnly && !btn->icon().isNull())
                             ? Qt::ToolButtonIconOnly : origStyle;
      if (item->rowProportion != rp)
      {
        item->rowProportion = rp;
        changed = true;
      }
      if (btn->buttonType() != type)
      {
        btn->setButtonType(type);
        changed = true;
      }
      if (btn->toolButtonStyle() != style)
      {
        btn->setToolButtonStyle(style);
        changed = true;
      }
      if (btn->accessibleName().isEmpty() && btn->defaultAction())
        btn->setAccessibleName(btn->defaultAction()->text());
      if (changed)
        btn->invalidateSizeHint();
    }
    if (changed)
      if (SARibbonPanelLayout *lay = panel->panelLayout())
      {
        lay->invalidateButtonSizeHintCache();
        lay->invalidate();
        panel->updateGeometry();
      }
  }

  QIcon groupIcon(SARibbonPanel *panel)
  {
    for (QAction *a : panel->actions())
      if (!a->icon().isNull())
        return a->icon();
    return {};
  }

  class CategoryCollapser : public QObject
  {
    public:
      explicit CategoryCollapser(SARibbonCategory *cat) : QObject(cat), m_cat(cat)
      {
        cat->setProperty(kCollapser, QVariant::fromValue<QObject *>(this));
        cat->installEventFilter(this);
      }

      void apply()
      {
        if (m_applying || !m_cat)
          return;
        auto *lay = qobject_cast<SARibbonCategoryLayout *>(m_cat->layout());
        const int avail = m_cat->width() - 2; // 2px 余量：舍入不让 SARibbon 误判溢出
        if (!lay || avail <= 2)
          return;
        m_applying = true;
        refreshGroups(lay);
        for (auto &g : m_groups)
          if (!g.measured)
            measure(g);

        // 固定开销：页签边距 + 每组一条分隔线。
        const QMargins mg = lay->contentsMargins();
        int sepW = 0;
        if (auto *sep = m_cat->findChild<SARibbonSeparatorWidget *>(QString(), Qt::FindDirectChildrenOnly))
          sepW = sep->sizeHint().width();
        int total = mg.left() + mg.right();
        std::vector<int> level(m_groups.size(), PaleoRibbon::CollapseFull);
        for (const auto &g : m_groups)
          total += g.w[0] + sepW;

        // 折叠顺序：优先级小的先折；同优先级自右向左。
        std::vector<int> order(m_groups.size());
        for (size_t i = 0; i < order.size(); ++i)
          order[i] = int(i);
        std::stable_sort(order.begin(), order.end(), [this](int a, int b) {
          const int pa = m_groups[a].priority, pb = m_groups[b].priority;
          return pa != pb ? pa < pb : a > b;
        });
        for (int L = PaleoRibbon::CollapseSmall; L <= PaleoRibbon::CollapsePopup && total > avail; ++L)
          for (int i : order)
          {
            if (total <= avail)
              break;
            auto &g = m_groups[i];
            const int cur = level[i];
            if (g.w[L] >= g.w[cur])
              continue; // 这一级不省宽度（如只有小按钮的组的 1 级）——跳过，不白降
            total += g.w[L] - g.w[cur];
            level[i] = L;
          }

        bool changed = false;
        for (size_t i = 0; i < m_groups.size(); ++i)
        {
          changed |= m_groups[i].level != level[i];
          setLevel(m_groups[i], level[i]);
        }
        if (!changed)
        {
          // 级别没变就不重排：重排会把 SARibbon 的横向滚动（聚焦滚入）复位。
          m_applying = false;
          return;
        }
        lay->invalidate();
        // 同步落几何：stub 刚插入时停在 x=0，等下一轮事件才排开会短暂叠放。
        // 不论页签当前是否可见都同步落几何：隐藏页签里新插的 stub 否则全停在 x=0，
        // 直到该页再次 resize 才排开（tst_shell_chrome 叠放检查）。
        lay->setGeometry(m_cat->rect());
        relayoutPanels();
        m_applying = false;
        // 隐藏时插入的 stub 要等可见后再排一次；同步那次可能被 SARibbon 的重入守卫吞掉。
        QTimer::singleShot(0, this, [this] {
          if (!m_cat)
            return;
          if (auto *l = qobject_cast<SARibbonCategoryLayout *>(m_cat->layout()))
          {
            l->invalidate();
            l->setGeometry(m_cat->rect());
            relayoutPanels();
          }
          // 重排会复位横向滚动：焦点在本页按钮上就重新滚入（键盘用户不丢焦点件）。
          QWidget *f = QApplication::focusWidget();
          if (f && m_cat->isAncestorOf(f) && f->property("paleoKbdFocus").toBool())
            PaleoRibbon::scrollIntoView(f);
        });
        PaleoRibbon::makeKeyboardReachable(m_cat);
      }

      // 组尺寸没变时 SARibbon 不重排组内按钮（同 rect 跳过）——级别切换后按钮
      // 会停在旧坐标、跑出组外（tst_shell_chrome 聚焦滚入）。逐组强制落几何。
      void relayoutPanels()
      {
        for (auto &g : m_groups)
          for (SARibbonPanel *p : {g.panel.data(), g.stub.data()})
            if (p && p->isVisibleTo(m_cat))
              if (SARibbonPanelLayout *pl = p->panelLayout())
              {
                pl->invalidate();
                pl->setGeometry(p->rect());
              }
      }

      void schedule()
      {
        if (m_scheduled)
          return;
        m_scheduled = true;
        QTimer::singleShot(0, this, [this] {
          m_scheduled = false;
          apply();
        });
      }

      SARibbonPanel *stubFor(const SARibbonPanel *p) const
      {
        for (const auto &g : m_groups)
          if (g.panel == p)
            return g.level == PaleoRibbon::CollapsePopup ? g.stub.data() : nullptr;
        return nullptr;
      }

    protected:
      bool eventFilter(QObject *obj, QEvent *ev) override
      {
        if (m_applying)
          return false;
        switch (ev->type())
        {
          case QEvent::Resize:
          case QEvent::Show:
            if (obj == m_cat)
              apply();
            break;
          case QEvent::ChildAdded:
          case QEvent::ChildRemoved:
            if (obj == m_cat)
              schedule();
            break;
          case QEvent::ActionAdded:
          case QEvent::ActionRemoved:
          case QEvent::ActionChanged:
          case QEvent::FontChange:
          case QEvent::StyleChange:
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
          case QEvent::DevicePixelRatioChange:
#endif
            if (obj == m_cat)
              for (auto &g : m_groups)
                g.measured = false;
            else
              for (auto &g : m_groups)
                if (g.panel == obj)
                  g.measured = false;
            schedule();
            break;
          default:
            break;
        }
        return false;
      }

    private:
      struct Group
      {
        QPointer<SARibbonPanel> panel;
        QPointer<SARibbonPanel> stub;
        QPointer<QFrame> popup;
        int level = PaleoRibbon::CollapseFull;
        int priority = 0;
        std::array<int, 4> w{};
        bool measured = false;
      };

      void refreshGroups(SARibbonCategoryLayout *lay)
      {
        std::vector<Group> next;
        for (SARibbonPanel *p : lay->panelList())
        {
          if (p->property(kStub).toBool())
          {
            for (auto &g : m_groups)
              if (g.stub == p && g.panel)
                next.push_back(g);
            continue;
          }
          auto it = std::find_if(m_groups.begin(), m_groups.end(),
                                 [p](const Group &g) { return g.panel == p; });
          if (it != m_groups.end())
            next.push_back(*it);
          else
          {
            Group g;
            g.panel = p;
            p->installEventFilter(this);
            next.push_back(g);
          }
        }
        for (auto &g : next)
          g.priority = g.panel->property(kPriority).toInt();
        m_groups.swap(next);
      }

      void measure(Group &g)
      {
        const int keep = g.level == PaleoRibbon::CollapsePopup ? 0 : g.level;
        for (int L = 0; L <= PaleoRibbon::CollapseIconOnly; ++L)
        {
          setVisualLevel(g.panel, L);
          g.w[L] = g.panel->sizeHint().width();
          if (L > 0)
            g.w[L] = std::min(g.w[L], g.w[L - 1]);
        }
        setVisualLevel(g.panel, keep);
        ensureStub(g);
        g.w[3] = std::min(g.stub->sizeHint().width(), g.w[2]);
        g.measured = true;
      }

      void ensureStub(Group &g)
      {
        if (g.stub)
          return;
        const QString title = g.panel->panelName();
        auto *stub = new SARibbonPanel(title, m_cat);
        stub->setObjectName(g.panel->objectName() + QStringLiteral(".collapsed"));
        stub->setProperty(kStub, true);
        auto *act = new QAction(groupIcon(g.panel), title + QStringLiteral(" \u25BE"), stub);
        act->setObjectName(g.panel->objectName() + QStringLiteral(".collapsedButton"));
        const QString tip = QCoreApplication::translate("PaleoRibbon", "展开「%1」组的全部命令").arg(title);
        act->setToolTip(tip);
        act->setWhatsThis(tip);
        act->setWhatsThis(tip);
        stub->addLargeAction(act);
        if (auto *b = stub->actionToRibbonToolButton(act))
          b->setAccessibleName(title);
        QPointer<SARibbonPanel> real = g.panel;
        connect(act, &QAction::triggered, this, [this, real] { showPopup(real); });
        // 先过一遍 insert/take：页签把标题高、图标尺寸、换行等组设置写到占位组上，
        // 宽度才测得准（这些 setter 对外不可见）。
        m_cat->insertPanel(stub, m_cat->panelCount());
        m_cat->takePanel(stub);
        stub->hide();
        g.stub = stub;
      }

      void ensurePopup(Group &g)
      {
        if (g.popup)
          return;
        auto *popup = new QFrame(m_cat, Qt::Popup);
        popup->setObjectName(QStringLiteral("ribbonCollapsePopup"));
        popup->setFrameShape(QFrame::StyledPanel);
        popup->setAutoFillBackground(true);
        popup->setAccessibleName(g.panel->panelName());
        auto *v = new QVBoxLayout(popup);
        {
          const int m = PaleoTheme::tokens().spacingXs; // #876: DESIGN token, not raw px
          v->setContentsMargins(m, m, m, m);
        }
        // 命令执行完收起弹框（Office 行为）；输入类控件（下拉/数值框）不触发 actionTriggered，可连续操作。
        connect(g.panel, &SARibbonPanel::actionTriggered, popup, &QWidget::hide);
        g.popup = popup;
      }

      void showPopup(SARibbonPanel *real)
      {
        auto it = std::find_if(m_groups.begin(), m_groups.end(),
                               [real](const Group &g) { return g.panel == real; });
        if (it == m_groups.end() || !it->popup || !it->stub)
          return;
        QFrame *popup = it->popup;
        popup->adjustSize();
        QWidget *anchor = it->stub;
        QPoint pos = anchor->mapToGlobal(QPoint(0, anchor->height()));
        if (QScreen *s = anchor->screen())
        {
          const QRect avail = s->availableGeometry();
          pos.setX(std::clamp(pos.x(), avail.left(), std::max(avail.left(), avail.right() - popup->width())));
          if (pos.y() + popup->height() > avail.bottom())
            pos.setY(std::max(avail.top(), anchor->mapToGlobal(QPoint(0, 0)).y() - popup->height()));
        }
        popup->move(pos);
        popup->show();
        popup->raise();
        real->setFocus(Qt::PopupFocusReason);
        if (QWidget *first = real->nextInFocusChain(); first && real->isAncestorOf(first)) first->setFocus(Qt::TabFocusReason);
      }

      void setLevel(Group &g, int level)
      {
        if (!g.panel)
          return;
        const int old = g.level;
        if (old == level && g.panel->property(kLevel).isValid())
          return;
        if (level == PaleoRibbon::CollapsePopup && old != PaleoRibbon::CollapsePopup)
        {
          setVisualLevel(g.panel, PaleoRibbon::CollapseFull); // 弹框里是完整组
          ensureStub(g);
          ensurePopup(g);
          const int idx = m_cat->panelIndex(g.panel);
          m_cat->takePanel(g.panel);
          g.popup->layout()->addWidget(g.panel);
          g.panel->show();
          m_cat->insertPanel(g.stub, idx);
          g.stub->show();
        }
        else if (level != PaleoRibbon::CollapsePopup && old == PaleoRibbon::CollapsePopup)
        {
          g.popup->hide();
          const int idx = m_cat->panelIndex(g.stub);
          m_cat->takePanel(g.stub);
          g.stub->hide();
          g.popup->layout()->removeWidget(g.panel);
          m_cat->insertPanel(g.panel, idx);
        }
        if (level != PaleoRibbon::CollapsePopup)
          setVisualLevel(g.panel, level);
        g.level = level;
        g.panel->setProperty(kLevel, level);
      }

      QPointer<SARibbonCategory> m_cat;
      std::vector<Group> m_groups;
      bool m_applying = false;
      bool m_scheduled = false;
  };

  CategoryCollapser *collapserOf(const QObject *cat)
  {
    // 无 Q_OBJECT（匿名命名空间不跑 moc）：属性里只放 CategoryCollapser，静态转回。
    return cat ? static_cast<CategoryCollapser *>(cat->property(kCollapser).value<QObject *>()) : nullptr;
  }

  SARibbonCategory *categoryOf(const SARibbonPanel *p)
  {
    for (QWidget *w = p ? p->parentWidget() : nullptr; w; w = w->parentWidget())
      if (auto *c = qobject_cast<SARibbonCategory *>(w))
        return c;
    return nullptr;
  }

  // 新页签（含上下文页签）也接管：bar 收到 ChildAdded 后补一轮。
  class BarWatcher : public QObject
  {
    public:
      explicit BarWatcher(SARibbonBar *bar) : QObject(bar), m_bar(bar)
      {
        setObjectName(QStringLiteral("paleoRibbonCollapseWatcher"));
        bar->installEventFilter(this);
      }
      void attachAll()
      {
        if (!m_bar)
          return;
        for (SARibbonCategory *c : m_bar->findChildren<SARibbonCategory *>())
          if (!collapserOf(c))
            (new CategoryCollapser(c))->schedule();
      }
    protected:
      bool eventFilter(QObject *, QEvent *ev) override
      {
        if (ev->type() == QEvent::ChildAdded || ev->type() == QEvent::Show)
          QTimer::singleShot(0, this, [this] { attachAll(); });
        return false;
      }
    private:
      QPointer<SARibbonBar> m_bar;
  };
} // namespace

namespace PaleoRibbon
{
  void enableAdaptiveCollapse(SARibbonBar *bar)
  {
    // PALEO_RIBBON_COLLAPSE=0：关掉折叠（对照截图 / 排障），回到纯滚动箭头。
    if (!bar || qEnvironmentVariable("PALEO_RIBBON_COLLAPSE") == QLatin1String("0"))
      return;
    auto *w = static_cast<BarWatcher *>(bar->findChild<QObject *>(
        QStringLiteral("paleoRibbonCollapseWatcher"), Qt::FindDirectChildrenOnly));
    if (!w)
      w = new BarWatcher(bar);
    w->attachAll();
  }

  void relayoutCollapse(SARibbonCategory *category)
  {
    if (!category)
      return;
    if (!collapserOf(category))
      new CategoryCollapser(category);
    collapserOf(category)->apply();
  }

  int collapseLevel(const SARibbonPanel *panel)
  {
    return panel ? panel->property(kLevel).toInt() : CollapseFull;
  }

  SARibbonPanel *collapsedStub(const SARibbonPanel *panel)
  {
    auto *c = collapserOf(categoryOf(panel));
    return c ? c->stubFor(panel) : nullptr;
  }
} // namespace PaleoRibbon
