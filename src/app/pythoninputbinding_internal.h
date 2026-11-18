// 层：组装根
#pragma once
#include <QPointer>
#include <QString>

class DataCatalog;
namespace paleo::capability { class CapabilityHost; }
namespace paleo::document { class DocumentSession; struct SourceRef; struct SourceRecord; }
namespace paleo::host { struct CallContext; }

namespace paleo::python::inputbinding {
// Host-owned checks shared by typed inputs; format-specific validation stays
// with each adapter so its versioned contract and error codes remain explicit.
struct Scope {
  QPointer<DataCatalog> catalog;
  QPointer<document::DocumentSession> document;
  QPointer<capability::CapabilityHost> host;
};
bool live(const Scope &scope);
bool samePath(const QString &left, const QString &right);
QString checkScope(const Scope &scope, const host::CallContext &expected);
QString modelRoot(const Scope &scope);
QString mappedSource(const Scope &scope, const document::SourceRef &reference,
                     const QString &model, document::SourceRecord &out,
                     const QString &mappingError);
} // namespace paleo::python::inputbinding
