// 层：视图
// ui/pages/dataops/dataopsundo — D5 视图层命令栈。
// 挂接/解挂/设主/角色变更/标签/改型/软删/恢复/建实体/改名 override 全进栈；
// 命令对象统一接口（D5.7）：redo()/undo()/text()/id()/mergeWith()。
// 栈深 50（D5.3），超限弹底；相邻同类命令经 mergeWith 合并（如对同一资产
// 连续打标签）。栈随项目会话清空（D5.6：catalog 路径变化 → clear()）。
#pragma once

#include <QList>
#include <QObject>
#include <QString>

namespace paleo::dataops
{

// 命令基类：redo/undo 均为「应用到 catalog/sidecar 的纯操作」，不触发 UI
// 刷新（刷新由栈的信号订阅方统一做——避免一次 undo 连发 N 次全表重建）。
class DataOpCommand
{
public:
  virtual ~DataOpCommand() = default;
  // PW-015：返回 false = 未能把 catalog/sidecar 带到目标状态（原因见
  // errorText()）。栈据此不再搬运该命令，避免历史与真实状态脱节。
  virtual bool redo() = 0;
  virtual bool undo() = 0;
  QString errorText() const { return m_error; }
  // 菜单/状态栏文案（D5.2）：「挂接 A1.Las→well-A1」式主谓宾。
  virtual QString text() const = 0;
  // 同类合并键：相邻栈顶命令 id 相同且 mergeWith 接受 → 合并为一条。
  virtual QByteArray id() const { return QByteArray(); }
  // 返回 true = 吞并 other（other 的效果已被本命令覆盖）。
  virtual bool mergeWith(const DataOpCommand *other)
  {
    Q_UNUSED(other);
    return false;
  }

protected:
  QString m_error;
};

// PW-049：宏命令——批量操作作为一条撤销记录（一次 Ctrl+Z 整组撤销，
// 也只占一个栈深名额）。redo 顺序执行、undo 逆序执行；中途失败时回滚
// 已完成的成员，保证整组要么全生效要么全不生效。
class DataOpMacro : public DataOpCommand
{
public:
  DataOpMacro(QString text, QList<DataOpCommand *> children)
    : m_text(std::move(text)), m_children(std::move(children))
  {
  }
  ~DataOpMacro() override { qDeleteAll(m_children); }
  bool redo() override
  {
    for (int i = 0; i < m_children.size(); ++i)
    {
      if (!m_children.at(i)->redo())
      {
        m_error = m_children.at(i)->errorText();
        for (int j = i - 1; j >= 0; --j)
          m_children.at(j)->undo();
        return false;
      }
    }
    return true;
  }
  bool undo() override
  {
    for (int i = m_children.size() - 1; i >= 0; --i)
    {
      if (!m_children.at(i)->undo())
      {
        m_error = m_children.at(i)->errorText();
        for (int j = i + 1; j < m_children.size(); ++j)
          m_children.at(j)->redo();
        return false;
      }
    }
    return true;
  }
  QString text() const override { return m_text; }
  int childCount() const { return m_children.size(); }

private:
  QString m_text;
  QList<DataOpCommand *> m_children;
};

class DataOpsUndoStack : public QObject
{
  Q_OBJECT

public:
  static constexpr int kMaxDepth = 50; // D5.3

  explicit DataOpsUndoStack(QObject *parent = nullptr)
    : QObject(parent)
  {
  }

  ~DataOpsUndoStack() override { qDeleteAll(m_undo); qDeleteAll(m_redo); }

