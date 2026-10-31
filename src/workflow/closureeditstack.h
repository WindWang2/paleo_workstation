// 层：功能
#pragma once
// workflow/closureeditstack — 工作台「图层属性/存储直写」类修改的闭包撤销栈
// （#741 固定显示层级、#742 会话外绘制约束）。QUndoStack 属 QtWidgets 词表
// 禁项（tools/check_layering.py），功能层用纯 QtCore/STL 闭包栈，与
// FaultEditStack 同语义：record 时修改已执行成功；undo/redo 失败时条目原地
// 保留，历史不失序。视图层经 onChanged 跟随（#748 撤销路由）。
#include <QString>
#include <QVector>
#include <functional>
#include <utility>

namespace paleo::workflow {

class ClosureEditStack
{
public:
    using Fn = std::function<bool()>;
    // 记录一条已执行成功的修改；作废已撤销分支。
    void record(const QString &text, Fn undoFn, Fn redoFn)
    {
        m_undone.clear();
        m_done.push_back({text, std::move(undoFn), std::move(redoFn)});
        notify();
    }
    bool undo()
    {
        if (m_done.isEmpty() || !m_done.last().undoFn())
            return false;
        m_undone.push_back(m_done.takeLast());
        notify();
        return true;
    }
    bool redo()
    {
        if (m_undone.isEmpty() || !m_undone.last().redoFn())
            return false;
        m_done.push_back(m_undone.takeLast());
        notify();
        return true;
    }
    bool canUndo() const { return !m_done.isEmpty(); }
    bool canRedo() const { return !m_undone.isEmpty(); }
    int count() const { return static_cast<int>(m_done.size()); }
    QString undoText() const { return m_done.isEmpty() ? QString() : m_done.last().text; }
    QString redoText() const { return m_undone.isEmpty() ? QString() : m_undone.last().text; }
    void clear()
    {
        m_done.clear();
        m_undone.clear();
        notify();
    }
    // 单一观察者（视图层撤销路由）；空函数即解除。
    void setOnChanged(std::function<void()> fn) { m_onChanged = std::move(fn); }

private:
    struct Entry
    {
        QString text;
        Fn undoFn;
        Fn redoFn;
    };
    void notify()
    {
        if (m_onChanged)
            m_onChanged();
    }
    QVector<Entry> m_done;
    QVector<Entry> m_undone;
    std::function<void()> m_onChanged;
};

} // namespace paleo::workflow
