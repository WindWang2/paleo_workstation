// 层：数据
#pragma once
#include <QString>
#include <QVector>
#include <QVariantMap>
#include <functional>
#include "../metadata/paleoprojectstore.h"

class ConstraintStore {
public:
  using WriteFn = std::function<PaleoProjectStore::WriteResult()>;
  using EnqueueFn = std::function<PaleoProjectStore::WriteResult(const WriteFn &)>;

  explicit ConstraintStore(const QString &gpkgPath, EnqueueFn enqueue = nullptr);
  explicit ConstraintStore(const QString &gpkgPath, PaleoProjectStore *store);

  QString gpkgPath() const { return m_gpkgPath; }

  bool append(const QString &horizon, const QString &id, const QString &wkt,
              const QString &type, int faciesCode, QString *error = nullptr, double weight = 1.0);
  // 加法式参数。旧 append 不写这两列的值。schemaVersion 与 paramsJson 一起保存。
  bool appendExtended(const QString &horizon, const QString &id, const QString &wkt,
                      const QString &type, int faciesCode, const QString &paramsJson,
                      int schemaVersion, QString *error = nullptr, double weight = 1.0);
  // 文件不存在时失败，不创建数据库。失败回滚，不改已有要素。
  bool updateParameters(const QString &id, const QString &paramsJson, int schemaVersion,
                        const QString &semanticType, QString *error = nullptr);
  QVector<QVariantMap> load(const QString &horizon = QString()) const;
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
};
