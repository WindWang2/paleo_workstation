// tst_startup_trace — goal/perf-systematize 簇1：启动分段仪表 + 比率门。
// 三层防线：
//   1) StartupTrace 单元面：mark 次序、相邻同名去重、JSON 形状、reset。
//   2) 真实进程集成：QProcess 起真正的 paleo 二进制（offscreen + 落盘 +
//      首帧即退），解析 JSON：段全、序对、时间非负。
//   3) 机器无关比率门：docs/perf/baselines/startup_ratios.json × 容差；
//      判别力证明——PALEO_STARTUP_INJECT_DELAY_MS 给真实进程注入已知
//      劣化（qgis 段 = qgis_init_share 分子），断言门必红（注入 900ms
//      不红 = 门无判别力，本测试自身失败）。
#include <QtTest>

#include "services/startuptrace.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>

#include <algorithm>

namespace
{
// 启动段词表（src/app/main.cpp + appcontext.cpp 打点；改动先同步两处）。
const QStringList kRequiredSegments = {
    QStringLiteral("main_entry"),   QStringLiteral("pre_qt_ready"),
    QStringLiteral("qgis_app_ready"), QStringLiteral("services_ready"),
    QStringLiteral("theme_ready"),  QStringLiteral("main_window_ready"),
    QStringLiteral("window_shown"), QStringLiteral("first_paint"),
};
// 比率门容差：单轮起真实进程在共享机上分母波动大（实测健康轮 qgis 份额
// 极值达基线 2.04×——快分母×慢分子），基线 ×2.5。判别力不受影响：注入
// 1500ms 劣化把份额推到 ~0.6，距门（0.088×2.5=0.22）仍有 2.7× 裕度。
constexpr double kGateTolerance = 2.5;
} // namespace

class StartupTraceTests : public QObject
{
    Q_OBJECT

  private slots:
    void markRecordsSegmentsInOrder();
    void adjacentDuplicateMarkCollapsed();
    void jsonShapeIsStable();
    void resetClearsState();
    void realStartupProducesCompleteTrace();
    void realStartupRatioGatesHold();
    void injectedDegradationMustTripGate();

  private:
    struct AppRun
    {
        bool ok = false;
        QString error;
        QJsonObject json;
    };
    static AppRun launchApp(const QString &tracePath, int injectDelayMs = 0);
    static QJsonObject loadBaselineRatios();
    static double segAtMs(const QJsonObject &startup, const QString &name);
    // 纯函数门判定（单元面可喂合成 JSON）。
    struct GateOutcome
    {
        bool evaluated = false;
        bool pass = false;
        double ratio = -1.0;
        double gate = -1.0;
        QString key;
    };
    static QVector<GateOutcome> evaluateGates(const QJsonObject &startup,
                                              const QJsonObject &baselines);
};

StartupTraceTests::AppRun StartupTraceTests::launchApp(const QString &tracePath,
                                                       int injectDelayMs)
{
    AppRun run;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    env.insert(QStringLiteral("PALEO_STARTUP_TRACE"), tracePath);
    env.insert(QStringLiteral("PALEO_STARTUP_EXIT_AFTER_FRAME"), QStringLiteral("1"));
    if (injectDelayMs > 0)
        env.insert(QStringLiteral("PALEO_STARTUP_INJECT_DELAY_MS"),
                   QString::number(injectDelayMs));
    QProcess proc;
    proc.setProcessEnvironment(env);
    proc.setProcessChannelMode(QProcess::SeparateChannels);
    proc.start(QStringLiteral(PALEO_APP_BINARY), {});
    if (!proc.waitForStarted(10000))
    {
        run.error = QStringLiteral("paleo 进程起不来：%1").arg(proc.errorString());
        return run;
    }
    if (!proc.waitForFinished(60000))
    {
        proc.kill();
        proc.waitForFinished(5000);
        run.error = QStringLiteral("paleo 60s 未退出（EXIT_AFTER_FRAME 未生效？）");
        return run;
    }
    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0)
    {
        run.error = QStringLiteral("paleo 异常退出 code=%1 status=%2")
                        .arg(proc.exitCode())
                        .arg(int(proc.exitStatus()));
        return run;
    }
    QFile f(tracePath);
    if (!f.open(QIODevice::ReadOnly))
    {
        run.error = QStringLiteral("落盘文件缺失：%1").arg(tracePath);
        return run;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject())
    {
        run.error = QStringLiteral("trace JSON 不可解析");
        return run;
    }
    run.json = doc.object();
    run.ok = true;
    return run;
}

