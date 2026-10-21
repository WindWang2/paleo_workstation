// 层：功能
// workflow/nativecppadapter.h — algo-atom P2：NativeCppAdapter 声明（入参/出参仅前置声明；
// 定义按域拆至 nativeinputs_*.h，调用方只带入所需核头）。
#pragma once

#include "../services/algorithmspec.h"

#include <QString>
#include <QVariantMap>

namespace paleo
{

namespace hztrack { struct TrackResult; struct PropagateResult; }
namespace inversion { struct BandlimitedResult; struct SparseSpikeResult; }
namespace faciesmapping { struct SynthesisResult; }
namespace wellsiting { struct CoverageReport; struct SitingField; }

struct OrdinaryKrigingInputs;
struct OrdinaryCoKrigingInputs;
struct SgsInputs;
struct TrackSectionInputs;
struct BandlimitedInputs;
struct SynthesizeFaciesInputs;
struct SparseSpikeInputs;
struct DiagnoseCoverageInputs;
struct PropagateVolumeInputs;

// 类型化入参定义见 nativeinputs_{geostat,hztrack,inversion,facies,wellsiting}.h。

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

    AlgorithmRunResult runPropagateVolume( const PropagateVolumeInputs &in,
                                           paleo::hztrack::PropagateResult *out = nullptr,
                                           QString *error = nullptr );
};

} // namespace paleo
