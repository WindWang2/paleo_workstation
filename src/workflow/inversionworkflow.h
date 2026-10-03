// 层：功能
#pragma once

#include "../algorithms/inversion/lowfreq.h"
#include "../algorithms/inversion/volume.h"
#include "../algorithms/inversion/wavelet.h"
#include "derivedassets.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <functional>
#include <vector>

// workflow/inversionworkflow — 确定性叠后反演编排（方向 22）。
// 三段式（现状 publishLocalDirectionJob 式 API，方向 20 JobRunner 未落地不
// 依赖）：prepare 在界面线程（校验/快照/低频模型/世代号 +1），compute 在
// 任务线程（道分块并行 ≤4、协作取消、临时文件），publish 回 catalog 所属
// 线程（DERIVED 登记）。世代号守卫由调用方自管（dock 现成世代号丢弃陈旧
// 结果模式）；本类无状态，可值语义使用。
//
// 诚实口径：产物 extra 必带 lowCutHz 与频段语义（低频模型 0–lowCut + 地震
// 带限），不标「高分辨率」；井阻抗曲线 TWT 化按垂直井语义（方向 19 递延）。

namespace paleo::inv {

// ---- 子波提取作业 ------------------------------------------------------------
struct WaveletJobRequest {
    QString wellId;
    QString displayName;
    double wellX = 0.0;
    double wellY = 0.0;
    // 井曲线（MD 域，米；AC µs/m、DEN g/cm³）——调用方从 LAS/ProjectData 采好。
    std::vector<double> acDepthsM;
    std::vector<float> acUsPerM;
    std::vector<double> denDepthsM;
    std::vector<float> denValues;
    // 时深表（空 → 拒绝：反演是时间域运算，缺时深不能装懂）。
    std::vector<double> tdDepthM;
    std::vector<double> tdTimeMs;
    QString seismicPath;          // 井旁道来源（SEG-Y）
    double waveletLengthMs = 128.0;
    QString parentVersionId;      // 可空（地震体版本，父谱系）
    QString sourceUri;
};

class InversionWorkflow {
public:
    struct WaveletJob {
        bool prepared = false;
        bool ok = false;
        QString error;
        WaveletJobRequest req;
        std::vector<paleo::inversion::ReflSpike> spikes; // prepare 阶段算好（界面线程）
        paleo::inversion::Wavelet wavelet;               // compute 阶段填
        double fitCorrelation = 0.0;
        QString nearestTraceNote;                        // QC：井旁道定位说明
    };

    bool prepareWaveletJob(const WaveletJobRequest &req, WaveletJob *job, QString *error = nullptr) const;
    bool computeWaveletJob(WaveletJob *job,
                           const std::function<bool()> &cancelled = {},
                           const std::function<void(double)> &progress = {}) const;
    // 返回 DERIVED 版本 id；空串 = 失败（*error 有话）。
    QString publishWaveletJob(const WaveletJob &job, DerivedAssetRegistrar &registrar,
                              QString *error = nullptr) const;

    // ---- 体反演作业 ----------------------------------------------------------
    // 低频井曲线：TWT 域散样（调用方给深度域曲线时先用 TimeDepthModel 转好）。
    struct LowFreqWellInput {
        QString wellId;
        double x = 0.0;
        double y = 0.0;
        std::vector<double> twtMs;
        std::vector<float> impedance;
    };

    struct InversionJobRequest {
        QString seismicPath;
        QString waveletPath;        // wavelet.json（DERIVED 资产文件路径）
        paleo::inversion::Wavelet wavelet; // 直接给子波时优先于 waveletPath
        bool hasWaveletValue = false;
        QString method;             // "bandlimited" | "sparse"
        double lowCutHz = 8.0;
        double lambda = 0.0;        // sparse；<=0 → 自动
        int maxIterations = 200;    // sparse
        int maxThreads = 0;         // 道并行上限；0 = auto（min(4, 硬件)），>0 夹 [1,64]
        double waveletLengthMs = 128.0; // 兜底理论 Ricker 用（无子波文件时拒绝，不留暗兜底）
        std::vector<LowFreqWellInput> lowFreqWells; // 可空 → 无低频（带限口径，如实记）
        std::vector<paleo::inversion::HorizonTwtGrid> horizons; // 可空 → 全局趋势回退
        QString displayName;
        QStringList parentPaths;    // 父版本谱系反查（绝对/工程相对路径）
        QString sourceUri;
    };

    struct InversionJob {
        bool prepared = false;
        bool ok = false;
        QString error;
        InversionJobRequest req;
        paleo::inversion::Wavelet wavelet;
        paleo::inversion::LowFreqModelResult lowFreq; // prepare 阶段建好
        bool hasLowFreq = false;
        // 体网格（prepare 从 SEG-Y 读）
        int nIl = 0;
        int nXl = 0;
        int nS = 0;
        double t0Ms = 0.0;
        double dtMs = 2.0;
        qint32 inlineMin = 0;
        qint32 inlineStep = 1;
        qint32 xlineMin = 0;
        qint32 xlineStep = 1;
        // compute 聚合 QC
        int processedTraces = 0;
        int failedTraces = 0;
        double meanResidualEnergyRatio = 0.0; // sparse
        double meanLowFreqVarianceFraction = 0.0; // bandlimited
        QString tempVolumePath;
    };

    bool prepareInversionJob(const InversionJobRequest &req, InversionJob *job,
                             QString *error = nullptr) const;
    bool computeInversionJob(InversionJob *job,
                             const std::function<bool()> &cancelled = {},
                             const std::function<void(double)> &progress = {}) const;
    struct PublishResult {
        bool ok = false;
        QString error;
        QString versionId;
        QString relativePath;
        QJsonObject volumeHeader; // 落盘的体头（QC/复现）
    };
    PublishResult publishInversionJob(const InversionJob &job, DerivedAssetRegistrar &registrar,
                                      QString *error = nullptr) const;
};

} // namespace paleo::inv
