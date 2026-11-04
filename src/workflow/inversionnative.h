// 层：功能
#pragma once
#include "inversionworkflow.h"
namespace paleo::inv {
QVariantMap trajectoryEvidence(const std::vector<seismic::WellTrajSample>& path,const QString& source,const QString& reference);
QString curveEvidenceHash(const std::vector<double>& axis,const std::vector<float>& values);
bool computeRegisteredWavelet(InversionWorkflow::WaveletJob* job,
    const std::function<bool()>& cancelled,const std::function<void(double)>& progress);
bool prepareRegisteredInversion(const InversionWorkflow::InversionJobRequest& request,
    const paleo::inversion::Wavelet& wavelet,InversionWorkflow::InversionJob* job,QString* error);
bool computeRegisteredInversion(InversionWorkflow::InversionJob* job,
    const std::function<bool()>& cancelled,const std::function<void(double)>& progress);
}
