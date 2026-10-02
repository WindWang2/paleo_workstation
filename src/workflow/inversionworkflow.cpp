// 层：功能
#include "inversionworkflow.h"

#include "../domain/seismic/timedepthmodel.h"
#include "../io/segyreader.h"
#include "../services/seismictaskservice.h"

#include "../algorithms/inversion/bandlimit.h"
#include "../algorithms/inversion/sparse.h"

#include <QDir>
#include <QFile>
#include <QUuid>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <thread>

namespace paleo::inv {
namespace {

using paleo::inversion::LowFreqModelInput;
using paleo::inversion::LowFreqWellTrace;
using paleo::inversion::WaveletExtractOptions;

bool setError(QString *error, const QString &text)
{
    if (error)
        *error = text;
    return false;
}

// 井曲线 → 层界面反射系数尖峰（TWT）。与 computeSyntheticSeismogram 同口径：
// 公共深度轴并集、Z=ρ·(1e6/DT)、r=(Z2−Z1)/(Z2+Z1)、界面 TWT 取顶底深度中点。
std::vector<paleo::inversion::ReflSpike> reflectivitySpikes(
    const std::vector<double> &acDepthsM, const std::vector<float> &acUsPerM,
    const std::vector<double> &denDepthsM, const std::vector<float> &denValues,
    const seismic::TimeDepthModel &td)
{
    std::vector<double> depths = acDepthsM;
    depths.insert(depths.end(), denDepthsM.begin(), denDepthsM.end());
    std::sort(depths.begin(), depths.end());
    depths.erase(std::unique(depths.begin(), depths.end()), depths.end());
    auto interp = [](const std::vector<double> &xs, const std::vector<float> &ys,
                     double x) -> double {
        if (xs.size() != ys.size() || xs.empty())
            return std::numeric_limits<double>::quiet_NaN();
        if (x <= xs.front())
            return ys.front();
        if (x >= xs.back())
            return ys.back();
        const auto it = std::upper_bound(xs.begin(), xs.end(), x);
        const size_t hi = size_t(it - xs.begin());
        const size_t lo = hi - 1;
        const double f = (x - xs[lo]) / (xs[hi] - xs[lo]);
        return double(ys[lo]) + f * (double(ys[hi]) - double(ys[lo]));
    };
    std::vector<paleo::inversion::ReflSpike> spikes;
    double zPrev = std::numeric_limits<double>::quiet_NaN();
    double dPrev = 0.0;
    for (double d : depths) {
        const double dt = interp(acDepthsM, acUsPerM, d);
        const double den = interp(denDepthsM, denValues, d);
        if (!(dt > 1e-6) || !(den > 0.0))
            continue;
        const double z = den * (1e6 / dt);
        if (std::isfinite(zPrev)) {
            const double denom = z + zPrev;
            const double r = std::fabs(denom) > 1e-9 ? (z - zPrev) / denom : 0.0;
            const double twt = td.DepthToTwtMs(0.5 * (d + dPrev));
            if (std::isfinite(twt) && std::fabs(r) > 1e-6)
                spikes.push_back({twt, float(r)});
        }
        zPrev = z;
        dPrev = d;
    }
    return spikes;
}


// 采样道头拟合测网仿射 x = ax + bx·il + cx·xl / y = ay + by·il + cy·xl。
// 不用 SegyGeometry 角点——cornerSlot 按中点比较分槽，奇数线数测网的中线
// 道会覆盖真角点（实测 3×3 的 slot3 = (inlMax, xlineMid)），方向 21 在飞
// 不改 segyreader，这里自拟合。规则测网下最小二乘即精确解。
struct SurveyAffine
{
    bool ok = false;
    QString reason;
    double ax = 0, bx = 0, cx = 0;
    double ay = 0, by = 0, cy = 0;
    double inlineMin = 0, inlineMax = 0;
    double xlineMin = 0, xlineMax = 0;
};

SurveyAffine fitSurveyAffine(const QString &path, qint64 traceCount)
{
    SurveyAffine f;
    const int samples = int(std::min<qint64>(48, std::max<qint64>(4, traceCount)));
    std::vector<double> il, xl, x, y;
    for (int k = 0; k < samples; ++k) {
        const qint64 idx = traceCount > 1
                               ? qint64(std::llround(double(k) * double(traceCount - 1) /
                                                     double(samples - 1)))
                               : 0;
        const seismic::SeismicTraceHeaderInfo h =
            seismic::SeismicTaskService::readTraceHeader(path, int(idx));
        if (!h.ok || !(std::fabs(h.cdpX) > 0.0) || !(std::fabs(h.cdpY) > 0.0))
            continue;
        il.push_back(h.inlineNo);
        xl.push_back(h.xlineNo);
        x.push_back(h.cdpX);
        y.push_back(h.cdpY);
    }
    if (int(il.size()) < 4)
    {
        f.reason = QStringLiteral("可用带坐标道头不足 4 个（无法拟合测网仿射）");
        return f;
    }
    // 3 参数最小二乘（法方程 3×3 高斯消元）；il/xl 中心化改病态。
    double mIl = 0, mXl = 0;
    for (size_t i = 0; i < il.size(); ++i)
    {
        mIl += il[i];
        mXl += xl[i];
    }
    mIl /= double(il.size());
    mXl /= double(xl.size());
    auto solve3 = [](std::vector<double> a, std::vector<double> b, double *out) {
        for (int col = 0; col < 3; ++col)
        {
            int piv = col;
            for (int r = col + 1; r < 3; ++r)
                if (std::fabs(a[size_t(r * 3 + col)]) > std::fabs(a[size_t(piv * 3 + col)]))
                    piv = r;
            if (std::fabs(a[size_t(piv * 3 + col)]) < 1e-12)
                return false;
            if (piv != col)
                for (int c = 0; c < 3; ++c)
                    std::swap(a[size_t(piv * 3 + c)], a[size_t(col * 3 + c)]);
            std::swap(b[size_t(piv)], b[size_t(col)]);
            for (int r = col + 1; r < 3; ++r)
            {
                const double fac = a[size_t(r * 3 + col)] / a[size_t(col * 3 + col)];
                for (int c = col; c < 3; ++c)
                    a[size_t(r * 3 + c)] -= fac * a[size_t(col * 3 + c)];
                b[size_t(r)] -= fac * b[size_t(col)];
            }
        }
        for (int r = 2; r >= 0; --r)
        {
            double acc = b[size_t(r)];
            for (int c = r + 1; c < 3; ++c)
                acc -= a[size_t(r * 3 + c)] * out[c];
            out[r] = acc / a[size_t(r * 3 + r)];
        }
        return true;
    };
    double s11 = 0, s12 = 0, s22 = 0, sx1 = 0, sx2 = 0, sy1 = 0, sy2 = 0, mx = 0, my = 0;
    for (size_t i = 0; i < il.size(); ++i)
    {
        il[i] -= mIl;
        xl[i] -= mXl;
        s11 += il[i] * il[i];
        s12 += il[i] * xl[i];
        s22 += xl[i] * xl[i];
        mx += x[i];
        my += y[i];
    }
    mx /= double(x.size());
    my /= double(y.size());
    for (size_t i = 0; i < il.size(); ++i)
    {
        sx1 += il[i] * (x[i] - mx);
        sx2 += xl[i] * (x[i] - mx);
        sy1 += il[i] * (y[i] - my);
        sy2 += xl[i] * (y[i] - my);
    }
    const std::vector<double> nmat = {s11, s12, 0, s12, s22, 0, 0, 0, 1};
    double xcoef[3] = {0, 0, 0};
    double ycoef[3] = {0, 0, 0};
    if (!solve3(nmat, {sx1, sx2, 0}, xcoef) || !solve3(nmat, {sy1, sy2, 0}, ycoef))
    {
        f.reason = QStringLiteral("测网仿射拟合奇异（inline/crossline 线号共线）");
        return f;
    }
    f.bx = xcoef[0];
    f.cx = xcoef[1];
    f.by = ycoef[0];
    f.cy = ycoef[1];
    f.ax = mx - f.bx * mIl - f.cx * mXl;
    f.ay = my - f.by * mIl - f.cy * mXl;
    f.inlineMin = *std::min_element(il.begin(), il.end()) + mIl;
    f.inlineMax = *std::max_element(il.begin(), il.end()) + mIl;
    f.xlineMin = *std::min_element(xl.begin(), xl.end()) + mXl;
    f.xlineMax = *std::max_element(xl.begin(), xl.end()) + mXl;
    f.ok = true;
    return f;
}

// TWT 散样曲线重采样到体时间网格（线性插值，窗外 NaN）。
std::vector<float> resampleToGrid(const std::vector<double> &twt, const std::vector<float> &imp,
                                  double t0Ms, double dtMs, int nS)
{
    std::vector<float> out(size_t(nS), std::numeric_limits<float>::quiet_NaN());
    if (twt.size() != imp.size() || twt.size() < 2 || !std::is_sorted(twt.begin(), twt.end()))
        return out;
    for (int i = 0; i < nS; ++i) {
        const double t = t0Ms + double(i) * dtMs;
        if (t < twt.front() || t > twt.back())
            continue;
        const auto it = std::upper_bound(twt.begin(), twt.end(), t);
        if (it == twt.begin() || it == twt.end())
            continue;
        const size_t hi = size_t(it - twt.begin());
        const size_t lo = hi - 1;
        const double f = (t - twt[lo]) / (twt[hi] - twt[lo]);
        out[size_t(i)] = float(double(imp[lo]) + f * (double(imp[hi]) - double(imp[lo])));
    }
    return out;
}

} // namespace

// ---- 子波作业 ----------------------------------------------------------------

bool InversionWorkflow::prepareWaveletJob(const WaveletJobRequest &req, WaveletJob *job,
                                          QString *error) const
{
    if (!job)
        return setError(error, QStringLiteral("job 为空"));
    job->prepared = false;
    job->ok = false;
    job->error.clear();
    job->req = req;
    if (req.acDepthsM.size() != req.acUsPerM.size() || req.acDepthsM.size() < 2 ||
        req.denDepthsM.size() != req.denValues.size() || req.denDepthsM.size() < 2)
        return setError(error, QStringLiteral("AC/DEN 曲线无效（深度/值等长且 ≥2 采样）"));
    if (req.tdDepthM.size() != req.tdTimeMs.size() || req.tdDepthM.size() < 2)
        return setError(error, QStringLiteral("时深表缺失或无效（反演是时间域运算，缺时深不装懂）"));
    if (req.seismicPath.isEmpty() || !QFile::exists(req.seismicPath))
        return setError(error, QStringLiteral("地震体路径无效: %1").arg(req.seismicPath));
    if (!(req.waveletLengthMs >= 16.0))
        return setError(error, QStringLiteral("子波长度过短（≥16 ms）"));

    seismic::TimeDepthModel td;
    std::vector<seismic::TdPoint> points;
    points.reserve(req.tdDepthM.size());
    for (size_t i = 0; i < req.tdDepthM.size(); ++i)
        points.push_back({req.tdDepthM[i], req.tdTimeMs[i]});
    td.setPoints(points);

    job->spikes = reflectivitySpikes(req.acDepthsM, req.acUsPerM, req.denDepthsM, req.denValues, td);
    if (job->spikes.size() < 2)
        return setError(error, QStringLiteral("反射系数尖峰不足 2 个（曲线/时深配合无法定子波）"));
    job->prepared = true;
    return true;
}

bool InversionWorkflow::computeWaveletJob(WaveletJob *job, const std::function<bool()> &cancelled,
                                          const std::function<void(double)> &progress) const
{
    if (!job || !job->prepared)
        return setError(&job->error, QStringLiteral("子波作业未 prepare"));
    if (cancelled && cancelled())
        return setError(&job->error, QStringLiteral("已取消"));
    if (progress)
        progress(0.1);

    SegyReader reader;
    QString openErr;
    if (!reader.open(job->req.seismicPath, &openErr))
        return setError(&job->error, QStringLiteral("打开地震体失败: %1").arg(openErr));
    if (progress)
        progress(0.4);

    // 测网仿射（采样道头拟合）：井位 → 连续 (IL, XL) 线号解 → 就近线。
    const SurveyAffine aff = fitSurveyAffine(job->req.seismicPath, reader.traceCount());
    if (!aff.ok)
        return setError(&job->error, aff.reason);
    const double det = aff.bx * aff.cy - aff.by * aff.cx;
    if (std::fabs(det) < 1e-12)
        return setError(&job->error, QStringLiteral("测网几何退化 det=%1 bx=%2 by=%3 cx=%4 cy=%5 n=%6").arg(det).arg(aff.bx).arg(aff.by).arg(aff.cx).arg(aff.cy).arg(aff.inlineMin));
    const double dx = job->req.wellX - aff.ax;
    const double dy = job->req.wellY - aff.ay;
    const double ilF = (dx * aff.cy - dy * aff.cx) / det;
    const double xlF = (aff.bx * dy - aff.by * dx) / det;
    const qint32 ilClamped = qint32(std::clamp(std::llround(ilF),
                                               qint64(qint32(aff.inlineMin)),
                                               qint64(qint32(aff.inlineMax))));
    const qint32 xlClamped = qint32(std::clamp(std::llround(xlF),
                                               qint64(qint32(aff.xlineMin)),
                                               qint64(qint32(aff.xlineMax))));
    job->nearestTraceNote = QStringLiteral("井旁道 inline %1 / crossline %2（井位连续解 IL %3 XL %4）")
                                .arg(ilClamped)
                                .arg(xlClamped)
                                .arg(ilF, 0, 'f', 1)
                                .arg(xlF, 0, 'f', 1);

    QVector<SegyTrace> line;
    QString lineErr;
    if (!reader.readInline(ilClamped, &line, &lineErr) || line.isEmpty())
        return setError(&job->error, QStringLiteral("读取井旁测线失败: %1").arg(lineErr));
    const SegyTrace *best = nullptr;
    for (const SegyTrace &t : line) {
        if (!best || std::abs(t.xlineNo - xlClamped) < std::abs(best->xlineNo - xlClamped))
            best = &t;
    }
    if (!best)
        return setError(&job->error, QStringLiteral("井旁测线无道"));
    if (progress)
        progress(0.7);

    const double dtMs = best->sampleIntervalUs > 0 ? best->sampleIntervalUs / 1000.0
                                                   : reader.sampleIntervalUs() / 1000.0;
    const double t0Ms = std::isfinite(best->startTimeMs) ? best->startTimeMs : 0.0;
    const int wlSamples = std::max(3, int(std::lround(job->req.waveletLengthMs / dtMs)) | 1);
    const paleo::inversion::WaveletExtractResult r = paleo::inversion::extractWavelet(
        best->samples.constData(), best->samples.size(), t0Ms, dtMs,
        job->spikes.data(), int(job->spikes.size()), -0.5 * (wlSamples - 1) * dtMs, wlSamples,
        WaveletExtractOptions{});
    if (!r.ok)
        return setError(&job->error, QStringLiteral("子波提取失败: %1")
                                         .arg(QString::fromStdString(r.reason)));
    job->wavelet = r.wavelet;
    job->fitCorrelation = r.fitCorrelation;
    job->ok = true;
    if (progress)
        progress(1.0);
    return true;
}

QString InversionWorkflow::publishWaveletJob(const WaveletJob &job, DerivedAssetRegistrar &registrar,
                                             QString *error) const
{
    if (!job.ok) {
        setError(error, job.error.isEmpty() ? QStringLiteral("子波作业未成功") : job.error);
        return QString();
    }
    if (!registrar.isBound()) {
        setError(error, QStringLiteral("派生产物登记未绑定 catalog"));
        return QString();
    }

    const QString name = job.req.displayName.isEmpty()
                             ? QStringLiteral("子波·%1").arg(job.req.wellId)
                             : job.req.displayName;
    QString stageErr;
    const DerivedStaging staging =
        registrar.stage(QStringLiteral("wavelet"), name, QStringLiteral("wavelet.json"), &stageErr);
    if (!staging.isValid()) {
        setError(error, stageErr);
        return QString();
    }

    QVariantMap extra;
    extra.insert(QStringLiteral("wellId"), job.req.wellId);
    extra.insert(QStringLiteral("method"), QStringLiteral("wiener-ls"));
    extra.insert(QStringLiteral("fitCorrelation"), job.fitCorrelation);
    extra.insert(QStringLiteral("dominantFreqHz"), job.wavelet.dominantFreqHz());
    extra.insert(QStringLiteral("sampleIntervalMs"), job.wavelet.sampleIntervalMs);
    extra.insert(QStringLiteral("t0Ms"), job.wavelet.t0Ms);
    extra.insert(QStringLiteral("lengthSamples"), job.wavelet.sampleCount());
    extra.insert(QStringLiteral("spikeCount"), int(job.spikes.size()));
    extra.insert(QStringLiteral("sourceSeismic"), job.req.seismicPath);
    extra.insert(QStringLiteral("nearestTrace"), job.nearestTraceNote);
    const QByteArray body = paleo::inversion::waveletToJson(job.wavelet, QJsonObject());
    QFile file(staging.absolutePath);
    if (!file.open(QIODevice::WriteOnly) || file.write(body) != body.size()) {
        setError(error, QStringLiteral("wavelet.json 写入失败: %1").arg(staging.absolutePath));
        return QString();
    }
    file.close();

    const QStringList parents = job.req.parentVersionId.isEmpty()
                                    ? QStringList()
                                    : QStringList{job.req.parentVersionId};
    const QString source = job.req.sourceUri.isEmpty()
                               ? QStringLiteral("wavelet:%1").arg(job.req.wellId)
                               : job.req.sourceUri;
    QString commitErr;
    if (!registrar.commit(staging, parents, source, extra, &commitErr)) {
        setError(error, commitErr);
        return QString();
    }
    return staging.versionId;
}

// ---- 体反演作业 --------------------------------------------------------------

bool InversionWorkflow::prepareInversionJob(const InversionJobRequest &req, InversionJob *job,
                                            QString *error) const
{
    if (!job)
        return setError(error, QStringLiteral("job 为空"));
    job->prepared = false;
    job->ok = false;
    job->error.clear();
    job->req = req;
    if (req.method != QStringLiteral("bandlimited") && req.method != QStringLiteral("sparse"))
        return setError(error, QStringLiteral("method 必须为 bandlimited 或 sparse"));
    if (req.seismicPath.isEmpty() || !QFile::exists(req.seismicPath))
        return setError(error, QStringLiteral("地震体路径无效: %1").arg(req.seismicPath));
    if (!(req.lowCutHz > 0.0))
        return setError(error, QStringLiteral("lowCutHz 必须 > 0"));

    paleo::inversion::Wavelet wavelet = req.wavelet;
    if (!req.hasWaveletValue) {
        if (req.waveletPath.isEmpty() || !QFile::exists(req.waveletPath))
            return setError(error,
                            QStringLiteral("子波文件无效: %1（先提取子波，不留暗兜底）")
                                .arg(req.waveletPath));
        QFile f(req.waveletPath);
        if (!f.open(QIODevice::ReadOnly))
            return setError(error, QStringLiteral("子波文件不可读: %1").arg(req.waveletPath));
        QString jsonErr;
        if (!paleo::inversion::waveletFromJson(f.readAll(), &wavelet, nullptr, &jsonErr))
            return setError(error, QStringLiteral("子波文件解析失败: %1").arg(jsonErr));
    }
    if (wavelet.isEmpty())
        return setError(error, QStringLiteral("子波为空"));

    SegyReader reader;
    QString openErr;
    if (!reader.open(req.seismicPath, &openErr))
        return setError(error, QStringLiteral("打开地震体失败: %1").arg(openErr));
    const QVector<qint32> ils = reader.inlineNumbers();
    const QVector<qint32> xls = reader.crosslineNumbers();
    job->nIl = ils.size();
    job->nXl = xls.size();
    job->nS = reader.samplesPerTrace();
    job->dtMs = reader.sampleIntervalUs() / 1000.0;
    const SegyGeometry g = reader.geometry();
    job->t0Ms = g.startTimeMs;
    job->inlineMin = ils.isEmpty() ? 0 : ils.front();
    job->inlineStep = ils.size() > 1 ? ils.at(1) - ils.at(0) : 1;
    job->xlineMin = xls.isEmpty() ? 0 : xls.front();
    job->xlineStep = xls.size() > 1 ? xls.at(1) - xls.at(0) : 1;
    if (job->nIl < 1 || job->nXl < 1 || job->nS < 16 || !(job->dtMs > 0.0))
        return setError(error, QStringLiteral("地震体网格无效（IL/XL/采样轴）"));
    if (std::fabs(wavelet.sampleIntervalMs - job->dtMs) > 1e-9)
        return setError(error, QStringLiteral("子波采样间隔 %1 ms 与体 %2 ms 不一致（先重采样子波）")
                                   .arg(wavelet.sampleIntervalMs)
                                   .arg(job->dtMs));
    job->wavelet = wavelet;

    // 低频模型（井曲线重采样到体网格；无井 → 带限口径，如实记）。
    job->hasLowFreq = !req.lowFreqWells.empty();
    if (job->hasLowFreq) {
        LowFreqModelInput in;
        in.t0Ms = job->t0Ms;
        in.dtMs = job->dtMs;
        in.nIl = job->nIl;
        in.nXl = job->nXl;
        in.nS = job->nS;
        const SurveyAffine aff = fitSurveyAffine(req.seismicPath, reader.traceCount());
        if (!aff.ok)
            return setError(error, aff.reason);
        // 仿射系数是每单位线号的；网格步是每索引步（线号步长乘回），
        // 原点取 (inlineMin, xlineMin) 格心的坐标。
        in.originX = aff.ax + aff.bx * job->inlineMin + aff.cx * job->xlineMin;
        in.originY = aff.ay + aff.by * job->inlineMin + aff.cy * job->xlineMin;
        in.ilStepX = aff.bx * job->inlineStep;
        in.ilStepY = aff.by * job->inlineStep;
        in.xlStepX = aff.cx * job->xlineStep;
        in.xlStepY = aff.cy * job->xlineStep;
        in.lowCutHz = req.lowCutHz;
        for (const LowFreqWellInput &w : req.lowFreqWells) {
            LowFreqWellTrace t;
            t.x = w.x;
            t.y = w.y;
            t.impedance = resampleToGrid(w.twtMs, w.impedance, job->t0Ms, job->dtMs, job->nS);
            in.wells.push_back(std::move(t));
        }
        in.horizons = req.horizons;
        const auto built = paleo::inversion::lowFreqImpedance(in);
        if (!built.ok)
            return setError(error, QStringLiteral("低频模型构建失败: %1")
                                       .arg(QString::fromStdString(built.reason)));
        job->lowFreq = built;
    }
    job->prepared = true;
    return true;
}

bool InversionWorkflow::computeInversionJob(InversionJob *job, const std::function<bool()> &cancelled,
                                            const std::function<void(double)> &progress) const
{
    if (!job || !job->prepared)
        return setError(&job->error, QStringLiteral("反演作业未 prepare"));
    if (cancelled && cancelled())
        return setError(&job->error, QStringLiteral("已取消"));

    SegyReader reader;
    QString openErr;
    if (!reader.open(job->req.seismicPath, &openErr))
        return setError(&job->error, QStringLiteral("打开地震体失败: %1").arg(openErr));
    const QVector<SegyTrace> traces = reader.traces();
    if (traces.isEmpty())
        return setError(&job->error, QStringLiteral("地震体无道"));
    if (progress)
        progress(0.1);

    QHash<qint32, int> ilToIdx;
    QHash<qint32, int> xlToIdx;
    const QVector<qint32> ils = reader.inlineNumbers();
    const QVector<qint32> xls = reader.crosslineNumbers();
    for (int i = 0; i < ils.size(); ++i)
        ilToIdx.insert(ils.at(i), i);
    for (int j = 0; j < xls.size(); ++j)
        xlToIdx.insert(xls.at(j), j);

    const int nIl = job->nIl;
    const int nXl = job->nXl;
    const int nS = job->nS;
    const double dtMs = job->dtMs;
    const bool sparse = job->req.method == QStringLiteral("sparse");
    std::vector<float> volume(size_t(nIl) * size_t(nXl) * size_t(nS),
                              std::numeric_limits<float>::quiet_NaN());

    std::atomic<int> done{0};
    std::atomic<int> failed{0};
    std::atomic<bool> aborted{false};
    std::mutex progressMutex;
    const int total = traces.size();
    const int nThreads = std::max(1, std::min(4, int(std::thread::hardware_concurrency())));
    const int chunk = (total + nThreads - 1) / nThreads;

    // 线程局部聚合（join 后合并），避免跨线程浮点累加竞态。
    std::vector<double> sumStat(size_t(nThreads), 0.0);
    auto invertRange = [&](int threadIdx, int t0i, int t1i) {
        std::vector<float> lowTrace(size_t(nS), 0.0f);
        for (int ti = t0i; ti < t1i && !aborted.load(std::memory_order_relaxed); ++ti) {
            if (cancelled && cancelled()) {
                aborted.store(true, std::memory_order_relaxed);
                return;
            }
            const SegyTrace &tr = traces.at(ti);
            const int ilIdx = ilToIdx.value(tr.lineNo, -1);
            const int xlIdx = xlToIdx.value(tr.xlineNo, -1);
            const float *lowPtr = nullptr;
            if (ilIdx >= 0 && xlIdx >= 0 && job->hasLowFreq) {
                paleo::inversion::lowFreqTraceAt(job->lowFreq, ilIdx, xlIdx, lowTrace.data());
                bool anyFinite = false;
                for (float v : lowTrace)
                    anyFinite = anyFinite || std::isfinite(v);
                lowPtr = anyFinite ? lowTrace.data() : nullptr;
            }
            const float *samples = tr.samples.constData();
            const int n = std::min(int(tr.samples.size()), nS);
            bool okTrace = false;
            if (ilIdx >= 0 && xlIdx >= 0 && n == nS && n > 0) {
                float *dst = volume.data() + (size_t(ilIdx) * size_t(nXl) + size_t(xlIdx)) * size_t(nS);
                if (sparse) {
                    paleo::inversion::SparseSpikeOptions opt;
                    opt.lambda = job->req.lambda;
                    opt.maxIterations = job->req.maxIterations;
                    const auto r = paleo::inversion::sparseSpikeInversion(samples, n, dtMs,
                                                                          job->wavelet, lowPtr, opt);
                    if (r.ok) {
                        std::copy(r.impedance.begin(), r.impedance.end(), dst);
                        sumStat[size_t(threadIdx)] += r.residualEnergyRatio;
                        okTrace = true;
                    }
                } else {
                    paleo::inversion::BandlimitedOptions opt;
                    opt.lowCutHz = job->req.lowCutHz;
                    const auto r = paleo::inversion::bandlimitedInversion(samples, n, dtMs,
                                                                          &job->wavelet, lowPtr, opt);
                    if (r.ok) {
                        std::copy(r.impedance.begin(), r.impedance.end(), dst);
                        sumStat[size_t(threadIdx)] += r.lowFreqVarianceFraction;
                        okTrace = true;
                    }
                }
            }
            if (!okTrace)
                failed.fetch_add(1, std::memory_order_relaxed);
            const int d = done.fetch_add(1, std::memory_order_relaxed) + 1;
            if (progress && (d % 64 == 0 || d == total)) {
                std::lock_guard<std::mutex> lock(progressMutex);
                progress(0.1 + 0.85 * double(d) / double(total));
            }
        }
    };

    std::vector<std::thread> pool;
    for (int t = 1; t < nThreads; ++t)
        pool.emplace_back(invertRange, t, std::min(total, t * chunk), std::min(total, (t + 1) * chunk));
    invertRange(0, 0, std::min(total, chunk));
    for (auto &th : pool)
        th.join();
    for (double v : sumStat) {
        if (sparse)
            job->meanResidualEnergyRatio += v;
        else
            job->meanLowFreqVarianceFraction += v;
    }
    if (aborted.load())
        return setError(&job->error, QStringLiteral("已取消"));
    if (progress)
        progress(0.97);

    job->processedTraces = done.load();
    job->failedTraces = failed.load();
    if (job->processedTraces > 0) {
        if (sparse)
            job->meanResidualEnergyRatio /= double(job->processedTraces);
        else
            job->meanLowFreqVarianceFraction /= double(job->processedTraces);
    }

    // 体头 + 临时落盘（发布时经 commitExternal 拷进受管目录）。
    QJsonObject header;
    header.insert(QStringLiteral("nIl"), nIl);
    header.insert(QStringLiteral("nXl"), nXl);
    header.insert(QStringLiteral("nS"), nS);
    header.insert(QStringLiteral("t0Ms"), job->t0Ms);
    header.insert(QStringLiteral("dtMs"), dtMs);
    header.insert(QStringLiteral("inlineMin"), double(job->inlineMin));
    header.insert(QStringLiteral("inlineStep"), double(job->inlineStep));
    header.insert(QStringLiteral("xlineMin"), double(job->xlineMin));
    header.insert(QStringLiteral("xlineStep"), double(job->xlineStep));
    header.insert(QStringLiteral("method"), job->req.method);
    header.insert(QStringLiteral("lowCutHz"), job->req.lowCutHz);
    header.insert(QStringLiteral("hasLowFreq"), job->hasLowFreq);
    header.insert(QStringLiteral("lowFreqLayersUsed"), job->lowFreq.layersUsed);
    header.insert(QStringLiteral("waveletDominantHz"), job->wavelet.dominantFreqHz());
    header.insert(QStringLiteral("sourceSeismic"), job->req.seismicPath);
    header.insert(QStringLiteral("frequencyBand"),
                  QStringLiteral("低频模型 0–%1Hz + 地震带限（不含带外高频，如实口径）")
                      .arg(job->req.lowCutHz));
    if (sparse) {
        header.insert(QStringLiteral("lambdaUsed"), job->req.lambda);
        header.insert(QStringLiteral("maxIterations"), job->req.maxIterations);
    }

    const QString tempDir = QDir(QDir::temp()).filePath(
        QStringLiteral("paleo-inv-%1").arg(QUuid::createUuid().toString(QUuid::Id128)));
    if (!QDir().mkpath(tempDir))
        return setError(&job->error, QStringLiteral("临时目录创建失败: %1").arg(tempDir));
    job->tempVolumePath = QDir(tempDir).filePath(QStringLiteral("impedance.iimp"));
    QFile out(job->tempVolumePath);
    if (!out.open(QIODevice::WriteOnly))
        return setError(&job->error, QStringLiteral("临时体写入失败: %1").arg(job->tempVolumePath));
    const QByteArray blob = paleo::inversion::writeImpedanceVolumeBlob(volume, header);
    if (out.write(blob) != blob.size())
        return setError(&job->error, QStringLiteral("临时体写入不完整"));
    out.close();

    job->ok = true;
    if (progress)
        progress(1.0);
    return true;
}

InversionWorkflow::PublishResult InversionWorkflow::publishInversionJob(
    const InversionJob &job, DerivedAssetRegistrar &registrar, QString *error) const
{
    PublishResult result;
    if (!job.ok)
    {
        result.error = job.error.isEmpty() ? QStringLiteral("反演作业未成功") : job.error;
        setError(error, result.error);
        return result;
    }
    if (!registrar.isBound())
    {
        result.error = QStringLiteral("派生产物登记未绑定 catalog");
        setError(error, result.error);
        return result;
    }
    const QString name = job.req.displayName.isEmpty()
                             ? QStringLiteral("波阻抗体·%1").arg(job.req.method)
                             : job.req.displayName;
    QString stageErr;
    const DerivedStaging staging = registrar.stage(
        QStringLiteral("impedance_volume"), name, QStringLiteral("impedance.iimp"), &stageErr);
    if (!staging.isValid())
    {
        result.error = stageErr;
        setError(error, result.error);
        return result;
    }

    const QStringList parents = registrar.parentVersionIdsFor(job.req.parentPaths);
    QVariantMap extra;
    extra.insert(QStringLiteral("provenance_schema_version"), 1);
    extra.insert(QStringLiteral("method"), job.req.method);
    extra.insert(QStringLiteral("lowCutHz"), job.req.lowCutHz);
    extra.insert(QStringLiteral("hasLowFreq"), job.hasLowFreq);
    extra.insert(QStringLiteral("lowFreqLayersUsed"), job.lowFreq.layersUsed);
    extra.insert(QStringLiteral("nIl"), job.nIl);
    extra.insert(QStringLiteral("nXl"), job.nXl);
    extra.insert(QStringLiteral("nS"), job.nS);
    extra.insert(QStringLiteral("t0Ms"), job.t0Ms);
    extra.insert(QStringLiteral("dtMs"), job.dtMs);
    extra.insert(QStringLiteral("inlineMin"), double(job.inlineMin));
    extra.insert(QStringLiteral("inlineStep"), double(job.inlineStep));
    extra.insert(QStringLiteral("xlineMin"), double(job.xlineMin));
    extra.insert(QStringLiteral("xlineStep"), double(job.xlineStep));
    extra.insert(QStringLiteral("processedTraces"), job.processedTraces);
    extra.insert(QStringLiteral("failedTraces"), job.failedTraces);
    extra.insert(QStringLiteral("waveletDominantHz"), job.wavelet.dominantFreqHz());
    extra.insert(QStringLiteral("frequencyBand"),
                 QStringLiteral("低频模型 0–%1Hz + 地震带限（不含带外高频，如实口径）")
                     .arg(job.req.lowCutHz));
    if (job.req.method == QStringLiteral("sparse"))
    {
        extra.insert(QStringLiteral("lambdaUsed"), job.req.lambda);
        extra.insert(QStringLiteral("maxIterations"), job.req.maxIterations);
        extra.insert(QStringLiteral("meanResidualEnergyRatio"), job.meanResidualEnergyRatio);
    }
    else
    {
        extra.insert(QStringLiteral("meanLowFreqVarianceFraction"), job.meanLowFreqVarianceFraction);
    }
    const QString source = job.req.sourceUri.isEmpty()
                               ? QStringLiteral("inversion:%1").arg(job.req.seismicPath)
                               : job.req.sourceUri;
    QString commitErr;
    if (!registrar.commitExternal(staging, job.tempVolumePath, parents, source, extra, &commitErr))
    {
        result.error = commitErr;
        setError(error, result.error);
        return result;
    }
    result.ok = true;
    result.versionId = staging.versionId;
    result.relativePath = staging.relativePath;
    return result;
}

} // namespace paleo::inv
