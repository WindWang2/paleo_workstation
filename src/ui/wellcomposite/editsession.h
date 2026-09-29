// 层：视图
#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

#include <memory>

#include "domain/wellcompositemodel.h"
#include "editstack.h"
#include "wellcompositestore.h"

// ui/wellcomposite/editsession — D3.3/D3.9/D3.10/D3.11/D3.15 编辑会话
//
// 会话持井数据工作副本（源数据永不写）：全部编辑走 push 命令（undo 栈深 50）、
// 审计日志记录操作摘要、源 mtime 变更检测（D3.10）、只读降级（D3.15）、
// 派生版本文档构建（D3.3：XML DERIVED 版本 + manifest 风格审计摘要——落盘由
// 壳/测试经 io 序列化，视图只产出文档与摘要）。

namespace WellComposite
{

class EditSession : public QObject
{
  Q_OBJECT

public:
  explicit EditSession(ComprehensiveWellData workingCopy, QObject *parent = nullptr);

  // 工作文档（编辑后的最新态）
  const ComprehensiveWellData &document() const { return m_doc; }
  void resetDocument(const ComprehensiveWellData &doc); // 重载/放弃编辑

  EditStack *stack() const { return m_stack.get(); }

  // D3.9 脏状态转发
  bool isDirty() const { return m_stack->isDirty(); }
  void markSaved();

  // ---- D3.15 只读降级 ----
  void setReadOnly(bool readOnly, const QString &reason = QString());
  bool isReadOnly() const { return m_readOnly; }
  QString readOnlyReason() const { return m_readOnlyReason; }

  // ---- D3.10 源数据变更冲突 ----
  void setSourceMtime(qint64 mtimeMs);
  qint64 sourceMtime() const { return m_sourceMtimeMs; }
  bool sourceChanged(qint64 currentMtimeMs) const;
  // D3.10 冲突广播（面板接 sourceConflictDetected 弹重载/分叉选择）
  void checkSourceConflict(qint64 currentMtimeMs);

  // ---- D3.11 编辑审计 ----
  void appendAudit(const QString &operation, const QString &detail);
  // 审计摘要（manifest 风格：操作计数 + 逐条「时间 | 操作 | 细节」）
  QString auditSummary() const;
  QStringList auditLines() const { return m_audit; }

  // ---- D3.1/D3.2 标志层（TOPs）编辑 ----
  bool moveMarker(const QString &name, double newDepth);
  bool renameMarker(const QString &oldName, const QString &newName);
  bool insertMarker(const QString &name, double depth);
  bool removeMarker(const QString &name);

  // ---- D3.4 岩性区间编辑 ----
  bool editLithoInterval(int index, const LithologyInterval &interval);
  bool removeLithoInterval(int index);
  bool appendLithoInterval(const LithologyInterval &interval);

  // ---- D3.5 相区间编辑 ----
  bool editFaciesInterval(int index, const FaciesInterval &interval);

  // ---- D3.6 地层指派（显式，程序不猜） ----
  bool applyStratAssignments(const QList<StratAssignment> &assignments);

  // ---- D3.13 合并/拆分 ----
  int mergeAdjacentLithoIntervals();            // 返回合并对数
  bool splitLithoInterval(int index, double atDepth); // 在界线处一分为二

  // ---- D3.7 批量导入应用（预校验后） ----
  struct BatchApplyResult
  {
    int inserted = 0;
    int renamed = 0;
    int moved = 0;
    QStringList rejected; // 被拒行（行内容）
  };
  BatchApplyResult applyBatchMarkers(const QVector<QPair<QString, double>> &rows); // (name, depth)

  // ---- D3.3 派生版本文档 ----
  // 审计摘要写入文档侧（manifest 等价物：井名 + 源标识 + 操作历史）；返回
  // 携带审计字段的派生文档（源文档 + 编辑后的数据 + audit 摘要）。
  ComprehensiveWellData buildDerivedDocument() const;
  QString buildManifestStyleSummary() const;

  // 编辑操作通用守卫（只读拒绝时返回 false）
  bool editable() const;

signals:
  void documentChanged();      // 任何编辑/undo/redo 后（面板据此重同步道）
  void readOnlyChanged(bool readOnly);
  void sourceConflictDetected(qint64 currentMtime);

private:
  void pushCmd(const QString &text, std::function<void()> undoFn, std::function<void()> redoFn,
               const QString &auditOp, const QString &auditDetail);

  ComprehensiveWellData m_doc;
  std::unique_ptr<EditStack> m_stack;
  QStringList m_audit; // 逐条「ISO 时间 | 操作 | 细节」
  bool m_readOnly = false;
  QString m_readOnlyReason;
  qint64 m_sourceMtimeMs = 0;
  QString m_sourceId; // 井名（派生文档来源标识）
};

} // namespace WellComposite
