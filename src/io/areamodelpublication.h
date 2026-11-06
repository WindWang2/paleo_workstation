// 层：数据
#pragma once
namespace paleo::data { struct ModelChangeHead; }
#include <QString>
#include <functional>
class DataCatalog;

namespace paleo::area_model {
// The prepared root is an operation-owned .paleo-area-* project, never a source directory.
// Success means the model is published; its backup remains until projection completes.
bool publishPreparedModel(const QString &project, const QString &stagingProject,
                          const paleo::data::ModelChangeHead &expected, QString *error);
bool hasPendingModelPublication(const QString &project);
// The callback must durably project model changes, including its Catalog watermark.
// Recovery never reparses an original source and never overwrites a conflicting model.
bool recoverModelPublication(const QString &project,
    const std::function<bool(QString *)> &projectChanges, QString *error);
bool recoverModelPublication(DataCatalog &catalog, QString *error);
}
