// 层：数据
#pragma once
#include <QString>
#include <QStringList>
#include <QVector>

class PaleoProjectStore;

// metadata/wellsectionstore — 连井剖面编辑产物在 project.sqlite 的持久化
// 面（goal/wellsection-deep）。单节一行：井序（有序井 id 集）+ 层位连线
// 改接集 + 版本号——每次 save 版本递增（剖面编辑产物的 manifest 版本口径，
// 同 MapVersionStore::saveVersion 语义）。写一律经
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
    WellSectionRecord save(const QString &sectionId, const QStringList &wellIds,
                           const QVector<WellSectionLinkOverride> &links,
                           QString *error = nullptr);
    // 读回；库中无行 → version 0 + 空集（新工程首用）。
    WellSectionRecord load(const QString &sectionId,
                           QString *error = nullptr) const;

private:
    QString m_dbPath;
    PaleoProjectStore *m_store = nullptr; // 可空：无则直写（测试）
    bool m_readOnly = false;
};

} // namespace metadata
