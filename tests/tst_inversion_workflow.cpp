// 层：测试壳
// goal/seismic-inversion — 反演编排三段式：DERIVED 登记、并行确定性、取消诚实。
#include <QtTest>
#include <QFile>
#include <QTemporaryDir>

#include <cmath>
#include <random>

#include "../src/io/segyreader.h"
#include "../src/catalog/datacatalog.h"
#include "../src/workflow/derivedassets.h"
#include "../src/workflow/inversionworkflow.h"

using namespace paleo::inv;
using paleo::inversion::ReflSpike;
using paleo::inversion::Wavelet;

namespace
{

constexpr double kDt = 2.0;   // ms
constexpr int kNs = 400;      // 0–798 ms（层厚 250 ms ≫ 8Hz 平滑窗 125 ms，层内有稳定平台）
constexpr int kNIl = 3;       // inline 1..3
constexpr int kNXl = 3;       // crossline 10/20/30
constexpr double kF0Hz = 25.0;

// 三层阻抗：t<250 → base；250≤t<500 → base+400；t≥500 → base−200。
double layerZ(double base, double tMs)
{
    return tMs < 250.0 ? base : (tMs < 500.0 ? base + 400.0 : base - 200.0);
}

double baseAtXl(int xlIdx)
{
    return 5000.0 + 500.0 * double(xlIdx);
}

// cell → 界面反射系数尖峰（与正演同式）。
std::vector<ReflSpike> cellSpikes(double base)
{
    const double z1 = layerZ(base, 0.0);
    const double z2 = layerZ(base, 250.0);
    const double z3 = layerZ(base, 500.0);
    return {
        {250.0, float((z2 - z1) / (z2 + z1))},
        {500.0, float((z3 - z2) / (z3 + z2))},
    };
}

std::vector<float> synthTrace(const std::vector<ReflSpike> &spikes, const Wavelet &w)
{
    std::vector<float> s(std::size_t(kNs), 0.0f);
    const int off = int(std::lround(w.t0Ms / kDt));
    for (const ReflSpike &sp : spikes) {
        const int m = int(std::lround(sp.twtMs / kDt));
        for (int j = 0; j < w.sampleCount(); ++j) {
            const int i = m + off + j;
            if (i >= 0 && i < kNs)
                s[std::size_t(i)] += float(double(sp.amplitude) * double(w.samples[std::size_t(j)]));
        }
    }
    return s;
}

// 3×3 SEG-Y：inline 1..3（y 5000→4800 步 −100），xline 10/20/30（x 1000→1100 步 50）。
// 井位 (1050, 4900) = inline 2 / xline 20 格心，base=5500。
bool writeInversionSegy(const QString &path, const Wavelet &w)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(QByteArray(3200, ' '));
    QByteArray binHdr(400, char(0));
    const auto put16 = [&binHdr](int at, qint16 v) {
        binHdr[at] = char(quint8(v >> 8));
        binHdr[at + 1] = char(quint8(v));
    };
    const auto put32i = [](QByteArray &buf, int at, qint32 v) {
        buf[at] = char(quint8(v >> 24));
        buf[at + 1] = char(quint8(v >> 16));
        buf[at + 2] = char(quint8(v >> 8));
        buf[at + 3] = char(quint8(v));
    };
    put16(16, qint16(2000)); // dt µs
    put16(20, qint16(kNs));
    put16(24, qint16(5));    // IEEE
    file.write(binHdr);

    for (int i = 0; i < kNIl; ++i) {
        for (int j = 0; j < kNXl; ++j) {
            const double base = baseAtXl(j);
            const std::vector<ReflSpike> spikes = cellSpikes(base);
            const std::vector<float> s = synthTrace(spikes, w);
            QByteArray trHdr(240, char(0));
            put32i(trHdr, 0, i * kNXl + j + 1);
            put32i(trHdr, 188, 1 + i);          // inline
            put32i(trHdr, 192, 10 + 10 * j);    // xline
            put32i(trHdr, 72, qint32(1000 + 50 * j)); // Source X
            put32i(trHdr, 76, qint32(5000 - 100 * i)); // Source Y
            file.write(trHdr);
            QByteArray samples(kNs * 4, char(0));
            for (int k = 0; k < kNs; ++k) {
                quint32 bits;
                const float val = s[std::size_t(k)];
                std::memcpy(&bits, &val, 4);
                bits = qToBigEndian(bits);
                std::memcpy(samples.data() + k * 4, &bits, 4);
            }
            file.write(samples);
        }
    }
    file.close();
    return true;
}

