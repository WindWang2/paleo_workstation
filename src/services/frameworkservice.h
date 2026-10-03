// 层：数据
#pragma once
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include "../algorithms/frameworksuggester.h"
#include "../catalog/frameworkstore.h"
#include "../domain/frameworkdiagnostics.h"
#include "../domain/sequenceframework.h"
#include "../domain/wellrecords.h"

// services/ — 层序地层格架的读侧门面 + 建议/诊断编排（方向 28）。
//
// 为什么放在 services：层边界护栏禁止 ui→algorithms 与 store→algorithms
// （tools/check_layering.py），而「建议」既要用算法核又要落库。services 是
// 数据层里唯一同时看得见 paleo_store 与 paleo_algorithms 的位置，因此
// 编排放这里，UI 只消费本类发出的信号与值。
//
// 写库纪律：suggest() 是纯读（catalog 零写入）；唯一写库入口是
// commitAccepted()——只应用 accepted==true 的候选。
namespace SequenceFramework
{

class FrameworkService : public QObject
{
  Q_OBJECT
  public:
    explicit FrameworkService( const QString &projectDir, QObject *parent = nullptr );

    // 接线：绑定活 catalog（宿主在工程打开时调用；catalog 换工程后重接）。
    void attachCatalog( DataCatalog *catalog );
    DataCatalog *catalog() const { return m_catalog; }
    QString projectDir() const { return m_projectDir; }

    const Framework &framework() const { return m_fw; }
    bool reload( QString *error = nullptr );
    bool save( const Framework &fw, QString *error = nullptr );

    // ---- 建议面（只出候选，不写库）----
    // 井分层来自 catalog 的 well_stratification 资产（受管版本落盘读出）。
    QVector<WellTopRecord> loadWellTops( QString *error = nullptr ) const;
    // 纯函数面：给定井分层直接算候选，catalog 零写入。
    QVector<SuggestionCandidate> suggest( const QVector<WellTopRecord> &tops,
                                          const SuggestOptions &opt = SuggestOptions() ) const;
    // 唯一写库入口：把 accepted 候选应用进格架并落一个新版本。
    // 没有任何 accepted 候选 → 不落盘、返回 true（不空涨 revision）。
    bool commitAccepted( const QVector<SuggestionCandidate> &candidates,
                         QString *error = nullptr );

    // ---- 诊断面 ----
    // 已入库且非未决的层序界面（有编图数据）。
    QStringList mappedHorizons() const;
    // 井分层里出现的全部分层名（引用完整性校验的分母）。
    static QStringList layerNamesOf( const QVector<WellTopRecord> &tops );
    // 组装诊断输入（horizons 走 mappingHorizons()，层位序唯一权威）。
    DiagnosticInput buildDiagnosticInput( const QVector<WellTopRecord> &tops ) const;
    DiagnosticReport diagnose( const QVector<WellTopRecord> &tops ) const;

  signals:
    void frameworkChanged();   // reload/save/commitAccepted 成功后发射
    void catalogWriteRejected( const QString &reason );

  private:
    QString m_projectDir;
    DataCatalog *m_catalog = nullptr;
    Framework m_fw;
};

} // namespace SequenceFramework
