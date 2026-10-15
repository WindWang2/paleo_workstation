// 层：视图
#pragma once
// 方向 65：dockwidget 拆分后的内部契约头——undo 命令族被「解释会话」与
// 「追踪/体传播」两个 TU 共用，故提出为头。其余 helper（themed 三件套 /
// WellSideTraceWidget / ProjectPointOntoPolyline / wellTimeDepthTable /
// wellImpedanceTwt）经实测均为单 TU 局部，留在各自 TU 的匿名命名空间内。
// 类体内定义即隐式 inline，多 TU include 无 ODR 风险。
// 名字落在 namespace seismic 而非嵌套子命名空间：两个消费 TU 同在该命名空间内，
// 未限定查找即可命中，迁出的命令体因此零字符改动（原匿名命名空间 → 本头）。
// 全仓实测无同名类（rg PickCommand 仅命中本族）。
#include "ui/seismicsection/seismicsectiondockwidget.h"
#include "ui/seismicsection/seismicpickpanel.h"

#include <QUndoCommand>

namespace seismic {

// undo 命令（D4.6）：对会话模型的原子操作 + 面板/叠加刷新
class PickCommandBase : public QUndoCommand
{
public:
    PickCommandBase(SeismicSectionDockWidget *dock, QUndoCommand *parent = nullptr)
        : QUndoCommand(parent), m_dock(dock) {}

protected:
    void refresh()
    {
        if (m_dock && m_dock->pickPanel())
            m_dock->pickPanel()->refreshFromSession();
        if (m_dock)
            m_dock->saveInterpretationSession(); // 自动保存（D4.8 会话持久化）
        // opt-02：画布拾取叠加与会话同源刷新——undo/redo 后画布必须与
        // 面板/落盘一致（旧实现 Add::undo、Remove::undo/redo、Rename 均
        // 不刷画布：撤销后画布残留幽灵拾取点 / 恢复的点不显示）。
        if (m_dock)
            m_dock->refreshInterpretationOverlay();
    }
    SeismicSectionDockWidget *m_dock;
};

class AddPicksCommand : public PickCommandBase
{
public:
    AddPicksCommand(SeismicSectionDockWidget *dock, QList<SeismicPick> picks)
        : PickCommandBase(dock), m_picks(std::move(picks))
    {
        setText(QObject::tr("添加 %1 个拾取").arg(m_picks.size()));
    }
    void undo() override
    {
        // opt-02：按 redo 实际分配的 id 回收（与 ReplacePicksCommand 同款
        // 记账）——旧实现按值 indexOf，但 redo 给拾取重新发了 id，撤销
        // 「添加」从未真正从会话删掉这些拾取（面板行消失是面板按栈过滤
        // 的假象，落盘会话仍含它们）。
        auto &picks = m_dock->mutableSession().picks;
        for (const int id : std::as_const(m_lastIds))
            for (int i = picks.size() - 1; i >= 0; --i)
                if (picks[i].id == id)
                {
                    picks.removeAt(i);
                    break;
                }
        refresh();
    }
    void redo() override
    {
        m_lastIds.clear();
        auto &session = m_dock->mutableSession();
        for (SeismicPick p : m_picks)
        {
            p.id = session.nextId++;
            session.picks.append(p);
            m_lastIds.append(p.id);
        }
        refresh();
    }

private:
    QList<SeismicPick> m_picks;
    QList<int> m_lastIds; // 最近一次 redo 分配的 id（undo 按此回收）
};

class RemovePickCommand : public PickCommandBase
{
public:
    RemovePickCommand(SeismicSectionDockWidget *dock, SeismicPick pick)
        : PickCommandBase(dock), m_pick(pick)
    {
        setText(QObject::tr("删除拾取 %1").arg(pick.id));
    }
    void undo() override
    {
        m_dock->mutableSession().picks.append(m_pick);
        refresh();
    }
    void redo() override
    {
        auto &session = m_dock->mutableSession();
        const int idx = session.picks.indexOf(m_pick);
        if (idx >= 0)
            session.picks.removeAt(idx);
        refresh();
    }

private:
    SeismicPick m_pick;
};

class RenamePickCommand : public PickCommandBase
{
public:
    RenamePickCommand(SeismicSectionDockWidget *dock, int pickId, QString oldName, QString newName)
        : PickCommandBase(dock), m_id(pickId), m_old(std::move(oldName)), m_new(std::move(newName))
    {
        setText(QObject::tr("拾取 %1 改层位 %2→%3").arg(pickId).arg(m_old, m_new));
    }
    void apply(const QString &name)
    {
        for (SeismicPick &p : m_dock->mutableSession().picks)
            if (p.id == m_id)
                p.horizonName = name;
        refresh();
    }
    void undo() override { apply(m_old); }
    void redo() override { apply(m_new); }

private:
    int m_id;
    QString m_old, m_new;
};

// goal/horizon-autotrack — 追踪合并替换（一步 undo）：redo = 移除被超越的
// 机器拾取 + 添加新拾取；undo = 原样复原（被移拾取按原 id 回位）。
// 手动拾取（conf==1，D4.10 语义）不参与替换——编排层已在入栈前滤除。
class ReplacePicksCommand : public PickCommandBase
{
public:
    ReplacePicksCommand(SeismicSectionDockWidget *dock,
                        QList<SeismicPick> removed, QList<SeismicPick> added)
        : PickCommandBase(dock), m_removed(std::move(removed)), m_added(std::move(added))
    {
        setText(QObject::tr("追踪替换 %1 个拾取").arg(m_added.size()));
    }
    void undo() override
    {
        auto &picks = m_dock->mutableSession().picks;
        for (const int id : std::as_const(m_lastAddedIds))
            for (int i = picks.size() - 1; i >= 0; --i)
                if (picks[i].id == id)
                {
                    picks.removeAt(i);
                    break;
                }
        for (const SeismicPick &p : std::as_const(m_removed))
            picks.append(p); // 原 id 复原
        refresh();
    }
    void redo() override
    {
        auto &picks = m_dock->mutableSession().picks;
        auto &session = m_dock->mutableSession();
        for (const SeismicPick &p : std::as_const(m_removed))
            for (int i = picks.size() - 1; i >= 0; --i)
                if (picks[i].id == p.id)
                {
                    picks.removeAt(i);
                    break;
                }
        m_lastAddedIds.clear();
        for (SeismicPick p : std::as_const(m_added))
        {
            p.id = session.nextId++;
            picks.append(p);
            m_lastAddedIds.append(p.id);
        }
        refresh();
    }

private:
    QList<SeismicPick> m_removed;
    QList<SeismicPick> m_added;
    QList<int> m_lastAddedIds; // 最近一次 redo 分配的 id（undo 按此回收）
};

} // namespace seismic
