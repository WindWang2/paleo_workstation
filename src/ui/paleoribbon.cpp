// 层：视图
#include "paleoribbon.h"

#include "paleotheme.h"

#include <SARibbon.h>

#include <QAbstractButton>
#include <QAction>
#include <QCoreApplication>
#include <QEvent>
#include <QPointer>
#include <QStyle>
#include <QTimer>
#include <QToolButton>

// qrc 在静态库 paleo_saribbon 里（amalgamated SARibbon.cpp 内联）——显式
// 初始化，模板/调色板资源路径 ":/SARibbonTheme/..." 才一定可达。
static void initSARibbonResource() { Q_INIT_RESOURCE(SARibbonResource); }

namespace
{
  // 事件过滤器挂在面板按钮上：EnabledChange / ToolTipChange 时整体重同步。
  // setText 没有对应事件——动态文案的按钮在改文案的同一处都会改 tooltip
  // （setThicknessHorizon / setVersionState），ToolTipChange 顺带把文案带过来。
  class ButtonMirror : public QObject
  {
    public:
      ButtonMirror(QAction *action, QAbstractButton *button, bool syncText)
        : QObject(action), m_action(action), m_button(button), m_buttonId(button),
          m_syncText(syncText), m_baseTip(action->toolTip())
      {
        setObjectName(QStringLiteral("paleoButtonMirror"));
        button->installEventFilter(this);
        QObject::connect(action, &QAction::triggered, this, [this] {
          if (m_button && m_button->isEnabled())
            m_button->click();
        });
        if (button->isCheckable())
          QObject::connect(button, &QAbstractButton::toggled, action, &QAction::setChecked);
        sync();
      }

      void resync() { sync(); } // whatsThis 变化无事件，由 syncMirrors 显式调

    protected:
      bool eventFilter(QObject *obj, QEvent *ev) override
      {
        // 先看事件类型、再用裸身份指针比对：obj == QPointer<QAbstractButton> 会经
        // QPointer::data() 把 obj 下转成 QAbstractButton——按钮析构途中（已退化为
        // QWidget/QObject）收到的事件会触发 UBSan downcast（审计 L6）。
        if ((ev->type() == QEvent::EnabledChange || ev->type() == QEvent::ToolTipChange) &&
            obj == m_buttonId)
          sync();
        return QObject::eventFilter(obj, ev);
      }

    private:
      void sync()
      {
        if (!m_button || !m_action)
          return;
        // 门控原因挂在 action 上（见 applyGate）：置灰但保留源按钮状态，
        // 开门后经 resync 还原；tooltip 让位给原因，空了回源 tooltip。
        const QString gate = m_action->property("paleoGateReason").toString();
        m_action->setEnabled(m_button->isEnabled() && gate.isEmpty());
        if (m_syncText && !m_button->text().isEmpty())
          m_action->setText(m_button->text());
        const QString tip = m_button->toolTip();
        m_action->setToolTip(!gate.isEmpty() ? gate : (tip.isEmpty() ? m_baseTip : tip));
        // 帮助文本：源 whatsThis 优先，空则退到 accessibleDescription
        //（数据页导入按钮把用途写在后者）；只在非空时写，绝不清空。
        QString help = m_button->whatsThis();
        if (help.isEmpty())
          help = m_button->accessibleDescription();
        if (!help.isEmpty())
          m_action->setWhatsThis(help);
        if (m_button->isCheckable())
        {
          m_action->setCheckable(true);
          m_action->setChecked(m_button->isChecked());
        }
      }

      QPointer<QAction> m_action;
      QPointer<QAbstractButton> m_button;
      const QObject *m_buttonId; // 仅作身份比对，从不解引用
      bool m_syncText;
      QString m_baseTip;
  };

  // ShowToParent/HideToParent 只看显式 show()/hide()——父链没上屏时同样送达。
  class VisibilityFollower : public QObject
  {
    public:
      VisibilityFollower(QAction *action, QWidget *widget)
        : QObject(action), m_action(action), m_widgetId(widget)
      {
        widget->installEventFilter(this);
        action->setChecked(!widget->isHidden());
      }

    protected:
      bool eventFilter(QObject *obj, QEvent *ev) override
      {
        if ((ev->type() == QEvent::ShowToParent || ev->type() == QEvent::HideToParent) &&
            obj == m_widgetId && m_action)
          m_action->setChecked(ev->type() == QEvent::ShowToParent);
        return QObject::eventFilter(obj, ev);
      }

    private:
      QPointer<QAction> m_action;
      const QObject *m_widgetId; // 仅作身份比对（同 ButtonMirror：不经 QPointer 下转）
  };

