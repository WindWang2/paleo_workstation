// 层：视图
#pragma once

#include <QPointer>
#include <QString>

class QBoxLayout;
class QMainWindow;
class QTabBar;
class WorkspaceFocus;

void installWorkspaceFocus(QMainWindow *window, QTabBar *tabs, QBoxLayout *layout);
void setWorkspaceDocumentCaption(QTabBar *tabs, const QString &surface, const QString &title,
                                 const QString &detail = {});
void refreshWorkspaceDocumentCaptions(QTabBar *tabs);
void exitWorkspaceFocus(QMainWindow *window);

// Run page policy and layout serialization against the ordinary window layout,
// including when one policy change calls another. The active document stays put.
class WorkspaceLayoutScope final
{
public:
  explicit WorkspaceLayoutScope(QMainWindow *window);
  ~WorkspaceLayoutScope();
  WorkspaceLayoutScope(const WorkspaceLayoutScope &) = delete;
  WorkspaceLayoutScope &operator=(const WorkspaceLayoutScope &) = delete;

private:
  QPointer<QObject> m_focus;
};
