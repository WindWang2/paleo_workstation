// 层：视图
#pragma once
#include "wellsectionpanel.h"

#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

// ui/wellsection — WellSectionFenceWidget：栅状图（fence）编排面板。
// 左 = 剖面条列表（自动布点/手工增删改井序）；中 = 剖面 tabs（每条一个
// WellSectionPanel + 独立 workflow）；右 = 井网预览（剖面线 + 交点井）。
// 交点井联动：任一剖面点名 → 其余剖面同帧 selectWell（不经
// SelectionContext 回环，避免自反馈）。井序持久化经 WellSectionStore
// 的 fence-<n> 节（版本随落盘推进）。
class DataCatalog;
class PaleoTaskService;
class WellSectionWorkflow;
class QListWidget;
class QTabWidget;
class QLabel;
class QToolButton;
namespace metadata {
class WellSectionStore;
}

class WellSectionFenceWidget : public QWidget
{
  Q_OBJECT
  public:
    struct Params
    {
        DataCatalog *catalog = nullptr;
        SelectionContext *selection = nullptr;   // 可空
        PaleoTaskService *tasks = nullptr;       // 可空 → 同步取数
        metadata::WellSectionStore *store = nullptr; // 可空 → 不持久化
        QVector<WellSectionPanel::WellChoice> choices;
    };

    explicit WellSectionFenceWidget(const Params &params,
                                    QWidget *parent = nullptr);

    // ---- 动作（测试同用）----
    // 自动布点：最小交叉启发式（PCA 主轴 + 剪草机条带）。
    void autoPlan(int targetSections);
    // 手工指定/整体替换：每条 = 有序井 id 集；空条目忽略；空集 → 清空。
    void setSections(const QVector<QStringList> &sections);
    // 换工程重绑存储（壳层调用；落库目标跟随新 project.sqlite）。
    void setStore(metadata::WellSectionStore *store);

    // ---- 测试钩子 ----
    int sectionCount() const { return m_sections.size(); }
    QStringList sectionWellIds(int index) const;
    WellSectionPanel *sectionPanel(int index) const;

  signals:
    void sectionsChanged();

  private:
    struct SectionCtl
    {
        WellSectionPanel *panel = nullptr;
        WellSectionWorkflow *wf = nullptr;
        int lastGen = 0;
    };

    void rebuild();       // 井集 → tabs/列表/预览重建 + 落库
    void saveToStore();   // fence-<n> 节写（版本推进）+ 尾部清理
    void loadFromStore();
    void editSectionWells(int index); // 手工指定某条的井与顺序
    void addManualSection();
    void removeSection(int index);
    void updateSync();    // 交点井联动接线（rebuild 后重连）

    Params m_params;
    QVector<QStringList> m_wellIds; // 各剖面井序（权威态）
    QVector<QStringList> m_persistedIds; // 最近落库快照（等值短路）
    QVector<SectionCtl> m_sections;
    QListWidget *m_list = nullptr;
    QTabWidget *m_tabs = nullptr;
    QLabel *m_hint = nullptr;
    QToolButton *m_addBtn = nullptr;
    QToolButton *m_removeBtn = nullptr;
    QToolButton *m_editBtn = nullptr;
    QWidget *m_preview = nullptr;

    // 预览缓存（choices 坐标 → 预览归一）。
    QVector<WellSectionPanel::WellChoice> previewChoices() const {
        return m_params.choices;
    }
};
