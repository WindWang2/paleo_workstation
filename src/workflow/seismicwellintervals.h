// 层：功能
#pragma once
#include <QVariantList>
#include <functional>
bool readSeismicWellInterval(QVariantMap &source,const QVariantList &schema,
                            const std::function<bool()> &cancel, QString *error);
