// 层：视图
#include "paleoribbon.h"

#include "paleotheme.h"

#include <SARibbon.h>

#include <QAbstractButton>
#include <QAction>
#include <QEvent>
#include <QPointer>
#include <QStyle>

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
        : QObject(action), m_action(action), m_button(button), m_syncText(syncText),
          m_baseTip(action->toolTip())
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

    protected:
      bool eventFilter(QObject *obj, QEvent *ev) override
      {
        if (obj == m_button &&
            (ev->type() == QEvent::EnabledChange || ev->type() == QEvent::ToolTipChange))
          sync();
        return QObject::eventFilter(obj, ev);
      }

    private:
      void sync()
      {
        if (!m_button || !m_action)
          return;
        m_action->setEnabled(m_button->isEnabled());
        if (m_syncText && !m_button->text().isEmpty())
          m_action->setText(m_button->text());
        const QString tip = m_button->toolTip();
        m_action->setToolTip(tip.isEmpty() ? m_baseTip : tip);
        if (m_button->isCheckable())
        {
          m_action->setCheckable(true);
          m_action->setChecked(m_button->isChecked());
        }
      }

      QPointer<QAction> m_action;
      QPointer<QAbstractButton> m_button;
      bool m_syncText;
      QString m_baseTip;
  };

  // ShowToParent/HideToParent 只看显式 show()/hide()——父链没上屏时同样送达。
  class VisibilityFollower : public QObject
  {
    public:
      VisibilityFollower(QAction *action, QWidget *widget)
        : QObject(action), m_action(action), m_widget(widget)
      {
        widget->installEventFilter(this);
        action->setChecked(!widget->isHidden());
      }

    protected:
      bool eventFilter(QObject *obj, QEvent *ev) override
      {
        if (obj == m_widget && m_action &&
            (ev->type() == QEvent::ShowToParent || ev->type() == QEvent::HideToParent))
          m_action->setChecked(ev->type() == QEvent::ShowToParent);
        return QObject::eventFilter(obj, ev);
      }

    private:
      QPointer<QAction> m_action;
      QPointer<QWidget> m_widget;
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
    // 整体替换（不是追加）：SA::applyRibbonTheme 会覆盖窗口样式表，这里
    // 重设一份完整的「ribbon 细节 + 壳样式」；主题来回切换不会累积。
    win->setStyleSheet(PaleoTheme::ribbonStyleSheet() + QLatin1Char('\n') +
                       shellQss);
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