// 井曲线（MD 域，时深 v=2500：d = t·1.25）：Z 由 base=5500 的三层律给出。
void wellCurves(std::vector<double> *depths, std::vector<float> *ac, std::vector<float> *den,
                std::vector<double> *tdD, std::vector<double> *tdT)
{
    const double base = baseAtXl(1); // xline 20 → 5500
    const double v = 2500.0;
    depths->clear();
    ac->clear();
    den->clear();
    for (int d = 0; d <= 1050; d += 5) {
        const double t = double(d) * 2000.0 / v;
        const double z = layerZ(base, t);
        const double vel = z / 2.4;
        depths->push_back(double(d));
        ac->push_back(float(1e6 / vel));
        den->push_back(2.4f);
    }
    tdD->clear();
    tdT->clear();
    for (int d = 0; d <= 1050; d += 50) {
        tdD->push_back(double(d));
        tdT->push_back(double(d) * 2000.0 / v);
    }
}

std::vector<paleo::inversion::HorizonTwtGrid> flatHorizons()
{
    std::vector<paleo::inversion::HorizonTwtGrid> hs;
    for (double t : {250.0, 500.0}) {
        paleo::inversion::HorizonTwtGrid h;
        h.name = t == 250.0 ? "H250" : "H500";
        h.twtMs.assign(std::size_t(kNIl * kNXl), float(t));
        hs.push_back(std::move(h));
    }
    return hs;
}

// 井低频输入：TWT 域三层曲线。井位取格心——xline 20（base 5500）与
// xline 30（base 6000）各一口，低频模型的横向变化由井控承载（单井 = 横向
// 恒定，这是低频模型语义而非缺陷）。
InversionWorkflow::LowFreqWellInput lowFreqWell(int xlIdx)
{
    InversionWorkflow::LowFreqWellInput w;
    w.wellId = xlIdx == 1 ? QStringLiteral("well-A1") : QStringLiteral("well-A2");
    w.x = 1000.0 + 50.0 * double(xlIdx);
    w.y = 4900.0;
    const double base = baseAtXl(xlIdx);
    for (int t = 0; t <= 798; t += 2) {
        w.twtMs.push_back(double(t));
        w.impedance.push_back(float(layerZ(base, double(t))));
    }
    return w;
}

double cellMean(const std::vector<float> &volume, int ilIdx, int xlIdx, int from, int to)
{
    const size_t base = (size_t(ilIdx) * kNXl + size_t(xlIdx)) * kNs;
    double sum = 0.0;
    int cnt = 0;
    for (int i = from; i < to; ++i) {
        const double v = double(volume[base + size_t(i)]);
        if (std::isfinite(v)) {
            sum += v;
            ++cnt;
        }
    }
    return cnt > 0 ? sum / double(cnt) : std::numeric_limits<double>::quiet_NaN();
}

} // namespace

class TestInversionWorkflow : public QObject
{
    Q_OBJECT

private slots:
    // Oracle 6：子波提取全链（井曲线+时深+井旁道 → DERIVED wavelet 资产）。
    void waveletEndToEnd();
    // Oracle 5/6：带限反演全链（提取的子波 + 低频模型 → DERIVED 阻抗体，
    // 井位格层均值闭环，extra 频段口径如实）。
    void bandlimitedEndToEnd();
    // 稀疏脉冲全链：登记 + 体可读 + 有限。
    void sparseEndToEnd();
    // 取消诚实：cancelled → compute 失败、publish 拒绝、无 DERIVED 版本。
    void cancelIsHonest();
    // prepare 失败诚实：缺子波/坏方法/子波 dt 不匹配。
    void prepareFailuresHonest();
    // 无时间戳：两次 compute 字节级一致（并行确定性）。
    void byteStableAcrossRuns();

private:
    QString m_sgyPath;
    Wavelet m_truthWavelet;
};

