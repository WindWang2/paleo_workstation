// 层：功能
#pragma once
#include "domain/documentsource.h"

namespace paleo::document {
// A user-confirmed, whole-file LAS assignment for the selected RAW version.
struct WellboreSourceConfirmation {
    SourceRef source;
    QString versionId, wellEntityId, boreKey, displayName;
    quint64 expectedEpoch = 0, expectedRevision = 0;
};
}
