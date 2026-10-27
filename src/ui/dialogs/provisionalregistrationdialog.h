// 层：视图
#pragma once

#include <QString>
#include <QVariantMap>

class PreviewDocService;
class QWidget;

namespace PaleoProvisionalRegistration
{
bool prompt(QWidget *parent, PreviewDocService *document, const QString &path,
            QVariantMap *parameters, QString *error);
}
