// 层：视图
#pragma once
#include "wellsectionpanel.h"

#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

#include <optional>

// ui/wellsection — WellSectionFenceWidget：栅状图（fence）编排面板。
// 左 = 剖面条列表（自动布点/手工增删改井序）；中 = 剖面 tabs（每条一个
// WellSectionPanel + 独立 workflow）；右 = 井网预览（剖面线 + 交点井）。
// 交点井联动：任一剖面点名 → 其余剖面同帧 selectWell（不经
// SelectionContext 回环，避免自反馈）。井序 + 深度域 + 井间距经
// WellSectionStore 的 fence-<n> 节持久化（方向 98 读回收口：域/间距随节
// 井集同存同读，旧库行缺 spacing 列回落当前面板状态并如实标注）。
class DataCatalog;
class PaleoTaskService;
class WellSectionWorkflow;
class FaultSetStore;
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
        FaultSetStore *faultStore = nullptr;     // 可空 → 断层投绘降级
        QVector<WellSectionPanel::WellChoice> choices;
    };

  public:
    // 井选项刷新（catalog 变更后壳层调用；预览/手工编辑用新井集）。
    void setChoices(const QVector<WellSectionPanel::WellChoice> &choices);

    explicit WellSectionFenceWidget(const Params &params,
                                    QWidget *parent = nullptr);

    // ---- 动作（测试同用）----
    // 自动布点：最小交叉启发式（PCA 主轴 + 剪草机条带）。
    void autoPlan(int targetSections);
    // 手工指定/整体替换：每条 = 有序井 id 集；空条目忽略；空集 → 清空。
    void setSections(const QVector<QStringList> &sections);
    // 换工程重绑存储（壳层调用；落库目标跟随新 project.sqlite）。
    void setStore(metadata::WellSectionStore *store);
    // 工程级剖面状态 + 视图同步（方向 69：深度域随工程落库 round-trip；
    // 方向 98：井间距同入剖面状态——域/间距随 fence-<n> 节落库并读回，
    // 旧库行缺 spacing 列回落主面板当前状态并标注。三处一致性）：应用到
    // 全部剖面面板（程序化 setter，不回发信号——防环路）。
    void setDepthDomain(wellsection::DepthDomain domain);
    void setSpacingMode(wellsection::SpacingMode mode);

    // ---- 测试钩子 ----
    int sectionCount() const { return m_sections.size(); }
    QStringList sectionWellIds(int index) const;
    WellSectionPanel *sectionPanel(int index) const;
    // 读回状态标注（测试钩子）：store 有节但缺 spacing 存档时的回落说明
    //（空串 = 无回落或无存档）。
    QString storeFallbackNote() const { return m_storeNote; }

  signals:
    void sectionsChanged();
    // 剖面面板的用户域/井距动作向上传播（壳层接主面板；程序化同步不发）。
    void depthDomainChanged(wellsection::DepthDomain domain);
    void spacingModeChanged(wellsection::SpacingMode mode);

  private:
    struct SectionCtl
    {
        WellSectionPanel *panel = nullptr;
        WellSectionWorkflow *wf = nullptr;
        int lastGen = 0;
    };

    // 井集 → tabs/列表/预览重建；persist=false = 纯读回路径（构造/换库，
    // 不落库——旧档缺 spacing 的回落标注不在打开瞬间被首写抹掉，版本也
    // 不随打开推进）。
    void rebuild(bool persist = true);
    void saveToStore();   // fence-<n> 节写（版本推进）+ 尾部清理
    void loadFromStore();
    void editSectionWells(int index); // 手工指定某条的井与顺序
    void addManualSection();
    void removeSection(int index);
    void updateSync();    // 交点井联动接线（rebuild 后重连）

    Params m_params;
    QVector<QStringList> m_wellIds; // 各剖面井序（权威态）
    QVector<QStringList> m_persistedIds; // 最近落库快照（等值短路）
    // 最近落库的域/间距快照（等值短路；nullopt = 无节/间距未存档）。
    std::optional<wellsection::DepthDomain> m_persistedDomain;
    std::optional<wellsection::SpacingMode> m_persistedSpacing;
    // loadFromStore 读回的域/间距（rebuild 建面板后程序化应用；nullopt =
    // 无存档或缺列回落——缺列时记 m_storeNote 如实标注）。
    std::optional<wellsection::DepthDomain> m_loadedDomain;
    std::optional<wellsection::SpacingMode> m_loadedSpacing;
    QString m_storeNote; // 读回回落标注（提示行尾附注；空 = 无回落）
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