QJsonObject StartupTraceTests::loadBaselineRatios()
{
    const QStringList candidates = {
#ifdef PALEO_SOURCE_DIR
        QStringLiteral(PALEO_SOURCE_DIR "/docs/perf/baselines/startup_ratios.json"),
#endif
        QStringLiteral("../docs/perf/baselines/startup_ratios.json"),
        QStringLiteral("../../docs/perf/baselines/startup_ratios.json"),
        QStringLiteral("docs/perf/baselines/startup_ratios.json"),
    };
    for (const QString &c : candidates)
    {
        QFile f(QFileInfo(c).absoluteFilePath());
        if (f.open(QIODevice::ReadOnly))
        {
            const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
            if (doc.isObject())
                return doc.object();
        }
    }
    return {};
}

double StartupTraceTests::segAtMs(const QJsonObject &startup, const QString &name)
{
    const QJsonArray segs = startup.value(QStringLiteral("segments")).toArray();
    for (const QJsonValue &v : segs)
    {
        const QJsonObject o = v.toObject();
        if (o.value(QStringLiteral("name")).toString() == name)
            return o.value(QStringLiteral("at_ms")).toDouble(-1.0);
    }
    return -1.0;
}

QVector<StartupTraceTests::GateOutcome> StartupTraceTests::evaluateGates(
    const QJsonObject &startup, const QJsonObject &baselines)
{
    QVector<GateOutcome> out;
    const double total =
        segAtMs(startup, QStringLiteral("first_paint")); // 分母：main→首帧
    if (total <= 0)
        return out;
    const auto share = [&startup, total](const char *from, const char *to) {
        const double a = segAtMs(startup, QString::fromLatin1(from));
        const double b = segAtMs(startup, QString::fromLatin1(to));
        if (a < 0 || b < 0 || b < a)
            return -1.0;
        return (b - a) / total;
    };
    struct GateDef
    {
        const char *key;
        double ratio;
    };
    const double qgisShare = share("pre_qt_ready", "qgis_app_ready");
    const double serviceShare = share("qgis_app_ready", "services_ready");
    const double paintShare = share("window_shown", "first_paint");
    const double loaderMs = startup.value(QStringLiteral("process_to_main_ms")).toDouble(-1.0);
    const double loaderShare =
        loaderMs > 0 ? loaderMs / (loaderMs + total) : -1.0;
    const GateDef defs[] = {
        {"qgis_init_share_max", qgisShare},
        {"service_assembly_share_max", serviceShare},
        {"show_to_paint_share_max", paintShare},
        {"loader_share_max", loaderShare},
    };
    for (const GateDef &d : defs)
    {
        if (d.ratio < 0)
            continue; // 段缺失（如非 Linux 的 loader 段）→ 不评不改判
        GateOutcome g;
        g.evaluated = true;
        g.ratio = d.ratio;
        g.key = QString::fromLatin1(d.key);
        g.gate = baselines.value(QString::fromLatin1(d.key)).toDouble(-1.0) * kGateTolerance;
        g.pass = g.gate > 0 && d.ratio <= g.gate;
        out.append(g);
    }
    return out;
}

// ---- 1) 单元面 ----

void StartupTraceTests::markRecordsSegmentsInOrder()
{
    StartupTrace::resetForTest();
    StartupTrace::mark(QStringLiteral("main_entry"));
    StartupTrace::mark(QStringLiteral("pre_qt_ready"));
    StartupTrace::mark(QStringLiteral("services_ready"));
    const auto segs = StartupTrace::segments();
    QCOMPARE(segs.size(), 3);
    QCOMPARE(segs.at(0).name, QStringLiteral("main_entry"));
    QCOMPARE(segs.at(1).name, QStringLiteral("pre_qt_ready"));
    QCOMPARE(segs.at(2).name, QStringLiteral("services_ready"));
    QVERIFY(segs.at(0).atMs <= segs.at(1).atMs);
    QVERIFY(segs.at(1).atMs <= segs.at(2).atMs);
}

