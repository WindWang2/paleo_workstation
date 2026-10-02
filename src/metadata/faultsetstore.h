// 层：数据
#pragma once
#include "../domain/faultset.h"
#include <QString>

class PaleoProjectStore;

// metadata/faultsetstore — 断层解释集（FaultSet）在 project.sqlite 的
// 持久化面（goal/fault-interpretation）。单行 JSON 文档表 fault_set，
// 经 MetaStore user_version 门与 LayerManifest/MapVersionStore 共库。
// 写一律经 PaleoProjectStore::enqueueWrite（EnqueueFn 注入，ConstraintStore
// 同式）；无注入（测试直用）时同步直写。
class FaultSetStore
{
public:
    explicit FaultSetStore(const QString &metaSqlitePath,
                           PaleoProjectStore *projectStore = nullptr);

    // 打开（幂等建表）；失败 → false + *error。
    bool open(QString *error = nullptr);

    // 单写实例降级面（AppContext 只读降级时拒绝写）。
    void setReadOnly(bool readOnly) { m_readOnly = readOnly; }
    bool isReadOnly() const { return m_readOnly; }

    // 整集落盘（原子 REPLACE 单行）。写经注入的写队列。
    bool save(const paleo::fault::FaultSet &set, QString *error = nullptr);
    // 读回；库中无行 → 集清空返回 true（新工程首用）。
    bool load(paleo::fault::FaultSet &set, QString *error = nullptr) const;

private:
    QString m_dbPath;
    PaleoProjectStore *m_store = nullptr; // 可空：无则直写（测试）
    bool m_readOnly = false;
};
