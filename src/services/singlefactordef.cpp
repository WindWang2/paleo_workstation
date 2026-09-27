#include "singlefactordef.h"

// 层：数据
// m2 基座桩：注册表词表由单因素页任务（B）按 PALEO_QGIS_PLAN.md §10 实现，
// 此处先保证数据层文件与链接就位（CMake 已注册）。
QVector<SingleFactorDefinition> SingleFactorRegistry::builtins()
{
  return {};
}

SingleFactorDefinition SingleFactorRegistry::byId(const QString &factorId, bool *ok)
{
  if (ok)
    *ok = false;
  return SingleFactorDefinition();
}

QString SingleFactorRegistry::titleFor(const QString &factorId)
{
  return factorId;
}