  // 执行命令并入栈（redo 在此处调用一次）。空指针忽略。
  // PW-015：redo 失败 → 命令不入栈（删除），返回 false，原因见 lastError()。
  bool push(DataOpCommand *cmd)
  {
    if (!cmd)
      return false;
    if (!cmd->redo())
    {
      m_lastError = cmd->errorText();
      delete cmd;
      emit operationFailed(m_lastError);
      return false;
    }
    m_lastError.clear();
    // 相邻同类合并（D5.3）：栈顶同 id 且 mergeWith 接受 → 不再压新条目。
    if (!m_undo.isEmpty() && !cmd->id().isEmpty() &&
        m_undo.last()->id() == cmd->id() && m_undo.last()->mergeWith(cmd))
    {
      delete cmd;
      clearRedo(); // 分支点作废（指针元素须 delete，不能只 clear）
      emit stackChanged();
      return true;
    }
    m_undo.append(cmd);
    if (m_undo.size() > kMaxDepth)
      delete m_undo.takeFirst(); // 弹底：最老命令的效果成为既成事实
    clearRedo();
    emit stackChanged();
    return true;
  }

  // 最近一次失败的原因（push/undo/redo 失败时设置；成功时清空）。
  QString lastError() const { return m_lastError; }

  bool canUndo() const { return !m_undo.isEmpty(); }
  bool canRedo() const { return !m_redo.isEmpty(); }
  QString undoText() const { return m_undo.isEmpty() ? QString() : m_undo.last()->text(); }
  QString redoText() const { return m_redo.isEmpty() ? QString() : m_redo.last()->text(); }
  int depth() const { return m_undo.size(); }
  int redoDepth() const { return m_redo.size(); }

  // 撤销/重做。返回执行的命令文案（D5.4 状态栏反馈），空 = 栈空未动。
  QString undo()
  {
    if (m_undo.isEmpty())
      return QString();
    DataOpCommand *cmd = m_undo.takeLast();
    if (!cmd->undo())
    {
      failAndReset(cmd);
      return QString();
    }
    m_lastError.clear();
    m_redo.append(cmd);
    emit stackChanged();
    return cmd->text();
  }
  QString redo()
  {
    if (m_redo.isEmpty())
      return QString();
    DataOpCommand *cmd = m_redo.takeLast();
    if (!cmd->redo())
    {
      failAndReset(cmd);
      return QString();
    }
    m_lastError.clear();
    m_undo.append(cmd);
    if (m_undo.size() > kMaxDepth)
      delete m_undo.takeFirst();
    emit stackChanged();
    return cmd->text();
  }

  // D5.6：项目会话清空策略——open()/切换工程时调用；redo 支路一并清。
  void clear()
  {
    if (m_undo.isEmpty() && m_redo.isEmpty())
      return;
    qDeleteAll(m_undo);
    qDeleteAll(m_redo);
    m_undo.clear();
    m_redo.clear();
    emit stackChanged();
  }

  // 测试/审计面：当前栈内命令文案（栈底→栈顶）。
  QStringList undoTexts() const
  {
    QStringList out;
    for (const DataOpCommand *c : m_undo)
      out << c->text();
    return out;
  }

signals:
  // 任何栈变化（push/undo/redo/clear）——订阅方刷新按钮态/菜单文案/面板。
  void stackChanged();
  // PW-015：命令执行失败（原因文案）。
  void operationFailed(const QString &error);

private:
  // PW-015：undo/redo 失败后历史已不再描述真实状态——整栈作废（失败命令
  // 一并删除），宁可丢历史也不让后续撤销在错误基线上操作。
  void failAndReset(DataOpCommand *cmd)
  {
    m_lastError = cmd->errorText();
    if (m_lastError.isEmpty())
      m_lastError = tr("操作未能完成");
    delete cmd;
    qDeleteAll(m_undo);
    qDeleteAll(m_redo);
    m_undo.clear();
    m_redo.clear();
    emit operationFailed(m_lastError);
    emit stackChanged();
  }

  // 作废 redo 支路：QList 的 clear 只销毁指针元素，指向对象要 qDeleteAll。
  void clearRedo()
  {
    qDeleteAll(m_redo);
    m_redo.clear();
  }

  QList<DataOpCommand *> m_undo;
  QList<DataOpCommand *> m_redo;
  QString m_lastError;
};

} // namespace paleo::dataops
