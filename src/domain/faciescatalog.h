// 层：数据
#pragma once
#include <QVariantList>
#include <QVariantMap>
namespace FaciesCatalog {
QVariantList library();
QVariantList defaults();
QVariantMap find(const QVariantList &schema, const QVariant &code);
QVariantMap attributes(const QVariantList &schema, const QVariant &code);
QString resourcePath(const QString &texture);
} // namespace FaciesCatalog
