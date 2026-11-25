// 层：视图
#pragma once
#include <QString>
class QVBoxLayout;
class QWidget;
namespace paleo::document { class DocumentSession; }
namespace paleo::preview {
void addWellboreSourceButton(document::DocumentSession *session, const QString &versionId,
                             const QString &wellId, const QString &wellName, QWidget *host, QVBoxLayout *layout);
}