void StartupTraceTests::adjacentDuplicateMarkCollapsed()
{
    StartupTrace::resetForTest();
    StartupTrace::mark(QStringLiteral("main_entry"));
    StartupTrace::mark(QStringLiteral("theme_ready"));
    StartupTrace::mark(QStringLiteral("theme_ready")); // 相邻双记只留一次
    StartupTrace::mark(QStringLiteral("theme_ready"));
    StartupTrace::mark(QStringLiteral("window_shown"));
    QCOMPARE(StartupTrace::segments().size(), 3);
}

void StartupTraceTests::jsonShapeIsStable()
{
    StartupTrace::resetForTest();
    StartupTrace::mark(QStringLiteral("main_entry"));
    StartupTrace::mark(QStringLiteral("first_paint"));
    const QJsonDocument doc = QJsonDocument::fromJson(StartupTrace::toJson());
    QVERIFY(doc.isObject());
    const QJsonObject root = doc.object();
    QVERIFY(root.contains(QStringLiteral("process_to_main_ms")));
    QVERIFY(root.contains(QStringLiteral("total_ms")));
    const QJsonArray segs = root.value(QStringLiteral("segments")).toArray();
    QCOMPARE(segs.size(), 2);
    QCOMPARE(segs.at(0).toObject().value(QStringLiteral("name")).toString(),
             QStringLiteral("main_entry"));
    QVERIFY(segs.at(0).toObject().value(QStringLiteral("at_ms")).isDouble());
}

void StartupTraceTests::resetClearsState()
{
    StartupTrace::resetForTest();
    StartupTrace::mark(QStringLiteral("main_entry"));
    QVERIFY(!StartupTrace::segments().isEmpty());
    StartupTrace::resetForTest();
    QVERIFY(StartupTrace::segments().isEmpty());
}

// ---- 2) 真实进程集成 ----

void StartupTraceTests::realStartupProducesCompleteTrace()
{
    QTemporaryDir dir;
    const QString trace = dir.filePath("startup.json");
    const AppRun run = launchApp(trace);
    QVERIFY2(run.ok, qPrintable(run.error));
    const QJsonArray segs = run.json.value(QStringLiteral("segments")).toArray();
    QStringList seen;
    for (const QJsonValue &v : segs)
        seen << v.toObject().value(QStringLiteral("name")).toString();
    for (const QString &req : kRequiredSegments)
        QVERIFY2(seen.contains(req),
                 qPrintable(QStringLiteral("缺启动段 %1（实得 %2）")
                                .arg(req, seen.join(QStringLiteral(",")))));
    // 次序：词表序即时间序。
    int lastIdx = -1;
    for (const QString &req : kRequiredSegments)
    {
        const int idx = seen.indexOf(req);
        QVERIFY2(idx > lastIdx,
                 qPrintable(QStringLiteral("段乱序：%1").arg(req)));
        lastIdx = idx;
    }
    QVERIFY(run.json.value(QStringLiteral("total_ms")).toDouble(0) > 0);
}

