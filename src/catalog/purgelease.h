// 层：数据
#pragma once
#include <QString>
#include <memory>

// Keeps catalog writers/project reopen out of the asynchronous physical purge
// phase. Value-only workers own the lease; release occurs even if UI closes.
class CatalogPurgeLease final {
public:
  static std::shared_ptr<CatalogPurgeLease> acquire(const QString &catalogPath);
  static bool isHeld(const QString &catalogPath);
  ~CatalogPurgeLease();
private:
  explicit CatalogPurgeLease(QString key) : m_key(std::move(key)) {}
  QString m_key;
};
