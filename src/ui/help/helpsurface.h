// 层：视图
#pragma once
#include <QObject>
#include <QPointer>
#include <QStringList>

class QAction;
class QMenu;
class QMessageBox;
class QWidget;

namespace paleo::shortcuts
{
class ShortcutSheetDialog;
}

namespace paleo::help
{

// ui/help/helpsurface — 帮助面骨架（方向 63）：「帮助」菜单三入口 +
// F1（快捷键总表）+ Shift+F1（「这是什么？」模式）。
//
// 只立骨架：完整用户手册属另一方向（ledger 记轻决策）。三个动作挂在宿主
// 窗口上（addAction），菜单不展开时快捷键同样生效；键序取自中央注册表。
// 总表与关于框都是非模态单例（offscreen 测试不被 exec 阻塞）。
class HelpSurface : public QObject
{
  Q_OBJECT
public:
  explicit HelpSurface(QWidget *window);

  QMenu *menu() const { return m_menu; }
  QAction *shortcutsAction() const { return m_shortcuts; }
  QAction *whatsThisAction() const { return m_whatsThis; }
  QAction *aboutAction() const { return m_about; }

  // 关于框附加行（如 QGIS 版本），由宿主按自身依赖提供。
  void setAboutDetails(const QStringList &lines) { m_aboutDetails = lines; }

  // 前置声明类型的 QPointer 解引用须见完整类型——访问器定义在 .cpp。
  shortcuts::ShortcutSheetDialog *shortcutSheet() const;
  QMessageBox *aboutBox() const;

public slots:
  // 打开快捷键总表；「当前位置」取调用时的键盘焦点所在面板。
  void showShortcutSheet();
  // 进入「这是什么？」模式；已在模式中则退出。
  void toggleWhatsThisMode();
  void showAbout();

private:
  QWidget *m_window = nullptr;
  QMenu *m_menu = nullptr;
  QAction *m_shortcuts = nullptr;
  QAction *m_whatsThis = nullptr;
  QAction *m_about = nullptr;
  QStringList m_aboutDetails;
  QPointer<shortcuts::ShortcutSheetDialog> m_sheet;
  QPointer<QMessageBox> m_aboutBox;
};

} // namespace paleo::help
