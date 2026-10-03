// 层：测试壳
// goal/seismic-inversion — 真工区实测（env 门控）：200P_seismic.sgy（263,451 道
// ×901 样×2ms）全量带限反演，只读契约（目录前后快照逐项相等），墙钟记账不设
// 硬门（环境差异大，比率行进 ledger/progress）。未设 env 即 QSKIP。
#include <QtTest>
#include <QDir>
#include <QFile>
#include <QProcessEnvironment>

#include <cmath>

#include "../src/algorithms/inversion/wavelet.h"
#include "../src/workflow/inversionworkflow.h"

using namespace paleo::inv;

namespace
{

QString resolveRealSgy()
{
    const auto env = QProcessEnvironment::systemEnvironment();
    const QString direct = env.value(QStringLiteral("PALEO_SEISMIC_REAL_SGY"));
    if (!direct.isEmpty() && QFile::exists(direct))
        return direct;
    const QString area = env.value(QStringLiteral("PALEO_REAL_PROJECT_AREA"));
    if (area.isEmpty())
        return QString();
    const QString cand = QDir(area).filePath(QStringLiteral("地震体/200P_seismic.sgy"));
    return QFile::exists(cand) ? cand : QString();
}

// 目录快照（名+字节），只读契约用。
QHash<QString, qint64> snapshotDir(const QString &path)
{
    QHash<QString, qint64> snap;
    QDir dir(path);
    const auto entries =
        dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo &fi : entries)
        snap.insert(fi.absoluteFilePath(), fi.isDir() ? -1 : fi.size());
    return snap;
}

} // namespace

class TestInversionRealArea : public QObject
{
    Q_OBJECT

private slots:
    void realVolumeInversion();
};

void TestInversionRealArea::realVolumeInversion()
{
    const QString sgy = resolveRealSgy();
    if (sgy.isEmpty())
        QSKIP("PALEO_REAL_PROJECT_AREA / PALEO_SEISMIC_REAL_SGY 未设或真工区不存在——真工区实测跳过");

    const QString area = QFileInfo(sgy).absolutePath();
    const QHash<QString, qint64> before = snapshotDir(area);

    // 合成 Ricker 子波（真工区子波提取需井/catalog 装配，属 UI 链路；此处
    // 测读面+反演+体落盘的吞吐，子波口径如实记「合成 25Hz」）。
    const paleo::inversion::Wavelet w = paleo::inversion::makeRicker(25.0, 2.0, 128.0);
    InversionWorkflow wf;
    InversionWorkflow::InversionJobRequest req;
    req.seismicPath = sgy;
    req.wavelet = w;
    req.hasWaveletValue = true;
    req.method = QStringLiteral("bandlimited");
    req.lowCutHz = 8.0;
    req.displayName = QStringLiteral("真工区实测·带限");

    InversionWorkflow::InversionJob job;
    QString err;
    QVERIFY2(wf.prepareInversionJob(req, &job, &err), qPrintable(err));
    qInfo() << "real grid: IL" << job.nIl << "XL" << job.nXl << "S" << job.nS
            << "dt" << job.dtMs << "ms, traces expect" << job.nIl * job.nXl;

    QElapsedTimer timer;
    timer.start();
    QVERIFY2(wf.computeInversionJob(&job), qPrintable(job.error));
    const qint64 elapsed = timer.elapsed();
    qInfo() << "inversion real-area perf:" << elapsed << "ms for" << job.processedTraces
            << "/" << job.nIl * job.nXl << "traces (failed" << job.failedTraces << ")";
    QVERIFY(job.processedTraces > 200000); // 263k 级体（允许坏道少量失败）
    QVERIFY(job.failedTraces * 100 < job.processedTraces); // 失败 <1%

    // 体临时文件可读回（头自洽）。
    QFile vf(job.tempVolumePath);
    QVERIFY(vf.open(QIODevice::ReadOnly));
    std::vector<float> volume;
    QJsonObject header;
    QString verErr;
    QVERIFY2(paleo::inversion::readImpedanceVolumeBlob(vf.readAll(), &volume, &header, &verErr),
             qPrintable(verErr));
    QCOMPARE(int(volume.size()), job.nIl * job.nXl * job.nS);
    // 有限值抽样（NaN=缺失道诚实存在，不要求全有限）。
    int finite = 0;
    for (std::size_t i = 0; i < volume.size(); i += volume.size() / 1000 + 1)
        finite += std::isfinite(volume[i]) ? 1 : 0;
    qInfo() << "sampled finite ratio:" << finite;
    QVERIFY(finite > 0);

    // 只读契约：真工区目录逐项不变（产物只在系统临时目录）。
    QCOMPARE(snapshotDir(area), before);
}

QTEST_MAIN(TestInversionRealArea)
#include "tst_inversion_realarea.moc"
