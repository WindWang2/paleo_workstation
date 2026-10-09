// 层：视图
// 方向 65：D4 解释会话 TU——拾取/断层的增删改（全部经 undo 栈）、FaultSet 棒
// 回显、层位/断层资产登记。手动拾取 conf==1 的语义在这里落地。
#include "ui/seismicsection/seismicsectiondock_internal.h"
#include "ui/seismicsection/seismicattrpanel.h"

#include "domain/seismic/sectionaxis.h"
#include "workflow/faultinterpretationcontroller.h"

#include "catalog/datacatalog.h"
#include "services/seismictaskservice.h"

namespace seismic {

QString SeismicSectionDockWidget::sessionFilePath() const {
    return m_session.sourceSgyPath.isEmpty() ? QString()
        : m_session.sourceSgyPath + QStringLiteral(".seispicks.json");
}

void SeismicSectionDockWidget::setInterpretationCatalog(DataCatalog *catalog,
                                                        const QString &assetId,
                                                        const QString &versionId,
                                                        const QString &outputDir) {
    m_catalog = catalog;
    m_catalogAssetId = assetId;
    m_catalogVersionId = versionId;
    m_interpretationDir = outputDir;
}

// #236：工程边界复位（resetProjectScopedState 调）——旧工程的解释态全部
// 作废：在途属性/预览任务取消（迟到回调经体路径守卫丢弃）、会话与 undo 栈
// 清空、可登记属性结果清空（防登记错配）、书签下拉清空（书签按体 settings
// 键存，旧体条目不得残留下拉）、属性叠加清除。
void SeismicSectionDockWidget::resetInterpretationState() {
    if (m_attrTask) {
        m_attrTask->requestCancel();
        m_attrTask.clear();
    }
    if (m_attrPreviewTask) {
        m_attrPreviewTask->requestCancel();
        m_attrPreviewTask.clear();
    }
    ++m_attrPreviewGen; // 迟到预览回调随世代跳号丢弃
    m_session = SeismicInterpretationSession{};
    if (m_undoStack)
        m_undoStack->clear();
    m_lastAttrResult = {};
    m_lastAttrParams = {};
    m_lastAttrSourcePath.clear();
    if (m_attrPanel)
        m_attrPanel->clearResult();
    m_bookmarks.clear();
    if (m_cboBookmark)
        m_cboBookmark->clear();
    if (m_canvas)
        m_canvas->clearAttrOverlay();
    refreshInterpretationOverlay(); // 会话已空 → 拾取/断层棒叠加全清
}

SeismicInterpretationSession &SeismicSectionDockWidget::mutableSession() {
    return m_session; // undo 命令的写入口（友元路径）
}

void SeismicSectionDockWidget::refreshInterpretationOverlay() {
    m_canvas->setPickOverlays(m_session.picks, m_session.faults);
    if (m_pickPanel)
        m_pickPanel->refreshFromSession();
    refreshFaultStickOverlay(); // FaultSet 棒随会话刷新同拍更新
}

void SeismicSectionDockWidget::setPickMode(SectionPickMode mode) {
    m_canvas->setPickMode(mode);
    if (m_pickPanel)
        m_pickPanel->setVisible(mode != SectionPickMode::None);
    if (mode != SectionPickMode::None && m_session.interpreters.isEmpty()) {
        m_session.interpreters << QStringLiteral("解释员A");
        if (m_pickPanel)
            m_pickPanel->refreshFromSession();
    }
}

void SeismicSectionDockWidget::addPickFromCanvas(int traceCol, double twtMs) {
    const SectionRef ref = m_canvas->sectionRef();
    if (!ref.valid || !m_volume || !m_volume->IsLoaded())
        return;
    const float dtMs = m_volume->SampleIntervalUs() > 0
        ? m_volume->SampleIntervalUs() / 1000.0f : 2.0f;
    SeismicPick pick;
    // #146：画布 TWT 含记录延迟 t0，样点号相对剖面首样。
    pick.sampleIndex = sectionSampleForTwt(twtMs, ref.t0Ms, dtMs);
    if (pick.sampleIndex < 0)
        return;
    pick.twtMs = sectionTwtForSample(pick.sampleIndex, ref.t0Ms, dtMs);
    // #147：列 → 实际测线号（非单位线距不能 colMin+col）。
    int lineNo = 0;
    if (!sectionLineForColumn(ref.colLines, ref.colMin, traceCol, &lineNo))
        return;
    if (ref.type == SgySliceType::Inline) {
        pick.inlineNo = ref.index;
        pick.xlineNo = lineNo;
    } else if (ref.type == SgySliceType::Xline) {
        pick.xlineNo = ref.index;
        pick.inlineNo = lineNo;
    } else {
        return; // 时间切片不拾取（水平向无 TWT 概念）
    }
    pick.confidence = 1.0f;
    pick.interpreter = m_pickPanel ? m_pickPanel->currentInterpreter() : QString();
    pick.horizonName = m_pickPanel ? m_pickPanel->currentHorizon() : QStringLiteral("H1");
    if (pick.interpreter.isEmpty())
        pick.interpreter = QStringLiteral("解释员A");
    if (!m_session.interpreters.contains(pick.interpreter))
        m_session.interpreters << pick.interpreter;
    if (pick.horizonName.isEmpty())
        pick.horizonName = QStringLiteral("H1");

    m_undoStack->push(new AddPicksCommand(this, {pick}));
    refreshInterpretationOverlay();
}

void SeismicSectionDockWidget::addPicks(const QList<SeismicPick> &picks) {
    if (picks.isEmpty())
        return;
    m_undoStack->push(new AddPicksCommand(this, picks));
    refreshInterpretationOverlay();
}

void SeismicSectionDockWidget::removePick(int id) {
    const SeismicPick *p = m_session.pickById(id);
    if (!p)
        return;
    m_undoStack->push(new RemovePickCommand(this, *p));
    refreshInterpretationOverlay();
}

void SeismicSectionDockWidget::renamePickHorizon(int id, const QString &newName) {
    const SeismicPick *p = m_session.pickById(id);
    if (!p)
        return;
    m_undoStack->push(new RenamePickCommand(this, id, p->horizonName, newName));
    refreshInterpretationOverlay();
}

void SeismicSectionDockWidget::addFaultFromCanvas(const QVector<QPair<double, double>> &points) {
    if (points.size() < 2)
        return;
    if (m_faultController) {
        // goal/fault-interpretation：拾取落 FaultSet（undo 入编排器栈，
        // 落工程存储）。不再双写会话伴生文件——FaultSet 是断层权威路径。
        paleo::fault::FaultSectionRef section;
        if (!currentFaultSection(&section))
            return; // 时间切片等无剖面身份，不拾取
        paleo::fault::FaultStick stick;
        stick.section = section;
        stick.points = points;
        stick.interpreter = m_pickPanel ? m_pickPanel->currentInterpreter() : QString();
        m_faultController->addStick(stick); // 模型变更经 faultSetChanged 回刷
        return;
    }
    const SectionRef ref = m_canvas->sectionRef();
    if (!ref.valid)
        return;
    SeismicFaultSegment seg;
    seg.id = m_session.nextId++;
    seg.sectionType = ref.type;
    seg.sectionIndex = ref.index;
    seg.points = points;
    seg.interpreter = m_pickPanel ? m_pickPanel->currentInterpreter() : QString();
    seg.name = QStringLiteral("F%1").arg(m_session.faults.size() + 1);
    m_session.faults.append(seg);
    saveInterpretationSession();
    refreshInterpretationOverlay();
}

void SeismicSectionDockWidget::setFaultController(
    paleo::fault::FaultInterpretationController *controller) {
    if (m_faultController == controller)
        return;
    m_faultController = controller;
    if (!controller) {
        refreshFaultStickOverlay();
        return;
    }
    connect(controller, &paleo::fault::FaultInterpretationController::faultSetChanged, this,
            &SeismicSectionDockWidget::refreshFaultStickOverlay);
    connect(controller, &paleo::fault::FaultInterpretationController::faultSelectionChanged, this,
            &SeismicSectionDockWidget::refreshFaultStickOverlay);
    refreshFaultStickOverlay();
}

bool SeismicSectionDockWidget::currentFaultSection(paleo::fault::FaultSectionRef *out) const {
    const SectionRef ref = m_canvas->sectionRef();
    paleo::fault::FaultSectionRef section;
    if (ref.valid && ref.type == SgySliceType::Inline) {
        section.kind = paleo::fault::FaultSectionRef::Inline;
        section.index = ref.index;
        section.displayName = tr("IL %1").arg(ref.index);
    } else if (ref.valid && ref.type == SgySliceType::Xline) {
        section.kind = paleo::fault::FaultSectionRef::Xline;
        section.index = ref.index;
        section.displayName = tr("XL %1").arg(ref.index);
    } else if (m_lastPathPoints.size() >= 2) {
        // 任意线身份 = IL/XL 路径点串（同路径重提取 → 同 pathId → 棒回显）
        QStringList pts;
        for (const glm::ivec2 &p : m_lastPathPoints)
            pts << QStringLiteral("%1,%2").arg(p.x).arg(p.y);
        section.kind = paleo::fault::FaultSectionRef::Arbitrary;
        section.pathId = pts.join(QLatin1Char(';'));
        section.displayName = tr("任意线 %1").arg(section.pathId);
    } else {
        return false; // 时间切片 / 无剖面身份
    }
    if (out)
        *out = section;
    return true;
}

void SeismicSectionDockWidget::setFaultSurfaceCut(
    const SeismicSectionCanvas::FaultSurfaceCutDisplay &cut) {
    if (m_canvas)
        m_canvas->setFaultSurfaceCut(cut);
}

void SeismicSectionDockWidget::refreshFaultStickOverlay() {
    if (!m_faultController) {
        m_canvas->setFaultStickOverlays({});
        return;
    }
    paleo::fault::FaultSectionRef section;
    QVector<SeismicSectionCanvas::FaultStickDisplay> displays;
    if (currentFaultSection(&section)) {
        const QStringList selected = m_faultController->selectedFaultIds();
        for (const auto &pair : m_faultController->faultSet().sticksForSection(section)) {
            SeismicSectionCanvas::FaultStickDisplay d;
            d.points = pair.second.points;
            d.highlighted = selected.contains(pair.first);
            displays.append(d);
        }
    }
    m_canvas->setFaultStickOverlays(displays);
}

bool SeismicSectionDockWidget::saveInterpretationSession(QString *error) {
    if (m_session.sourceSgyPath.isEmpty())
        return false;
    return SeismicTaskService::saveSession(m_session, error);
}

bool SeismicSectionDockWidget::loadInterpretationSession(QString *error) {
    if (m_session.sourceSgyPath.isEmpty())
        return false;
    return SeismicTaskService::loadSession(m_session.sourceSgyPath, m_session, error);
}

QString SeismicSectionDockWidget::registerCurrentHorizonAsset(QString *error) {
    if (!m_catalog) {
        if (error)
            *error = tr("数据目录未连接，无法登记解释成果");
        return QString();
    }
    const QString horizon = m_pickPanel ? m_pickPanel->currentHorizon() : QString();
    QList<SeismicPick> picks;
    for (const SeismicPick &p : m_session.picks)
        if (horizon.isEmpty() || p.horizonName == horizon)
            picks << p;
    // goal/horizon-autotrack：CSV + 层位栅格 GeoTIFF + 可上图声明
    LayerDeclaration decl;
    const QString path = SeismicTaskService::registerHorizonAsset(
        m_catalog, m_catalogAssetId, m_catalogVersionId,
        horizon.isEmpty() ? QStringLiteral("H1") : horizon,
        picks, m_interpretationDir, error, &decl);
    if (!path.isEmpty() && !decl.layerId.isEmpty())
        emit horizonLayerDeclared(decl);
    return path;
}

QString SeismicSectionDockWidget::registerCurrentFaultAsset(QString *error) {
    if (!m_catalog) {
        if (error)
            *error = tr("数据目录未连接，无法登记解释成果");
        return QString();
    }
    return SeismicTaskService::registerFaultAsset(
        m_catalog, m_catalogAssetId, m_catalogVersionId,
        QStringLiteral("F1"), m_session.faults, m_interpretationDir, error);
}

} // namespace seismic
