// 层：数据
#pragma once
#include <QString>
#include <QVector>
#include <QVariantMap>
#include <functional>
#include "../metadata/paleoprojectstore.h"

class ConstraintStore {
public:
  struct AuditContext {
    QString actorId; // explicitly local:<operator>, no multi-user authentication
    QString reason;
    QString operationId;
    QString action; // edit/undo/redo/cancel; empty uses the actual create/update/delete
  };
  using WriteFn = std::function<PaleoProjectStore::WriteResult()>;
  using EnqueueFn = std::function<PaleoProjectStore::WriteResult(const WriteFn &)>;

  explicit ConstraintStore(const QString &gpkgPath, EnqueueFn enqueue = nullptr);
  explicit ConstraintStore(const QString &gpkgPath, PaleoProjectStore *store);

  QString gpkgPath() const { return m_gpkgPath; }
  bool setAuditContext(const AuditContext &context, QString *error = nullptr);
  void setReadOnly(bool readOnly) { m_readOnly = readOnly; }

  bool append(const QString &horizon, const QString &id, const QString &wkt,
              const QString &type, int faciesCode, QString *error = nullptr, double weight = 1.0);
  // 加法式参数。旧 append 不写这两列的值。schemaVersion 与 paramsJson 一起保存。
  bool appendExtended(const QString &horizon, const QString &id, const QString &wkt,
                      const QString &type, int faciesCode, const QString &paramsJson,
                      int schemaVersion, QString *error = nullptr, double weight = 1.0);
  // 文件不存在时失败，不创建数据库。失败回滚，不改已有要素。
  bool updateParameters(const QString &id, const QString &paramsJson, int schemaVersion,
                        const QString &semanticType, QString *error = nullptr);
  QVector<QVariantMap> load(const QString &horizon = QString(), bool includeAudit = false) const;
  // Newest first, max 200 per page. Deleted stable IDs remain queryable.
  QVector<QVariantMap> history(const QString &id, int limit = 50, int offset = 0,
                               QString *error = nullptr) const;
  QVector<QVariantMap> historyForHorizon(const QString &horizon, int limit = 200,
                                       int offset = 0, QString *error = nullptr) const;
  // 将该层位的追加审计复制进另一个 GPKG，供约束快照带走删除历史。
  // 不修改来源；没有审计时成功且不创建空表。
  bool copyAudit(const QString &destinationGpkg, const QString &horizon,
                 QString *error = nullptr, int *copied = nullptr) const;
  // 原子替换单一层位的完整编辑快照，保留 FID 与未编辑字段；拒绝跨层位
  // 及重复身份。经同一写队列；失败时整批回滚，不创建缺失资产。
  bool replaceHorizon(const QString &horizon, const QVector<QVariantMap> &rows,
                      QString *error = nullptr);
  bool remove(const QString &id, QString *error = nullptr);

private:
  bool appendInternal(const QString &horizon, const QString &id, const QString &wkt,
                      const QString &type, int faciesCode, const QString *paramsJson,
                      int schemaVersion, QString *error, double weight);

private:
  QString m_gpkgPath;
  EnqueueFn m_enqueue;
  AuditContext m_audit;
  bool m_readOnly = false;
};
