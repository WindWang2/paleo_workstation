// 层：视图
// 方向 65：goal/horizon-autotrack 种子追踪 + goal/horizon-3d 层位体传播。
// 两段都走 QPointer 迟到回调守卫；追踪合并替换是一步 undo（ReplacePicksCommand）。
#include "ui/seismicsection/seismicsectiondock_internal.h"

#include "domain/seismic/sectionaxis.h"

#include "catalog/datacatalog.h"
#include "services/seismictaskservice.h"

namespace seismic {

void SeismicSectionDockWidget::runTracking() {
    // goal/horizon-autotrack：多种子异步追踪（原 D4.2 单种子同步升级）。
    // 种子集 = 当前剖面、同层位的手动拾取（conf==1，D4.10 语义）；种子
    // pick 缺席时回落最后一个拾取。
    if (m_trackTask)
        return; // 在途中不重复触发（取消走 cancelTracking）
    const SeismicPick *seed = m_session.pickById(m_trackSeedPick);
    if (!seed && !m_session.picks.isEmpty())
        seed = &m_session.picks.last();
    if (!seed || m_lastSlice.values.empty())
        return;

    const SectionRef ref = m_canvas->sectionRef();
    if (!ref.valid)
        return;
    // 按值捕获（异步回调点火时本函数栈已退——引用捕获会悬垂）
    const auto onSection = [ref](const SeismicPick &p) {
        return ref.type == SgySliceType::Inline ? p.inlineNo == ref.index
                                                : p.xlineNo == ref.index;
    };
    const auto colOf = [ref](const SeismicPick &p) {
        return sectionColumnForLine(ref.colLines, ref.colMin,
                                    ref.type == SgySliceType::Inline ? p.xlineNo : p.inlineNo);
    };
    const QString interpreter = seed->interpreter;
    const QString horizon = seed->horizonName;
    const double dtMsSeed = m_volume && m_volume->SampleIntervalUs() > 0
        ? m_volume->SampleIntervalUs() / 1000.0 : 2.0;
    QList<QPair<int, int>> seeds;
    for (const SeismicPick &p : m_session.picks)
        if (p.horizonName == horizon && p.confidence == 1.0f && onSection(p))
        {
            const int col = colOf(p);
            // #146：种子样点按 TWT 重算（旧会话 sampleIndex 可能未扣记录延迟）。
            const int sample = sectionSampleForTwt(p.twtMs, ref.t0Ms, dtMsSeed);
            if (col >= 0 && col < m_lastSlice.width && sample >= 0 && sample < m_lastSlice.height)
                seeds.append({col, sample});
        }
    if (seeds.isEmpty())
        return;

    const float dtMs = m_volume && m_volume->SampleIntervalUs() > 0
        ? m_volume->SampleIntervalUs() / 1000.0f : 2.0f;
    if (m_pickPanel)
        m_pickPanel->setTrackingActive(true);
    QPointer<SeismicSectionDockWidget> guard(this); // 注入共享服务时迟到回调守卫
    // #285：会话身份快照——换体后 m_session.sourceSgyPath 已指向新体的
    // 伴生文件，迟到回调必须丢弃（否则 A 体剖面上追出的拾取被并入 B 体
    // 会话并自动落盘）。按会话路径而非世代号守卫：同体内换线不丢结果。
    const QString sessionPath = m_session.sourceSgyPath;
    SeismicTrackOptions trackOptions = m_trackOptions;
    trackOptions.columnLines = ref.colLines; // #147
    trackOptions.startTimeMs = ref.t0Ms;     // #146
    m_trackTask = m_taskService->startHorizonTracking(
        m_lastSlice, ref.type, ref.index, ref.colMin, ref.colMax, seeds,
        trackOptions, interpreter, horizon, dtMs,
        [this, guard, ref, horizon, colOf, onSection, sessionPath](bool ok, const QList<SeismicPick> &picks,
                                                      const SeismicTrackReport &report,
                                                      const QString &error) {
            if (!guard)
                return; // dock 已亡（取消后迟到回调）：丢弃
            // #285：换体后迟到丢弃——m_session 已锚到新体伴生文件，A 体
            // 追出的拾取不得并入 B 会话并自动落盘；陈旧完成报告同样不得
            // 闪现面板（结果已丢弃，报了反而误导）。
            if (m_session.sourceSgyPath != sessionPath)
                return;
            m_trackTask = nullptr;
            m_lastTrackReport = report;
            if (m_pickPanel)
            {
                m_pickPanel->setTrackingActive(false);
                if (ok)
                    m_pickPanel->showTrackReport(report);
                else if (!error.isEmpty())
                    m_pickPanel->showTrackError(error);
            }
            if (!ok)
            {
                emit trackingFinished(false);
                return;
            }
            // 合并替换（一步 undo）：同列新旧机器拾取取高置信；手动列不动
            QHash<int, const SeismicPick *> machineByCol;
            QSet<int> manualCols;
            for (const SeismicPick &p : m_session.picks)
            {
                if (p.horizonName != horizon || !onSection(p))
                    continue;
                const int col = colOf(p);
                if (col < 0 || col >= m_lastSlice.width)
                    continue;
                if (p.confidence == 1.0f)
                    manualCols.insert(col);
                else if (!machineByCol.contains(col))
                    machineByCol.insert(col, &p);
            }
            QList<SeismicPick> removed, added;
            for (const SeismicPick &p : picks)
            {
                const int col = colOf(p);
                if (manualCols.contains(col))
                    continue; // 手动优先（含种子列）：不覆盖不重复
                const auto it = machineByCol.constFind(col);
                if (it != machineByCol.constEnd())
                {
                    if (p.confidence > (*it)->confidence)
                    {
                        removed << *(*it);
                        added << p;
                    }
                }
                else
                {
                    added << p;
                }
            }
            if (!removed.isEmpty() || !added.isEmpty())
                m_undoStack->push(new ReplacePicksCommand(this, removed, added));
            emit trackingFinished(true);
        });
}

void SeismicSectionDockWidget::cancelTracking() {
    if (m_trackTask)
        m_trackTask->requestCancel();
}

// ---- goal/horizon-3d 层位 3D 体传播 ---------------------------------------

SeismicSectionDockWidget::PropagationReadiness
SeismicSectionDockWidget::propagationReadiness(QString *reason) const {
    if (!m_volume || !m_volume->IsLoaded()) {
        if (reason)
            *reason = tr("无可用地震体（先加载 SEG-Y）");
        return PropagationReadiness::NoVolume;
    }
    const SectionRef ref = m_canvas ? m_canvas->sectionRef() : SectionRef{};
    if (!ref.valid || ref.type != SgySliceType::Inline) {
        if (reason)
            *reason = tr("3D 传播需以 inline 剖面为种子剖面（当前非 inline 切片/任意线）");
        return PropagationReadiness::NotInlineSection;
    }
    const QString horizon = m_pickPanel ? m_pickPanel->currentHorizon() : QString();
    const auto hasSeed = [ref, horizon](const SeismicPick &p) {
        return p.inlineNo == ref.index &&
               (horizon.isEmpty() || p.horizonName == horizon) &&
               p.confidence == 1.0f; // 手动拾取为种子（机器拾取不自带起点权威）
    };
    for (const SeismicPick &p : m_session.picks)
        if (hasSeed(p)) {
            if (reason)
                reason->clear();
            return PropagationReadiness::Ready;
        }
    if (reason)
        *reason = tr("种子剖面无当前层位的手动拾取（先 Ctrl+左键 拾取）");
    return PropagationReadiness::NoSeeds;
}

void SeismicSectionDockWidget::runVolumePropagation() {
    // goal/horizon-3d：种子剖面（当前 inline）手动拾取 → IL 全体滑窗扩散
    // → 层位面资产（DERIVED 锚源体版本）+ GeoTIFF 上图声明。结果不进会话
    // 拾取表——全体拾取集是图件级对象，非单剖面编辑对象（QC 走报告/图层）。
    if (m_propTask)
        return; // 在途中不重复触发（取消走 cancelVolumePropagation）
    QString reason;
    if (propagationReadiness(&reason) != PropagationReadiness::Ready) {
        if (m_pickPanel)
            m_pickPanel->showTrackError(reason);
        emit propagationFinished(false);
        return;
    }
    const SectionRef ref = m_canvas->sectionRef();
    const QString horizon = m_pickPanel && !m_pickPanel->currentHorizon().isEmpty()
                                ? m_pickPanel->currentHorizon()
                                : QStringLiteral("H1");
    QString interpreter;
    const double dtMsSeed = m_volume->SampleIntervalUs() > 0
                                ? m_volume->SampleIntervalUs() / 1000.0 : 2.0;
    SeismicTaskService::VolumePropagationRequest req;
    req.seedInline = ref.index;
    for (const SeismicPick &p : m_session.picks) {
        if (p.inlineNo != ref.index ||
            (!horizon.isEmpty() && p.horizonName != horizon) ||
            p.confidence != 1.0f)
            continue;
        // #146：种子样点按 TWT 重算（旧会话 sampleIndex 可能未扣记录延迟）
        const int sample = sectionSampleForTwt(p.twtMs, ref.t0Ms, dtMsSeed);
        if (sample >= 0 && sample < m_volume->SampleCount())
            req.seeds.append({p.xlineNo, sample}); // 种子域 = (xline 线号, 采样)
        if (interpreter.isEmpty())
            interpreter = p.interpreter;
    }
    if (req.seeds.isEmpty()) {
        if (m_pickPanel)
            m_pickPanel->showTrackError(tr("种子拾取无法换算到采样域（TWT 越界）"));
        emit propagationFinished(false);
        return;
    }
    req.windowSamples = m_trackOptions.windowSamples;
    req.maxSearchSamples = m_trackOptions.maxSearchSamples;
    req.correlationThreshold = m_trackOptions.correlationThreshold;
    req.dipHistoryPicks = m_trackOptions.dipHistoryPicks;
    req.startTimeMs = ref.t0Ms; // #146 记录延迟入 TWT 域
    req.interpreter = interpreter.isEmpty() ? tr("解释") : interpreter;
    req.horizonName = horizon;

    if (m_pickPanel)
        m_pickPanel->setPropagateActive(true);
    QPointer<SeismicSectionDockWidget> guard(this); // 迟到回调守卫（同 runTracking）
    // #285：会话身份快照——换体后迟到丢弃，层位面资产不得登记到旧体谱系。
    const QString sessionPath = m_session.sourceSgyPath;
    m_propTask = m_taskService->startVolumePropagation(
        m_volume, req,
        [this, guard, horizon, sessionPath](bool ok, const QList<SeismicPick> &picks,
                               const SeismicTrackReport &report,
                               const QString &error) {
            if (!guard)
                return; // dock 已亡：丢弃
            // #285：换体后迟到丢弃——层位面资产不得登记到旧体谱系，陈旧
            // 完成报告同样不得闪现面板（m_propTask 已由 setVolume 摘牌取消）。
            if (m_session.sourceSgyPath != sessionPath)
                return;
            m_propTask = nullptr;
            if (m_pickPanel) {
                m_pickPanel->setPropagateActive(false);
                if (ok)
                    m_pickPanel->showTrackReport(report);
                else if (!error.isEmpty())
                    m_pickPanel->showTrackError(error);
            }
            if (!ok) {
                emit propagationFinished(false);
                return;
            }
            // DERIVED 资产登记（catalog 未注入 = 无登记上下文，如实报出）
            if (m_catalog) {
                LayerDeclaration decl;
                QString err;
                const QString path = SeismicTaskService::registerPropagatedHorizonAsset(
                    m_catalog, m_catalogAssetId, m_catalogVersionId, horizon,
                    picks, m_interpretationDir, &err, &decl);
                if (!path.isEmpty()) {
                    if (!decl.layerId.isEmpty())
                        emit horizonLayerDeclared(decl); // 上图（app 装配接 declare）
                } else if (m_pickPanel) {
                    m_pickPanel->showTrackError(tr("层位资产登记失败：%1").arg(err));
                }
            } else if (m_pickPanel) {
                m_pickPanel->showTrackError(
                    tr("追踪完成，但数据目录未连接，层位面未登记上图"));
            }
            emit propagationFinished(true);
        });
}

void SeismicSectionDockWidget::cancelVolumePropagation() {
    if (m_propTask)
        m_propTask->requestCancel();
}

} // namespace seismic
