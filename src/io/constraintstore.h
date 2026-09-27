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
  QVector<QVariantMap> load(const QString &horizon = QString()) const;
  bool remove(const QString &id, QString *error = nullptr);

private:
  QString m_gpkgPath;
  EnqueueFn m_enqueue;
};
