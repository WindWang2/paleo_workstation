// 层：功能
// workflow/algorithmadapters.h — algo-atom P2：执行适配器。
// ProcessingAlgorithmAdapter 包装 QgisProcessingService::run → AlgorithmRunResult；
// NativeCppAdapter：geostat / facies / hztrack / inversion / wellsiting 类型化入口。
// 设计：docs/designs/algorithm-atom-contract.md §6 P2。
#pragma once

#include "../algorithms/faciesmapping/evidencesynthesis.h"
#include "../algorithms/geostat/cokriging.h"
#include "../algorithms/geostat/kriging.h"
#include "../algorithms/geostat/sgs.h"
#include "../algorithms/horizontrack.h"
#include "../algorithms/inversion/bandlimit.h"
#include "../algorithms/inversion/sparse.h"
#include "../algorithms/wellsiting/wellsiting.h"
#include "../qgis/qgisprocessingservice.h"
#include "../services/algorithmspec.h"

#include <qgscoordinatereferencesystem.h>

#include <QString>
#include <QVariantMap>
#include <array>
#include <functional>
#include <vector>

namespace paleo
{

class ProcessingAlgorithmAdapter
{
  public:
    explicit ProcessingAlgorithmAdapter( QgisProcessingService *proc );

    AlgorithmRunResult run( const AlgorithmSpec &spec, const QVariantMap &paramValues,
                            QString *error = nullptr,
                            const QgisProcessingService::ProcessingHooks &hooks = {} );

    AlgorithmRunResult runById( const QString &algorithmId, const QVariantMap &paramValues,
                                QString *error = nullptr,
                                const QgisProcessingService::ProcessingHooks &hooks = {} );

  private:
    QgisProcessingService *m_proc = nullptr;
};

struct OrdinaryKrigingInputs
{
  std::vector<paleo::geostat::Sample> samples;
  paleo::geostat::GridSpec grid;
  paleo::geostat::VariogramModel model;
  paleo::geostat::KrigingParams params;
  paleo::geostat::Control control;
  QString estimatePath;
  QString variancePath; // 空 = 不写方差
  std::array<double, 6> geoTransform{ { 0, 1, 0, 0, 0, -1 } };
  QgsCoordinateReferenceSystem crs;
  QString canonicalCrsWkt;
};

struct OrdinaryCoKrigingInputs
{
  std::vector<paleo::geostat::Sample> primary;
  std::vector<paleo::geostat::Sample> secondary;
  paleo::geostat::GridSpec grid;
  paleo::geostat::CoKrigingModel model;
  paleo::geostat::CoKrigingParams params;
  paleo::geostat::Control control;
  QString estimatePath;
  QString variancePath;
  std::array<double, 6> geoTransform{ { 0, 1, 0, 0, 0, -1 } };
  QgsCoordinateReferenceSystem crs;
  QString canonicalCrsWkt;
};

struct SgsInputs
{
  std::vector<paleo::geostat::Sample> samples;
  paleo::geostat::GridSpec grid;
  paleo::geostat::VariogramModel model;
  paleo::geostat::SgsParams params;
  paleo::geostat::Control control;
  QString meanPath;
  QString stdPath;
  bool persistRealizations = true;
  QString memberPathPrefix; // 如 tempDir/member_r → 写出 member_r000.tif …
  std::array<double, 6> geoTransform{ { 0, 1, 0, 0, 0, -1 } };
  QgsCoordinateReferenceSystem crs;
  QString canonicalCrsWkt;
};

struct TrackSectionInputs
{
  const float *section = nullptr;
  int nTraces = 0;
  int nSamples = 0;
  paleo::hztrack::SeedPoint seed;
  paleo::hztrack::TrackOptions options;
  const float *coherence = nullptr;
  std::function<bool()> cancelled;
};

struct BandlimitedInputs
{
  const float *trace = nullptr;
  int n = 0;
  double sampleIntervalMs = 1.0;
  const paleo::inversion::Wavelet *wavelet = nullptr;
  const float *lowFreq = nullptr;
  paleo::inversion::BandlimitedOptions options;
};

struct SynthesizeFaciesInputs
{
  const std::vector<paleo::faciesmapping::CandidateRegion> *regions = nullptr;
  const std::vector<paleo::faciesmapping::EvidenceSource> *sources = nullptr;
  paleo::faciesmapping::SynthesisOptions options;
  const paleo::faciesmapping::Control *control = nullptr;
};

struct SparseSpikeInputs
{
  const float *trace = nullptr;
  int n = 0;
  double sampleIntervalMs = 1.0;
  const paleo::inversion::Wavelet *wavelet = nullptr; // 必填（核要求引用）
  const float *lowFreq = nullptr;
  paleo::inversion::SparseSpikeOptions options;
};

struct DiagnoseCoverageInputs
{
  const std::vector<paleo::wellsiting::Polygon> *domain = nullptr;
  const std::vector<paleo::wellsiting::Point2> *wells = nullptr;
  paleo::wellsiting::SitingOptions options;
};

class NativeCppAdapter
{
  public:
    // QVariantMap 入口：类型化方法请用下方 run*；其余返回明确未实现。
    AlgorithmRunResult run( const AlgorithmSpec &spec, const QVariantMap &paramValues,
                            QString *error = nullptr );

    AlgorithmRunResult runOrdinaryKriging( const OrdinaryKrigingInputs &in,
                                           QString *error = nullptr );
    AlgorithmRunResult runOrdinaryCoKriging( const OrdinaryCoKrigingInputs &in,
                                             QString *error = nullptr );
    AlgorithmRunResult runSgs( const SgsInputs &in, QString *error = nullptr );

    // 返回 RunResult；核结果经 out 回传（调用方可继续 merge/映射）。
    AlgorithmRunResult runTrackSection( const TrackSectionInputs &in,
                                        paleo::hztrack::TrackResult *out = nullptr,
                                        QString *error = nullptr );

    AlgorithmRunResult runBandlimited( const BandlimitedInputs &in,
                                       paleo::inversion::BandlimitedResult *out = nullptr,
                                       QString *error = nullptr );

    AlgorithmRunResult runSynthesizeFacies( const SynthesizeFaciesInputs &in,
                                            paleo::faciesmapping::SynthesisResult *out = nullptr,
                                            QString *error = nullptr );

    AlgorithmRunResult runSparseSpike( const SparseSpikeInputs &in,
                                       paleo::inversion::SparseSpikeResult *out = nullptr,
                                       QString *error = nullptr );

    // sampleField + describeField；field 出参供 domainArea 等编排诊断。
    AlgorithmRunResult runDiagnoseCoverage( const DiagnoseCoverageInputs &in,
                                            paleo::wellsiting::CoverageReport *outReport = nullptr,
                                            paleo::wellsiting::SitingField *outField = nullptr,
                                            QString *error = nullptr );
};

} // namespace paleo
