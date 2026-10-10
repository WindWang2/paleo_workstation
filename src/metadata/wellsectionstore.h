// 层：数据
#pragma once
#include "domain/wellsection.h"
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

#include <optional>

class PaleoProjectStore;

// metadata/wellsectionstore — 连井剖面编辑产物在 project.sqlite 的持久化
// 面（goal/wellsection-deep）。单节一行：井序（有序井 id 集）+ 层位连线
// 改接集 + 深度显示域（方向 69）+ 版本号——每次 save 版本递增（剖面编辑
// 产物的 manifest 版本口径，同 MapVersionStore::saveVersion 语义）。写一律经
// PaleoProjectStore::enqueueWrite（§41.2 单写纪律）；无注入（测试直用）
// 同步直写。经 MetaStore user_version 门与 LayerManifest/MapVersionStore/
// FaultSetStore 共库。
namespace metadata {

// 层位连线改接：井对无序（落库/查询前归一），键 = 两井 id + 顶名。
struct WellSectionLinkOverride
{
    QString leftWellId, rightWellId, topName;
    bool connected = true; // false = 用户断开

    bool operator==(const WellSectionLinkOverride &o) const
    {
        return leftWellId == o.leftWellId && rightWellId == o.rightWellId &&
               topName == o.topName && connected == o.connected;
    }
    bool operator!=(const WellSectionLinkOverride &o) const { return !(*this == o); }
};

struct WellSectionRecord
{
    QStringList wellIds;                    // 剖面井序（用户编辑产物）
    QVector<WellSectionLinkOverride> linkOverrides;
    // 深度显示域（方向 69：工程级 round-trip；旧库缺列 → MD 默认）。
    wellsection::DepthDomain depthDomain = wellsection::DepthDomain::MD;
    // 井间距模式（方向 98 fence 读回收口）。缺省 nullopt = 行里没存过
    //（方向 98 前旧库行 / 显式 nullopt 落库）——调用方回落自身状态并
    // 如实标注，不得拿 Equal 默认值冒充已存档。
    std::optional<wellsection::SpacingMode> spacing;
    int version = 0;                        // save 递增（首存 = 1）；0 = 库中无行
    bool valid() const { return version > 0; }
};

class WellSectionStore
{
public:
    explicit WellSectionStore(const QString &metaSqlitePath = QString(),
                              PaleoProjectStore *projectStore = nullptr);

    // 打开（幂等建表）；失败 → false + *error。
    bool open(QString *error = nullptr);

    // 换工程重绑（AppContext 值重绑同款语义；未 open 的路径在下一次
    // save/load 时生效）。
    void rebind(const QString &metaSqlitePath, PaleoProjectStore *projectStore);

    // 单写实例降级面（工程被其他实例持锁时拒绝写）。
    void setReadOnly(bool readOnly) { m_readOnly = readOnly; }
    bool isReadOnly() const { return m_readOnly; }

    // 落盘：version = 现值 + 1（写队列内读改写），返回落盘后记录。
    // depthDomain 进剖面状态（方向 69）：缺省 MD；旧库行按列默认 MD 读回。
    // spacing（方向 98）：nullopt 落 NULL（旧库缺列语义）；显式值落库
    // 供 fence 读回（load 对 NULL 回 nullopt——「缺」与「等距」可区分）。
    WellSectionRecord save(const QString &sectionId, const QStringList &wellIds,
                           const QVector<WellSectionLinkOverride> &links,
                           wellsection::DepthDomain depthDomain =
                               wellsection::DepthDomain::MD,
                           std::optional<wellsection::SpacingMode> spacing =
                               std::nullopt,
                           QString *error = nullptr);
    // 读回；库中无行 → version 0 + 空集（新工程首用）。
    WellSectionRecord load(const QString &sectionId,
                           QString *error = nullptr) const;
    // 全部节 id（栅状图多节发现用；升序）。
    QStringList sectionIds(QString *error = nullptr) const;
    // 删节（栅状图条数收缩时清尾行）；无该节 → true。
    bool remove(const QString &sectionId, QString *error = nullptr);

    // ---- 解释岩性来源显式选择（方向 98）----
    // 同井多份解释资产的用户选择（well_id → asset_id）。assetId 空 = 回落
    // 默认「取最新」（删行）。消费仲裁在 WellSectionWorkflow；本表只管
    // 工程级持久化（独立小表，随 project.sqlite 走，同库同写队列纪律）。
    bool saveInterpretationSelection(const QString &wellId,
                                     const QString &assetId,
                                     QString *error = nullptr);
    // 整表读回（开工程/换工程灌入 workflow；空表 = 全默认）。
    QHash<QString, QString> loadInterpretationSelections(
        QString *error = nullptr) const;

private:
    QString m_dbPath;
    PaleoProjectStore *m_store = nullptr; // 可空：无则直写（测试）
    bool m_readOnly = false;
};

} // namespace metadata