  // 聚焦被裁掉的命令按钮时把类别滚过去——SARibbon 的类别滚动是布局平移，
  // QWidget 的 scrollIntoView 语义不存在，不滚焦点就落在看不见的按钮上。
  // 一个过滤器挂全部按钮（makeKeyboardReachable 按 root 惰性建一份）。
  class FocusScrollFilter : public QObject
  {
    public:
      using QObject::QObject;
    protected:
      bool eventFilter(QObject *obj, QEvent *ev) override
      {
        if (ev->type() != QEvent::FocusIn)
          return false;
        // 聚焦瞬间的几何可能还是页签切换前的旧值（LayoutRequest 排队未跑）——
        // 推到下一轮事件循环再量，量到什么就滚什么，一次直滚到位。
        auto *btn = qobject_cast<QWidget *>(obj);
        QTimer::singleShot(0, this, [btn = QPointer<QWidget>(btn)] {
          if (!btn)
            return;
          SARibbonCategory *cat = nullptr;
          for (QWidget *p = btn->parentWidget(); p && !cat; p = p->parentWidget())
            cat = qobject_cast<SARibbonCategory *>(p);
          // categoryLayout() 是 protected——同一对象经公有 layout() 也能拿到
          //（vendor 内部也用 qobject_cast 这条等价路径）。
          auto *lay = cat ? qobject_cast<SARibbonCategoryLayout *>(cat->layout()) : nullptr;
          if (!lay)
            return;
          const QRect r(btn->mapTo(cat, QPoint(0, 0)), btn->size());
          // 可见的滚动钮压住类别边缘，预留它的宽度免得焦点件藏在按钮底下。
          int marginL = 0, marginR = 0;
          for (auto *tb : cat->findChildren<QToolButton *>())
            if (tb->inherits("SARibbonCategoryScrollButton") && tb->isVisibleTo(cat))
              (tb->arrowType() == Qt::LeftArrow ? marginL : marginR) = tb->width();
          // LTR 的 mXBase ∈ [类别宽-总宽, 0]：负 px 才把内容左移、露出右裁件。
          if (r.right() > cat->width())
            lay->scroll(-(r.right() - cat->width() + marginR));
          else if (r.left() < 0)
            lay->scroll(-r.left() + marginL);
        });
        return false;
      }
  };
} // namespace

namespace PaleoRibbon
{
  void prepareLibrary()
  {
    static const bool once = [] {
      initSARibbonResource();
      // 暗色翻案（DESIGN.md 决策日志 2026-09-28）：主题仍只由 PaleoTheme
      // 显式驱动——关掉 SARibbon 跟随系统暗色的自动换肤，防双写/互踩。
      SA::setEnableSystemDarkModeAutoSwitch(false);
      return true;
    }();
    Q_UNUSED(once);
  }

  void applyTheme(SARibbonMainWindow *win, const QString &shellQss)
  {
    if (!win)
      return;
    SA::SARibbonThemePalette palette;
    palette.loadFromJson(PaleoTheme::ribbonPaletteJson());
    SA::applyRibbonTheme(win, win->ribbonBar(), SARibbonTheme::RibbonThemeOffice2021Blue,
                         palette);
    // applyRibbonTheme has just replaced the stylesheet with a fresh template.
    // Keep its flat buttons/separators; append only our token-level overrides.
    // Repeated theme changes start from that fresh template, never accumulate.
    win->setStyleSheet(win->styleSheet() + QLatin1Char('\n') +
                       PaleoTheme::ribbonStyleSheet() + QLatin1Char('\n') +
                       shellQss);
  }

  void nameScrollButtons(QWidget *root)
  {
    if (!root)
      return;
    for (auto *b : root->findChildren<QToolButton *>())
      if (b->inherits("SARibbonCategoryScrollButton") && b->accessibleName().isEmpty())
      {
        // 两个字面量各自进 translate：lupdate 只认字面量实参，不认三目。
        const QString t = b->arrowType() == Qt::LeftArrow
            ? QCoreApplication::translate("PaleoRibbon", "向左滚动工具栏")
            : QCoreApplication::translate("PaleoRibbon", "向右滚动工具栏");
        b->setAccessibleName(t);
        b->setToolTip(t);
      }
  }

  void syncMirrors(QWidget *root)
  {
    if (!root)
      return;
    for (auto *a : root->findChildren<QAction *>())
      // ButtonMirror 不带 Q_OBJECT，findChild 只能到 QObject*——名字即类型契约。
      if (auto *m = a->findChild<QObject *>(QStringLiteral("paleoButtonMirror")))
        static_cast<ButtonMirror *>(m)->resync();
  }

  void applyGate(QAction *action, const QString &reason)
  {
    if (!action)
      return;
    action->setProperty("paleoGateReason", reason);
    if (auto *m = action->findChild<QObject *>(QStringLiteral("paleoButtonMirror")))
      static_cast<ButtonMirror *>(m)->resync();
  }

  void makeKeyboardReachable(QWidget *root)
  {
    if (!root)
      return;
    auto *filter = root->findChild<QObject *>(QStringLiteral("paleoFocusScrollFilter"));
    if (!filter)
    {
      filter = new FocusScrollFilter(root);
      filter->setObjectName(QStringLiteral("paleoFocusScrollFilter"));
    }
    for (auto *b : root->findChildren<SARibbonToolButton *>())
    {
      if (b->property("paleoFocusScroll").toBool())
        continue;
      b->setProperty("paleoFocusScroll", true);
      if (b->focusPolicy() == Qt::NoFocus)
        b->setFocusPolicy(Qt::TabFocus);
      b->installEventFilter(filter);
    }
  }

  void mirror(QAction *action, QAbstractButton *button, bool syncText, bool hideSource)
  {
    if (!action || !button)
      return;
    if (auto *old=action->findChild<QObject *>(QStringLiteral("paleoButtonMirror"))) delete old;
    new ButtonMirror(action, button, syncText);
    if (hideSource)
      button->hide();
  }

  void followVisibility(QAction *checkable, QWidget *widget)
  {
    if (checkable && widget)
      new VisibilityFollower(checkable, widget);
  }

  SARibbonToolButton *buttonFor(SARibbonPanel *panel, QAction *action)
  {
    return panel ? panel->actionToRibbonToolButton(action) : nullptr;
  }

  void markRun(SARibbonToolButton *button)
  {
    if (!button)
      return;
    button->setProperty("paleoRun", true);
    button->style()->unpolish(button);
    button->style()->polish(button);
  }
} // namespace PaleoRibbon
