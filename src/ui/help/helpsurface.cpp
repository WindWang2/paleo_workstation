// 层：视图
#include "helpsurface.h"

#include "../shortcuts/shortcutcatalog.h"
#include "../shortcuts/shortcutsheetdialog.h"

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QMenu>
#include <QMessageBox>
#include <QWhatsThis>
#include <QWidget>

namespace paleo::help
{

HelpSurface::HelpSurface(QWidget *window) : QObject(window), m_window(window)
{
  setObjectName(QStringLiteral("helpSurface"));
  m_menu = new QMenu(tr("帮助"), window);
  m_menu->setObjectName(QStringLiteral("helpMenu"));

  m_shortcuts = new QAction(tr("快捷键总表…"), window);
  m_shortcuts->setObjectName(QStringLiteral("helpShortcutsAction"));
  m_shortcuts->setStatusTip(tr("列出全部快捷键，可搜索、可复制"));
  shortcuts::bindAction(QStringLiteral("main.help.shortcuts"), m_shortcuts);
  connect(m_shortcuts, &QAction::triggered, this, &HelpSurface::showShortcutSheet);

  m_whatsThis = new QAction(tr("这是什么？"), window);
  m_whatsThis->setObjectName(QStringLiteral("helpWhatsThisAction"));
  m_whatsThis->setStatusTip(tr("进入后点击界面上的控件，查看它的一句话说明；按 Esc 退出"));
  shortcuts::bindAction(QStringLiteral("main.help.whatsThis"), m_whatsThis);
  connect(m_whatsThis, &QAction::triggered, this, &HelpSurface::toggleWhatsThisMode);

  m_about = new QAction(tr("关于 Paleo Workbench…"), window);
  m_about->setObjectName(QStringLiteral("helpAboutAction"));
  connect(m_about, &QAction::triggered, this, &HelpSurface::showAbout);

  m_menu->addAction(m_shortcuts);
  m_menu->addAction(m_whatsThis);
  m_menu->addSeparator();
  m_menu->addAction(m_about);

  // 挂到宿主窗口：菜单收起时 F1 / Shift+F1 照样生效（WindowShortcut）。
  if (window)
  {
    window->addAction(m_shortcuts);
    window->addAction(m_whatsThis);
  }
}

shortcuts::ShortcutSheetDialog *HelpSurface::shortcutSheet() const
{
  return m_sheet.data();
}

QMessageBox *HelpSurface::aboutBox() const
{
  return m_aboutBox.data();
}

void HelpSurface::showShortcutSheet()
{
  // 先取焦点（总表弹出后焦点会移进对话框）。
  const QString context = shortcuts::contextForWidget(QApplication::focusWidget());
  if (!m_sheet)
    m_sheet = new shortcuts::ShortcutSheetDialog(shortcuts::registry(), m_window);
  m_sheet->setActiveContext(context == QLatin1String("main") ? QString() : context);
  m_sheet->show();
  m_sheet->raise();
  m_sheet->activateWindow();
}

void HelpSurface::toggleWhatsThisMode()
{
  if (QWhatsThis::inWhatsThisMode())
    QWhatsThis::leaveWhatsThisMode();
  else
    QWhatsThis::enterWhatsThisMode();
}

void HelpSurface::showAbout()
{
  QStringList lines;
  lines << tr("<b>Paleo Workbench</b> — 古地理编图工作台");
  lines << tr("基于 QGIS 的石油地质古地理编图桌面应用。");
  const QString version = QCoreApplication::applicationVersion();
  if (!version.isEmpty())
    lines << tr("版本：%1").arg(version.toHtmlEscaped());
  lines << tr("Qt：%1").arg(QString::fromLatin1(qVersion()));
  for (const QString &l : m_aboutDetails)
    lines << l.toHtmlEscaped();
  if (!m_aboutBox)
  {
    m_aboutBox = new QMessageBox(m_window);
    m_aboutBox->setObjectName(QStringLiteral("aboutDialog"));
    m_aboutBox->setWindowTitle(tr("关于 Paleo Workbench"));
    m_aboutBox->setIcon(QMessageBox::NoIcon);
    m_aboutBox->setTextFormat(Qt::RichText);
    m_aboutBox->setStandardButtons(QMessageBox::Close);
    m_aboutBox->setModal(false);
  }
  m_aboutBox->setText(lines.join(QStringLiteral("<br>")));
  m_aboutBox->show();
  m_aboutBox->raise();
}

} // namespace paleo::help
