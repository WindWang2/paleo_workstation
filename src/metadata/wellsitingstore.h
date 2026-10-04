// 层：数据
#pragma once
#include "../domain/wellsitingplan.h"
#include <QString>

class PaleoProjectStore;

// metadata/wellsitingstore — 井网部署方案集在 project.sqlite 的持久化面
// （方向 34）。单行 JSON 文档表 well_siting_scenarios，经 MetaStore
// user_version 门与 LayerManifest/MapVersionStore 共库；写一律经
// PaleoProjectStore::enqueueWrite（FaultSetStore 同式）；无注入（测试直用）
// 时同步直写。
class WellSitingStore
{
public:
    explicit WellSitingStore(const QString &metaSqlitePath,
                             PaleoProjectStore *projectStore = nullptr);

    // 打开（幂等建表）；失败 → false + *error。
    bool open(QString *error = nullptr);

    // 单写实例降级面（AppContext 只读降级时拒绝写）。
    void setReadOnly(bool readOnly) { m_readOnly = readOnly; }
    bool isReadOnly() const { return m_readOnly; }

    // 整集落盘（原子 REPLACE 单行）。写经注入的写队列。
    bool save(const paleo::siting::ScenarioSet &set, QString *error = nullptr);
    // 读回；库中无行 → 集清空返回 true（新工程首用）。
    bool load(paleo::siting::ScenarioSet &set, QString *error = nullptr) const;

private:
    QString m_dbPath;
    PaleoProjectStore *m_store = nullptr; // 可空：无则直写（测试）
    bool m_readOnly = false;
};
