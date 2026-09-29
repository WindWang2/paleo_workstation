// 层：视图
#include "editstack.h"

namespace WellComposite
{

EditStack::EditStack(QObject *parent)
  : QObject(parent)
{
}

void EditStack::trimRedo()
{
  while (static_cast<int>(m_stack.size()) > m_index)
    m_stack.pop_back();
}

void EditStack::push(std::unique_ptr<EditCommand> cmd)
{
  if (!cmd)
    return;

  trimRedo();
  m_stack.push_back(std::move(cmd));
  if (static_cast<int>(m_stack.size()) > kMaxDepth)
  {
    m_stack.erase(m_stack.begin());
    if (m_savedIndex > 0)
      --m_savedIndex;
  }
  m_index = static_cast<int>(m_stack.size());

  // 压入即执行
  m_stack.back()->redo();

  updateDirty(false);
  emit stackChanged();
}

void EditStack::undo()
{
  if (!canUndo())
    return;
  --m_index;
  m_stack.at(m_index)->undo();
  updateDirty(true);
  emit stackChanged();
}

void EditStack::redo()
{
  if (!canRedo())
    return;
  m_stack.at(m_index)->redo();
  ++m_index;
  updateDirty(false);
  emit stackChanged();
}

void EditStack::clear()
{
  m_stack.clear();
  m_index = 0;
  m_savedIndex = 0;
  m_dirty = false;
  emit stackChanged();
  emit dirtyChanged(false);
}

void EditStack::markSaved()
{
  m_savedIndex = m_index;
  if (m_dirty)
  {
    m_dirty = false;
    emit dirtyChanged(false);
  }
}

void EditStack::updateDirty(bool /*undoing*/)
{
  const bool dirty = (m_index != m_savedIndex);
  if (dirty != m_dirty)
  {
    m_dirty = dirty;
    emit dirtyChanged(dirty);
  }
}

QString EditStack::undoText() const
{
  return canUndo() ? m_stack.at(m_index - 1)->text() : QString();
}

QString EditStack::redoText() const
{
  return canRedo() ? m_stack.at(m_index)->text() : QString();
}

QStringList EditStack::undoTexts() const
{
  QStringList texts;
  for (int i = m_index - 1; i >= 0; --i)
    texts << m_stack.at(i)->text();
  return texts;
}

QStringList EditStack::redoTexts() const
{
  QStringList texts;
  for (int i = m_index; i < m_stack.size(); ++i)
    texts << m_stack.at(i)->text();
  return texts;
}

} // namespace WellComposite
