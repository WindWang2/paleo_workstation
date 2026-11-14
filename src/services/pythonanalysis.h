// 层：数据
#pragma once
#include "domain/documentsource.h"
#include "pythoninputbundle.h"
#include "pythonprocess.h"
#include <paleo_data/model_changes.h>
#include <QVector>

namespace paleo::python {
enum class AnalysisKind { WellQuality, AttributeCorrelation };
inline constexpr int AnalysisMaxSamples=8192, AnalysisMaxChannels=16;
struct AnalysisCurveSelection {
  document::SourceRecord source; // Host-resolved source/model location, never a tool path.
  QString sourceVersionId;
  qlonglong wellId=0, logSetId=0, curveId=0, firstSample=0;
  int sampleCount=0;
  bool processed=true;
  QString unit, depthType, depthUnit, depthReference;
};
struct AnalysisReadPlan {
  AnalysisKind kind=AnalysisKind::WellQuality;
  QString projectDir, modelDirectory, documentId, callerScope;
  data::ModelChangeHead expectedHead;
  QVector<AnalysisCurveSelection> curves;
  QString alignment="exact_shared_axis_v1", sampling="all_selected_window_v1";
  quint32 seed=0;
  qint64 readByteBudget=8*1024*1024;
};
struct AnalysisReadMetrics {
  qint64 payloadBytes=0, legacyBytes=0, axisBlocks=0, valueBlocks=0, windows=0;
  qint64 metadataRows=0, metadataTextBytes=0;
  QJsonObject json() const;
};
struct AnalysisMaterialization {
  QString code;
  InputBundle bundle;
  QJsonObject metadata;
  AnalysisReadMetrics io;
  bool ok() const { return code==QLatin1String("ok"); }
};
struct AnalysisProcessInput {
  QString code;
  QByteArray bytes;
  QJsonObject metadata;
  bool ok() const { return code==QLatin1String("ok"); }
};
QString analysisKindId(AnalysisKind kind);
QString analysisTemplateSha256(AnalysisKind kind);
// Worker-only; the caller claims the shared process permit before any SDK I/O
// or input allocation and passes the same permit to runTrustedProcess later.
AnalysisMaterialization materializeAnalysis(const AnalysisReadPlan &plan,const QByteArray &trustedTemplate,
    const ExecutionPolicy &policy,const std::shared_ptr<ExecutionPermit> &permit,
    const std::function<bool()> &cancelled={});
// Rechecks the exact consumed bytes, then frames metadata + binary on stdin.
// Templates never open host paths, and a file swap cannot substitute unchecked arrays.
AnalysisProcessInput loadAnalysisProcessInput(const QString &projectDir,const QJsonObject &reference,
    const QString &expectedDocumentId,const QString &expectedCallerScope,
    const ExecutionPolicy &policy,const std::shared_ptr<ExecutionPermit> &permit,
    const std::function<bool()> &cancelled={});
} // namespace paleo::python