void TestInversionWorkflow::waveletEndToEnd()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    m_sgyPath = dir.filePath(QStringLiteral("inv.sgy"));
    m_truthWavelet = paleo::inversion::makeRicker(kF0Hz, kDt, 128.0);
    QVERIFY(writeInversionSegy(m_sgyPath, m_truthWavelet));

    DataCatalog catalog;
    QVERIFY(catalog.open(dir.path(), nullptr));
    CatalogAsset asset;
    asset.id = QStringLiteral("ast-seis");
    asset.type = QStringLiteral("seismic_survey");
    asset.format = QStringLiteral("sgy");
    asset.displayName = QStringLiteral("seis");
    QVERIFY(catalog.addAsset(asset, nullptr));
    CatalogVersion parent;
    parent.id = QStringLiteral("ver-raw");
    parent.assetId = asset.id;
    parent.stage = QStringLiteral("RAW");
    parent.versionNumber = 1;
    parent.fileName = QStringLiteral("inv.sgy");
    QVERIFY(catalog.addVersion(parent, nullptr));

    std::vector<double> depths, tdD, tdT;
    std::vector<float> ac, den;
    wellCurves(&depths, &ac, &den, &tdD, &tdT);

    InversionWorkflow wf;
    WaveletJobRequest req;
    req.wellId = QStringLiteral("well-A1");
    req.wellX = 1050.0;
    req.wellY = 4900.0;
    req.acDepthsM = depths;
    req.acUsPerM = ac;
    req.denDepthsM = depths;
    req.denValues = den;
    req.tdDepthM = tdD;
    req.tdTimeMs = tdT;
    req.seismicPath = m_sgyPath;
    req.parentVersionId = parent.id;

    InversionWorkflow::WaveletJob job;
    QString err;
    QVERIFY2(wf.prepareWaveletJob(req, &job, &err), qPrintable(err));
    QVERIFY2(wf.computeWaveletJob(&job), qPrintable(job.error));
    QVERIFY2(job.fitCorrelation > 0.9,
             qPrintable(QString("拟合相关 %1").arg(job.fitCorrelation)));
    QVERIFY(std::fabs(job.wavelet.dominantFreqHz() - kF0Hz) < 3.0);
    QVERIFY(!job.nearestTraceNote.isEmpty());

    DerivedAssetRegistrar registrar(&catalog, dir.path());
    const QString versionId = wf.publishWaveletJob(job, registrar, &err);
    QVERIFY2(!versionId.isEmpty(), qPrintable(err));

    const CatalogVersion *published = nullptr;
    for (const CatalogVersion &v : catalog.versions())
        if (v.id == versionId)
            published = &v;
    QVERIFY(published != nullptr);
    QCOMPARE(published->stage, QStringLiteral("DERIVED"));
    QCOMPARE(published->parentVersionIds, QStringList{parent.id});
    QCOMPARE(published->extra.value(QStringLiteral("wellId")).toString(), QStringLiteral("well-A1"));
    QVERIFY(published->extra.value(QStringLiteral("fitCorrelation")).toDouble() > 0.9);

    // 发布文件可读回（子波持久化闭环）。
    const QString waveletPath = dir.filePath(published->path);
    QFile f(waveletPath);
    QVERIFY(f.open(QIODevice::ReadOnly));
    Wavelet back;
    QString jsonErr;
    QVERIFY2(paleo::inversion::waveletFromJson(f.readAll(), &back, nullptr, &jsonErr),
             qPrintable(jsonErr));
    QCOMPARE(back.sampleCount(), job.wavelet.sampleCount());
    QVERIFY(std::fabs(back.dominantFreqHz() - kF0Hz) < 3.0);
}

