// 层：视图
#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>
#include <vector>

// ui/wellcomposite/editstack — D3.8/D3.9 编辑 undo/redo 栈 + 脏状态跟踪
//
// 命令模式：每个编辑操作压入一个 EditCommand（携带 undo/redo 闭包与操作名）。
// 栈深 50（超出丢弃最旧）；菜单项显示操作名；脏状态 = 自上次 markSaved 以来
// 有未撤销的编辑。

namespace WellComposite
{

class EditCommand
{
public:
  EditCommand(const QString &text, std::function<void()> undoFn, std::function<void()> redoFn)
    : m_text(text), m_undo(std::move(undoFn)), m_redo(std::move(redoFn))
  {
  }

  QString text() const { return m_text; }
  void undo() { if (m_undo) m_undo(); }
  void redo() { if (m_redo) m_redo(); }

private:
  QString m_text;
  std::function<void()> m_undo;
  std::function<void()> m_redo;
};

class EditStack : public QObject
{
  Q_OBJECT

public:
  static constexpr int kMaxDepth = 50; // D3.8 栈深 50

  explicit EditStack(QObject *parent = nullptr);

  void push(std::unique_ptr<EditCommand> cmd); // 压栈即执行 redo
  bool canUndo() const { return m_index > 0; }
  bool canRedo() const { return m_index < m_stack.size(); }
  void undo();
  void redo();
  void clear();

  // D3.9 脏状态：自 markSaved 后有净编辑（undo 回到保存点 = 干净）
  bool isDirty() const { return m_dirty; }
  void markSaved();
  QString undoText() const;  // 「撤销: <操作名>」，栈空返回空
  QString redoText() const;
  QStringList undoTexts() const; // 全量（菜单/测试）
  QStringList redoTexts() const;
  int count() const { return static_cast<int>(m_stack.size()); }

signals:
  void stackChanged();
  void dirtyChanged(bool dirty);

private:
  void trimRedo();
  void updateDirty(bool undoing);

  std::vector<std::unique_ptr<EditCommand>> m_stack;
  int m_index = 0; // 下一个 redo 命令索引
  int m_savedIndex = 0;
  bool m_dirty = false;
};

} // namespace WellComposite