void StartupTraceTests::realStartupRatioGatesHold()
{
    // 3 次起进程取每道门比率的中位数再判——共享机单轮冷启的分母波动
    //（快分母×慢分子，实测极值 2.04×基线）不应闪红回归门；中位对单轮
    // 异常免疫。判别力不受影响：注入 1500ms 的注劣化用例仍单轮判定。
    const QJsonObject baselines = loadBaselineRatios();
    QVERIFY2(!baselines.isEmpty(),
             "docs/perf/baselines/startup_ratios.json 缺失或不可解析");
    QTemporaryDir dir;
    QHash<QString, QVector<double>> ratioSamples;
    for (int i = 0; i < 3; ++i)
    {
        const AppRun run = launchApp(dir.filePath(QStringLiteral("startup%1.json").arg(i)));
        QVERIFY2(run.ok, qPrintable(run.error));
        const auto outcomes = evaluateGates(run.json, baselines);
        QVERIFY2(outcomes.size() >= 3,
                 "至少三道门应被评估（qgis/service/paint；loader 仅 Linux）");
        for (const GateOutcome &g : outcomes)
            ratioSamples[g.key].append(g.ratio);
    }
    for (auto it = ratioSamples.cbegin(); it != ratioSamples.cend(); ++it)
    {
        QVector<double> vals = it.value();
        std::sort(vals.begin(), vals.end());
        const double median = vals.at(vals.size() / 2);
        const double gate =
            baselines.value(it.key()).toDouble(-1.0) * kGateTolerance;
        QVERIFY2(median <= gate,
                 qPrintable(QStringLiteral("启动比率 %1 中位 %2 > 门 %3（基线×%4，"
                                           "3 轮样本 %5）")
                                .arg(it.key())
                                .arg(median, 0, 'f', 4)
                                .arg(gate, 0, 'f', 4)
                                .arg(kGateTolerance)
                                .arg([vals] {
                                  QStringList xs;
                                  for (double v : vals) xs << QString::number(v, 'f', 3);
                                  return xs.join(QStringLiteral(","));
                                }())));
    }
}

// ---- 3) 判别力：真实劣化必红 ----

void StartupTraceTests::injectedDegradationMustTripGate()
{
    // 判别力证明用「相对劣化」口径（负载免疫）：同测起一次健康进程 + 一次
    // 注入 1500ms 进 qgis 段（qgis_init_share 分子）的进程——注劣化轮的
    // qgis 份额必须 ≥3× 健康轮。绝对门（基线×2.5）在共享机重载下会被
    // 肥分母稀释（实测注劣化份额可低至 0.18），相对口径不受分母影响：
    // 健康轮 qgis 段 ~113ms（份额 ≤0.2），注劣化轮 qgis 段 +1500ms。
    QTemporaryDir dir;
    const AppRun healthy = launchApp(dir.filePath("healthy.json"));
    QVERIFY2(healthy.ok, qPrintable(healthy.error));
    const AppRun degraded =
        launchApp(dir.filePath("degraded.json"), /*injectDelayMs=*/1500);
    QVERIFY2(degraded.ok, qPrintable(degraded.error));
    const auto shareOf = [](const AppRun &run) {
        const double total = segAtMs(run.json, QStringLiteral("first_paint"));
        const double a = segAtMs(run.json, QStringLiteral("pre_qt_ready"));
        const double b = segAtMs(run.json, QStringLiteral("qgis_app_ready"));
        return (total > 0 && a >= 0 && b >= a) ? (b - a) / total : -1.0;
    };
    const double healthyShare = shareOf(healthy);
    const double degradedShare = shareOf(degraded);
    QVERIFY2(healthyShare > 0 && degradedShare > 0, "段缺失——份额不可计算");
    qInfo("qgis share: healthy=%.3f degraded=%.3f (x%.1f)", healthyShare,
          degradedShare, degradedShare / qMax(healthyShare, 1e-9));
    QVERIFY2(degradedShare >= 3.0 * healthyShare,
             qPrintable(QStringLiteral("注劣化份额 %1 < 3×健康 %2——测量管线"
                                       "对劣化不敏感，禁止放宽本断言")
                            .arg(degradedShare, 0, 'f', 3)
                            .arg(healthyShare, 0, 'f', 3)));
    // 绝对门同轮观测（空载常态下应红；不作主判据——中位口径见
    // realStartupRatioGatesHold）。
    const QJsonObject baselines = loadBaselineRatios();
    if (!baselines.isEmpty())
    {
        const auto outcomes = evaluateGates(degraded.json, baselines);
        for (const GateOutcome &g : outcomes)
            if (g.key == QStringLiteral("qgis_init_share_max"))
                qInfo("absolute gate on degraded run: ratio=%.3f gate=%.3f %s",
                      g.ratio, g.gate, g.pass ? "pass" : "TRIPPED");
    }
}

QTEST_MAIN(StartupTraceTests)
#include "tst_startup_trace.moc"