void TestInversionWorkflow::bandlimitedEndToEnd()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    m_sgyPath = dir.filePath(QStringLiteral("inv.sgy"));
    m_truthWavelet = paleo::inversion::makeRicker(kF0Hz, kDt, 128.0);
    QVERIFY(writeInversionSegy(m_sgyPath, m_truthWavelet));
    DataCatalog catalog;
    QVERIFY(catalog.open(dir.path(), nullptr));
    DerivedAssetRegistrar registrar(&catalog, dir.path());

    InversionWorkflow wf;
    InversionWorkflow::InversionJobRequest req;
    req.seismicPath = m_sgyPath;
    req.wavelet = m_truthWavelet; // 上一用例的提取-发布-读回链在 waveletEndToEnd 验过
    req.hasWaveletValue = true;
    req.method = QStringLiteral("bandlimited");
    req.lowCutHz = 8.0;
    req.lowFreqWells = {lowFreqWell(1), lowFreqWell(2)};
    req.horizons = flatHorizons();
    req.parentPaths = {m_sgyPath};

    InversionWorkflow::InversionJob job;
    QString err;
    QVERIFY2(wf.prepareInversionJob(req, &job, &err), qPrintable(err));
    QCOMPARE(job.nIl, kNIl);
    QCOMPARE(job.nXl, kNXl);
    QCOMPARE(job.nS, kNs);
    QVERIFY(job.hasLowFreq);
    QCOMPARE(job.lowFreq.layersUsed, 3);
    int progressCalls = 0;
    QVERIFY2(wf.computeInversionJob(&job, {}, [&](double) { ++progressCalls; }),
             qPrintable(job.error));
    QCOMPARE(job.processedTraces, kNIl * kNXl);
    QCOMPARE(job.failedTraces, 0);
    QVERIFY(progressCalls > 0);

    const InversionWorkflow::PublishResult pub = wf.publishInversionJob(job, registrar, &err);
    QVERIFY2(pub.ok, qPrintable(pub.error.isEmpty() ? err : pub.error));
    QVERIFY(!pub.versionId.isEmpty());

    const CatalogVersion *published = nullptr;
    for (const CatalogVersion &v : catalog.versions())
        if (v.id == pub.versionId)
            published = &v;
    QVERIFY(published != nullptr);
    QCOMPARE(published->stage, QStringLiteral("DERIVED"));
    QCOMPARE(published->extra.value(QStringLiteral("lowCutHz")).toDouble(), 8.0);
    QCOMPARE(published->extra.value(QStringLiteral("method")).toString(), QStringLiteral("bandlimited"));
    QVERIFY(published->extra.contains(QStringLiteral("frequencyBand")));
    QVERIFY(published->extra.value(QStringLiteral("frequencyBand")).toString().contains(QStringLiteral("带限")));

    // 体读回：井位格（inline 2 / xline 20）层均值闭环（让开边界 ± 半窗）。
    QFile vf(dir.filePath(published->path));
    QVERIFY(vf.open(QIODevice::ReadOnly));
    std::vector<float> volume;
    QJsonObject header;
    QString verErr;
    QVERIFY2(paleo::inversion::readImpedanceVolumeBlob(vf.readAll(), &volume, &header, &verErr),
             qPrintable(verErr));
    QCOMPARE(header.value(QStringLiteral("nIl")).toInt(), kNIl);
    QCOMPARE(int(volume.size()), kNIl * kNXl * kNs);
    const double l0 = cellMean(volume, 1, 1, 78, 92);
    const double l1 = cellMean(volume, 1, 1, 160, 220);
    const double l2 = cellMean(volume, 1, 1, 300, 390);
    QVERIFY2(std::fabs(l0 - 5500.0) / 5500.0 < 0.02,
             qPrintable(QString("层0 %1").arg(l0)));
    QVERIFY2(std::fabs(l1 - 5900.0) / 5900.0 < 0.02,
             qPrintable(QString("层1 %1").arg(l1)));
    QVERIFY2(std::fabs(l2 - 5300.0) / 5300.0 < 0.02,
             qPrintable(QString("层2 %1").arg(l2)));
    // 横向：xline 30 格（base 6000）层1 抬升 500。
    const double l1Far = cellMean(volume, 1, 2, 160, 220);
    QVERIFY2(std::fabs(l1Far - 6400.0) / 6400.0 < 0.02,
             qPrintable(QString("层1 远格 %1").arg(l1Far)));
}

void TestInversionWorkflow::sparseEndToEnd()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString sgy = dir.filePath(QStringLiteral("inv.sgy"));
    const Wavelet w = paleo::inversion::makeRicker(kF0Hz, kDt, 128.0);
    QVERIFY(writeInversionSegy(sgy, w));
    DataCatalog catalog;
    QVERIFY(catalog.open(dir.path(), nullptr));
    DerivedAssetRegistrar registrar(&catalog, dir.path());

    InversionWorkflow wf;
    InversionWorkflow::InversionJobRequest req;
    req.seismicPath = sgy;
    req.wavelet = w;
    req.hasWaveletValue = true;
    req.method = QStringLiteral("sparse");
    req.lowCutHz = 8.0;
    req.lowFreqWells = {lowFreqWell(1), lowFreqWell(2)};
    req.horizons = flatHorizons();

    InversionWorkflow::InversionJob job;
    QString err;
    QVERIFY2(wf.prepareInversionJob(req, &job, &err), qPrintable(err));
    QVERIFY2(wf.computeInversionJob(&job), qPrintable(job.error));
    const InversionWorkflow::PublishResult pub = wf.publishInversionJob(job, registrar, &err);
    QVERIFY2(pub.ok, qPrintable(pub.error.isEmpty() ? err : pub.error));

    QFile vf(dir.filePath(pub.relativePath));
    QVERIFY(vf.open(QIODevice::ReadOnly));
    std::vector<float> volume;
    QString verErr;
    QVERIFY(paleo::inversion::readImpedanceVolumeBlob(vf.readAll(), &volume, nullptr, &verErr));
    QCOMPARE(int(volume.size()), kNIl * kNXl * kNs);
    const size_t base = (size_t(1) * kNXl + size_t(1)) * kNs;
    for (int i : {20, 100, 200, 280})
        QVERIFY(std::isfinite(volume[base + size_t(i)]));
}

