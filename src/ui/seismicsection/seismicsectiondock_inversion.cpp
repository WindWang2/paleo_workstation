// 层：视图
// 方向 65：goal/seismic-inversion 反演控制器族——最近井子波提取（#125 逐井时深）
// 与 InversionWorkflow 三段式反演（世代号丢弃陈旧发布）。
#include "ui/seismicsection/seismicsectiondockwidget.h"
#include "ui/seismicsection/inversionpanel.h"

#include "catalog/datacatalog.h"
#include "workflow/derivedassets.h"
#include "workflow/inversionworkflow.h"
#include "workflow/wellimpedancetwt.h"
#include "services/seismictaskservice.h"

#include <QDir>
#include <QFile>
#include <QIODevice>

namespace seismic {

namespace {

// #125：井 AC×DEN → TWT 域阻抗散样。TWT 取曲线自带的逐井时间轴（SectionWorkbench
// 按该井时深表/常速近似 + 平移算好，与剖面井叠加同口径），不用画布全局模型重算；
// 时深无效的井返回 false（调用方跳过并提示），不补默认速度。
bool wellTimeDepthTable(const WellCurveItem *ac, const WellCurveItem *den,
                        std::vector<double> *tdDepthM, std::vector<double> *tdTwtMs)
{
    std::vector<std::pair<double, double>> pairs;
    paleo::inv::appendTimeDepthPairs(ac->depthsM, ac->twtMs, &pairs);
    paleo::inv::appendTimeDepthPairs(den->depthsM, den->twtMs, &pairs);
    return paleo::inv::buildTimeDepthTable(std::move(pairs), tdDepthM, tdTwtMs);
}

bool wellImpedanceTwt(const WellCurveItem *ac, const WellCurveItem *den,
                      std::vector<double> *twtMs, std::vector<float> *impedance)
{
    std::vector<double> tdD, tdT;
    if (!wellTimeDepthTable(ac, den, &tdD, &tdT))
        return false;
    return paleo::inv::wellImpedanceTwt(ac->depthsM, ac->values, den->depthsM, den->values, tdD, tdT,
                                        twtMs, impedance);
}

} // namespace

// 「提取子波（最近井）」：AC×DEN+时深+井旁道 → Wiener LS 子波 → DERIVED
// wavelet 资产（catalog 未注入时落临时文件并如实说明）。
void SeismicSectionDockWidget::extractWaveletFromNearestWell() {
    if (!m_volume || !m_volume->IsLoaded()) {
        if (m_invPanel)
            m_invPanel->showResult(false, tr("地震体未加载（先打开 SEG-Y）"));
        return;
    }
    if (m_candidateWells.empty()) {
        if (m_invPanel)
            m_invPanel->showResult(false, tr("剖面缓冲带内无候选井"));
        return;
    }
    const SectionWellInfo *best = nullptr;
    for (const SectionWellInfo &w : m_candidateWells)
        if (!best || w.offsetDistanceM < best->offsetDistanceM)
            best = &w;
    const WellCurveItem *ac = nullptr;
    const WellCurveItem *den = nullptr;
    for (const WellCurveItem &c : best->curves) {
        if (c.curveName == QLatin1String("AC") && !c.values.empty())
            ac = &c;
        if ((c.curveName == QLatin1String("DEN") || c.curveName == QLatin1String("RHOB")) &&
            !c.values.empty())
            den = &c;
    }
    if (!ac || !den) {
        if (m_invPanel)
            m_invPanel->showResult(
                false, tr("井 %1 缺 %2 曲线").arg(best->wellId, !ac ? QStringLiteral("AC") : QStringLiteral("DEN")));
        return;
    }

    // 时深表（#125）：取该井曲线自带的逐井 TWT（含时间平移），不用画布全局模型。
    std::vector<double> tdDepthM, tdTwtMs;
    if (!wellTimeDepthTable(ac, den, &tdDepthM, &tdTwtMs)) {
        if (m_invPanel)
            m_invPanel->showResult(false, tr("井 %1 缺有效时深（未对齐），不能提取子波").arg(best->wellId));
        return;
    }
    paleo::inv::InversionWorkflow wf;
    paleo::inv::WaveletJobRequest req;
    req.wellId = best->wellId;
    req.wellX = best->surfaceX;
    req.wellY = best->surfaceY;
    req.acDepthsM = ac->depthsM;
    req.acUsPerM = ac->values;
    req.denDepthsM = den->depthsM;
    req.denValues = den->values;
    req.tdDepthM = std::move(tdDepthM);
    req.tdTimeMs = std::move(tdTwtMs);
    req.seismicPath = m_session.sourceSgyPath;
    req.parentVersionId = m_catalogVersionId;

    paleo::inv::InversionWorkflow::WaveletJob job;
    QString err;
    if (!wf.prepareWaveletJob(req, &job, &err) || !wf.computeWaveletJob(&job)) {
        if (m_invPanel)
            m_invPanel->showResult(false, err.isEmpty() ? job.error : err);
        return;
    }

    QString waveletPath;
    if (m_catalog) {
        DerivedAssetRegistrar registrar(m_catalog, m_interpretationDir);
        const QString versionId = wf.publishWaveletJob(job, registrar, &err);
        if (!versionId.isEmpty()) {
            for (const CatalogVersion &v : m_catalog->versions()) {
                if (v.id == versionId)
                    waveletPath = QDir(m_interpretationDir).filePath(v.path);
            }
        }
    }
    if (waveletPath.isEmpty()) {
        // catalog 未注入或发布失败：临时文件承载（如实说明，不进谱系）。
        waveletPath = QDir(QDir::temp()).filePath(QStringLiteral("paleo-wavelet-%1.json").arg(best->wellId));
        QFile f(waveletPath);
        if (f.open(QIODevice::WriteOnly))
            f.write(paleo::inversion::waveletToJson(job.wavelet));
    }
    if (m_invPanel) {
        m_invPanel->setWaveletPath(waveletPath);
        m_invPanel->showResult(
            true, tr("子波已提取（%1）：主频 %2 Hz，拟合相关 %3")
                      .arg(job.nearestTraceNote)
                      .arg(job.wavelet.dominantFreqHz(), 0, 'f', 1)
                      .arg(job.fitCorrelation, 0, 'f', 3));
    }
}

// 「反演」：InversionWorkflow 三段式。prepare+compute 在任务线程，发布回
// GUI 线程（DERIVED 登记）；世代号丢弃陈旧发布。
void SeismicSectionDockWidget::runInversion(const InversionPanelParams &params) {
    if (!m_invPanel)
        return;
    if (!m_taskService) {
        m_invPanel->showResult(false, tr("任务服务未注入"));
        return;
    }
    if (!m_volume || !m_volume->IsLoaded()) {
        m_invPanel->showResult(false, tr("地震体未加载（先打开 SEG-Y）"));
        return;
    }
    if (params.waveletPath.isEmpty() || !QFile::exists(params.waveletPath)) {
        m_invPanel->showResult(false, tr("子波文件无效（先「提取子波」或浏览已有资产）"));
        return;
    }

    // 低频井输入：候选井 AC×DEN → TWT 阻抗散样（无层位 → 全局趋势回退，
    // 语义如实记入产物 extra.layersUsed）。
    std::vector<paleo::inv::InversionWorkflow::LowFreqWellInput> wells;
    QStringList skippedNoTd;
    for (const SectionWellInfo &w : m_candidateWells) {
        const WellCurveItem *ac = nullptr;
        const WellCurveItem *den = nullptr;
        for (const WellCurveItem &c : w.curves) {
            if (c.curveName == QLatin1String("AC") && !c.values.empty())
                ac = &c;
            if ((c.curveName == QLatin1String("DEN") || c.curveName == QLatin1String("RHOB")) &&
                !c.values.empty())
                den = &c;
        }
        if (!ac || !den)
            continue;
        paleo::inv::InversionWorkflow::LowFreqWellInput lw;
        lw.wellId = w.wellId;
        lw.x = w.surfaceX;
        lw.y = w.surfaceY;
        if (!wellImpedanceTwt(ac, den, &lw.twtMs, &lw.impedance)) {
            skippedNoTd << w.wellId; // 缺有效逐井时深：不参与低频模型（#125）
            continue;
        }
        wells.push_back(std::move(lw));
    }

    auto request = std::make_shared<paleo::inv::InversionWorkflow::InversionJobRequest>();
    request->seismicPath = m_session.sourceSgyPath;
    request->waveletPath = params.waveletPath;
    request->method = params.method;
    request->lowCutHz = params.lowCutHz;
    request->lambda = params.lambda;
    request->maxIterations = params.maxIterations;
    request->lowFreqWells = std::move(wells);
    request->displayName = params.method == QStringLiteral("sparse")
                               ? tr("波阻抗体·稀疏脉冲")
                               : tr("波阻抗体·带限道积分");
    if (!m_session.sourceSgyPath.isEmpty())
        request->parentPaths = QStringList{m_session.sourceSgyPath, params.waveletPath};

    auto job = std::make_shared<paleo::inv::InversionWorkflow::InversionJob>();
    const quint64 gen = ++m_invGeneration;
    m_invPanel->setBusy(true);
    m_invTask = m_taskService->startBounded(
        tr("地震反演"),
        [request, job](PaleoTask *task) -> QString {
            if (task->cancelRequested())
                return tr("已取消");
            task->reportStage(QStringLiteral("build"), 2);
            QString err;
            if (!paleo::inv::InversionWorkflow().prepareInversionJob(*request, job.get(), &err))
                return err;
            task->reportStage(QStringLiteral("decode"), 5);
            if (!paleo::inv::InversionWorkflow().computeInversionJob(
                    job.get(), [task] { return task->cancelRequested(); },
                    [task](double f) {
                        if (task)
                            task->reportStage(QStringLiteral("decode"),
                                              5 + int(f * 90.0));
                    }))
                return job->error;
            return QString();
        },
        QString(), /*quiet=*/false);

    if (!m_invTask)
        return;
    connect(m_invTask, &PaleoTask::changed, this, [this]() {
        if (m_invPanel && m_invTask && m_invTask->running())
            m_invPanel->updateProgress(m_invTask->stagePercent() < 0 ? 0 : m_invTask->stagePercent(),
                                       m_invTask->stage());
    });
    connect(m_invTask, &PaleoTask::finished, this, [this, gen, job, skippedNoTd]() {
        if (!m_invPanel || !m_invTask)
            return;
        if (gen != m_invGeneration) {
            m_invPanel->showResult(false, tr("已有更新的反演请求，本次结果丢弃"));
            return;
        }
        if (m_invTask->state() == PaleoTask::State::Cancelled) {
            m_invPanel->showResult(false, tr("反演已取消"));
            return;
        }
        if (m_invTask->state() != PaleoTask::State::Succeeded || !job->ok) {
            m_invPanel->showResult(false,
                                   job->error.isEmpty() ? m_invTask->errorText() : job->error);
            return;
        }
        if (!m_catalog) {
            m_invPanel->showResult(false, tr("反演完成但 catalog 未注入，无法登记（体在 %1）")
                                             .arg(job->tempVolumePath));
            return;
        }
        DerivedAssetRegistrar registrar(m_catalog, m_interpretationDir);
        QString pubErr;
        const auto pub = paleo::inv::InversionWorkflow().publishInversionJob(*job, registrar, &pubErr);
        if (!pub.ok) {
            m_invPanel->showResult(false, pubErr);
            return;
        }
        m_invPanel->showResult(
            true,
            tr("反演完成：体 %1×%2×%3，处理 %4 道（失败 %5）｜频段 0–%6Hz+带限｜%7")
                .arg(job->nIl)
                .arg(job->nXl)
                .arg(job->nS)
                .arg(job->processedTraces)
                .arg(job->failedTraces)
                .arg(job->req.lowCutHz)
                .arg(job->req.method == QStringLiteral("sparse")
                         ? tr("平均残差能量比 %1").arg(job->meanResidualEnergyRatio, 0, 'f', 3)
                         : tr("低频方差占比 %1").arg(job->meanLowFreqVarianceFraction, 0, 'f', 3))
                + (job->wavelet.amplitudeScale > 0.0
                       ? QString()
                       : tr("｜子波无振幅标定（旧资产/解析子波），道振幅按反射系数直接使用"))
                + (skippedNoTd.isEmpty()
                       ? QString()
                       : tr("｜缺有效时深未参与低频模型：%1").arg(skippedNoTd.join(QStringLiteral("、")))));
    });
}

} // namespace seismic
