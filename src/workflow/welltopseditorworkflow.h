// 层：功能
#pragma once
#include <QString>
#include <QVector>

#include "../domain/wellrecords.h"
#include "../domain/welltopsedit.h"

class DataCatalog;

// workflow/welltopseditorworkflow — 井分层编辑的版本化落库编排（方向 32）。
//
// 版本口径：编辑 = 同一 well_stratification 资产上的新 DERIVED 版本
// （versionNumber+1）。消费端（ProjectDataFacade::topsFor）读 primary 链接资产
// 的 currentVersion（最高号），新版本即自动成为下游读到的内容。
//
// 血缘口径（关键）：编辑版本**不**把旧版本记入 parentVersionIds——addVersion 的
// supersede 语义会把旧版本下游闭包（含本新版本自身，DERIVED）标 stale
// （datacatalog.cpp addVersion/markStaleDownstreamOf）。血缘改记 extra：
//   editKind     edit | batch-rename | batch-shift | batch-delete | merge | rollback
//   editWell     目标井（单井编辑/合并）
//   rowsAdded/rowsRemoved/rowsChanged   差异摘要（DiffSummary）
//   editBaseline/editBaselineNumber     基线（编辑前的 current）版本
//   rollbackTo   回滚目标版本 id（仅 rollback）
// 下游失效由 supersede 免手接：以旧版本为父的 DERIVED 产物（如剖面保存）
// 在 addVersion 同一原子写里落 extra["stale"]。
//
// 线程纪律（produce-then-commit，与 FolderImportWorkflow 同口径）：差异计算/
// 校验是 domain 纯函数（WellTopsEdit::*, 无共享态）可任意线程调用；本类的一切
// mutator（commit*/rollbackTo）只准 GUI 线程——单次 addVersion 即单事务，
// 落盘失败 catalog 内部原子回滚。
class WellTopsEditorWorkflow
{
public:
  WellTopsEditorWorkflow(DataCatalog *catalog, const QString &projectDir);

  struct CommitOutcome
  {
    bool ok = false;
    bool unchanged = false; // 与当前版本字节一致——如实不发版本
    QString newVersionId;
    int newVersionNumber = 0;
    WellTopsEdit::DiffSummary diff;
    QString error;
  };

  struct VersionInfo
  {
    QString id;
    int versionNumber = 0;
    QString stage;
    QString editKind;  // extra["editKind"]；RAW 导入为空
    QString editWell;  // extra["editWell"]
    bool stale = false;
  };

  // 当前版本全文件行（多井混合，文件序）。asset 未知/无版本/文件缺失 → false。
  bool loadAllRows(const QString &assetId, QVector<WellTopRecord> *rows, QString *error) const;

  // 按井过滤（DataCatalog::normalizeWellName 口径）。
  static QVector<WellTopRecord> rowsForWell(const QVector<WellTopRecord> &all,
                                            const QString &wellName);
  // 文件内井名列表（首见序）。
  static QStringList wellNamesIn(const QVector<WellTopRecord> &all);

  // 校验上下文：井实体 TD（well_head）+ AreaRules 层序词表（浅→深）。
  static WellTopsEdit::ValidationContext contextFor(const DataCatalog *catalog,
                                                    const QString &wellName);

  // 单井编辑提交：newRows = 该井的编辑表行（其余井原样保留——旧文件中该井
  // 行块的位置由首行锚定替换）。editKind 缺省 "edit"；导入融合的保存传
  // "merge"（溯源进版本 extra）。
  CommitOutcome commitWellRows(const QString &assetId, const QString &wellName,
                               const QVector<WellTopRecord> &newRows, const QString &note,
                               const QString &editKind = QString());

  // 整文件提交（批量修正已在调用方应用到 allRows 后）：editKind 记
  // batch-rename / batch-shift / batch-delete 等。
  CommitOutcome commitAllRows(const QString &assetId, const QVector<WellTopRecord> &newAllRows,
                              const QString &editKind, const QString &note);

  // 回滚 = 目标旧版本文件字节发新版本（currentVersion 指新号即生效）。
  CommitOutcome rollbackTo(const QString &assetId, const QString &targetVersionId);

  // 从外部 DC.dat 构造该井合并行集（不落库；取舍由 UI 层收集后走 commitWellRows）。
  bool loadMergeRows(const QString &assetId, const QString &wellName, const QString &externalPath,
                     QVector<WellTopsEdit::MergeRow> *rows, QString *error) const;

  QVector<VersionInfo> versionHistory(const QString &assetId) const;

  // 批量提交前的全文件校验面：逐井跑校验器，返回错误级消息（井名 + 行级
  // 描述；警告不拦——批量操作按井量大，警告留给编辑器逐井查看）。
  static QStringList validateAllWells(const DataCatalog *catalog,
                                      const QVector<WellTopRecord> &allRows);

private:
  CommitOutcome commitBytes(const QString &assetId, const QByteArray &bytes,
                            const QString &editKind, const QString &wellName,
                            const WellTopsEdit::DiffSummary &diff, const QString &note,
                            const QString &rollbackTarget);

  DataCatalog *m_catalog = nullptr;
  QString m_projectDir;
};