void TestInversionWorkflow::cancelIsHonest()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString sgy = dir.filePath(QStringLiteral("inv.sgy"));
    const Wavelet w = paleo::inversion::makeRicker(kF0Hz, kDt, 128.0);
    QVERIFY(writeInversionSegy(sgy, w));
    DataCatalog catalog;
    QVERIFY(catalog.open(dir.path(), nullptr));
    DerivedAssetRegistrar registrar(&catalog, dir.path());

    InversionWorkflow wf;
    InversionWorkflow::InversionJobRequest req;
    req.seismicPath = sgy;
    req.wavelet = w;
    req.hasWaveletValue = true;
    req.method = QStringLiteral("bandlimited");
    InversionWorkflow::InversionJob job;
    QString err;
    QVERIFY(wf.prepareInversionJob(req, &job, &err));
    QVERIFY(!wf.computeInversionJob(&job, [] { return true; }));
    QVERIFY(!job.ok);
    QVERIFY2(job.error.contains(QStringLiteral("取消")), qPrintable(job.error));
    const int versionsBefore = catalog.versions().size();
    const InversionWorkflow::PublishResult pub = wf.publishInversionJob(job, registrar, &err);
    QVERIFY(!pub.ok);
    QCOMPARE(catalog.versions().size(), versionsBefore);
}

void TestInversionWorkflow::prepareFailuresHonest()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString sgy = dir.filePath(QStringLiteral("inv.sgy"));
    const Wavelet w = paleo::inversion::makeRicker(kF0Hz, kDt, 128.0);
    QVERIFY(writeInversionSegy(sgy, w));

    InversionWorkflow wf;
    InversionWorkflow::InversionJobRequest req;
    req.seismicPath = sgy;
    req.method = QStringLiteral("bandlimited");

    InversionWorkflow::InversionJob job;
    QString err;
    // 缺子波：不留暗兜底。
    QVERIFY(!wf.prepareInversionJob(req, &job, &err));
    QVERIFY2(err.contains(QStringLiteral("子波")), qPrintable(err));
    // 坏方法名。
    req.wavelet = w;
    req.hasWaveletValue = true;
    req.method = QStringLiteral("avo-magic");
    QVERIFY(!wf.prepareInversionJob(req, &job, &err));
    QVERIFY2(err.contains(QStringLiteral("method")), qPrintable(err));
    // 子波 dt 与体不一致。
    req.method = QStringLiteral("bandlimited");
    req.wavelet = paleo::inversion::makeRicker(kF0Hz, 4.0, 128.0);
    QVERIFY(!wf.prepareInversionJob(req, &job, &err));
    QVERIFY2(err.contains(QStringLiteral("不一致")), qPrintable(err));
}

void TestInversionWorkflow::byteStableAcrossRuns()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString sgy = dir.filePath(QStringLiteral("inv.sgy"));
    const Wavelet w = paleo::inversion::makeRicker(kF0Hz, kDt, 128.0);
    QVERIFY(writeInversionSegy(sgy, w));

    InversionWorkflow wf;
    InversionWorkflow::InversionJobRequest req;
    req.seismicPath = sgy;
    req.wavelet = w;
    req.hasWaveletValue = true;
    req.method = QStringLiteral("bandlimited");
    req.lowFreqWells = {lowFreqWell(1), lowFreqWell(2)};
    req.horizons = flatHorizons();

    QByteArray first, second;
    for (int run = 0; run < 2; ++run) {
        InversionWorkflow::InversionJob job;
        QString err;
        QVERIFY(wf.prepareInversionJob(req, &job, &err));
        QVERIFY(wf.computeInversionJob(&job));
        QFile f(job.tempVolumePath);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray bytes = f.readAll();
        if (run == 0)
            first = bytes;
        else
            second = bytes;
    }
    QCOMPARE(first.size(), second.size());
    QVERIFY(first == second);
}

QTEST_MAIN(TestInversionWorkflow)
#include "tst_inversion_workflow.moc"
