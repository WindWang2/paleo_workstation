// 层：数据
#pragma once
#include <QVariantList>
#include <QVariantMap>
namespace PaleoMapFacies
{
// Returns a frozen project-compatible schema and a code for each input class.
// Partial paths may only be completed from an unambiguous declared/template leaf.
QVariantMap resolve(const QVariantList &project, const QVariantList &classes, QString *error);
} // namespace PaleoMapFacies
