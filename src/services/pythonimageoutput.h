// 层：数据
#pragma once
#include "pythonnumericoutput.h"

namespace paleo::python {
inline constexpr qint64 ImageOutputPngBytes = 4 * 1024 * 1024;
QString validateImageOutputDeclaration(const QJsonObject &declaration);
// Freeze this host identity with a new compute plan. Existing authenticated PNG
// publication does not call an encoder again or depend on its current version.
QJsonObject pngEncoderIdentity();
// Conservative host staging reservation, separate from child RSS. -1 means the
// declarations/frame bound are invalid. Callers may pass the 12-MiB frame ceiling
// before starting a child; validation repeats it for the actual captured frame.
qint64 imageOutputHostReservation(qint64 frameBytes, const QJsonObject &tableDeclaration,
                                 const QJsonObject &imageDeclaration);
struct ImageOutputValidation {
  QString code = "python_output_invalid";
  QByteArray report, csv, png;
  QJsonObject tableMetadata, imageMetadata, summary;
  qint64 hostReservedBytes = 0;
  bool ok() const { return code == QLatin1String("ok"); }
};
// Strict frame.v3 only: REPORT + TABLE + raw IMAGE. The host alone writes CSV
// and encodes PNG; no child filenames/MIME/PNG decoding or partial-port delivery.
ImageOutputValidation validateImageOutput(const QByteArray &frame,
    const QJsonObject &tableDeclaration, const QJsonObject &imageDeclaration,
    const QJsonObject &frozenEncoder, const ExecutionPolicy &policy,
    const GeneratedReportValidator &validateReport, const std::function<bool()> &cancelled = {});
}
