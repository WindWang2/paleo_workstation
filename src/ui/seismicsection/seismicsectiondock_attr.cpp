// 层：视图
// 方向 65：goal/seismic-attributes + goal/attr-volume 属性控制器族——剖面属性
// 计算、时间切片扫描、属性体扫描（含 3D 预览静默任务的世代号守卫）。
#include "ui/seismicsection/seismicsectiondockwidget.h"
#include "ui/seismicsection/seismicattrpanel.h"

#include "catalog/datacatalog.h"
#include "services/fspathutils.h" // #291 QString↔filesystem::path 走 UTF-16（MSVC 窄构造按 ANSI 解码）
#include "services/seismictaskservice.h"

#include <QPointer>

namespace seismic {

void SeismicSectionDockWidget::computeAttributeOnCurrentSection(
    SeismicTaskService::SeismicAttrKind kind,
    const SeismicTaskService::SeismicAttrParams &params) {
    if (!m_attrPanel)
        return;
    if (!m_taskService) {
        m_attrPanel->showResult(false, tr("任务服务未注入"));
        return;
    }
    if (!m_volume) {
        m_attrPanel->showResult(false, tr("地震体未加载（先打开 SEG-Y）"));
        return;
    }
    const int mode = m_cboSectionMode ? m_cboSectionMode->currentIndex() : 0;
    if (mode != 0 && mode != 1) {
        m_attrPanel->showResult(
            false, tr("属性计算支持 Inline/Crossline 剖面（时间片/任意线见 TODOS）"));
        return;
    }
    const SgySliceType type = mode == 0 ? SgySliceType::Inline : SgySliceType::Xline;
    const int index = m_spinSlice ? m_spinSlice->value() : 0;

    const QString sourcePath = paleo::fromFsPath(m_volume->Path());
    m_attrPanel->setBusy(true);
    // #224：请求时快照剖面身份（世代号随换线/换体/任意线推进）+ 迟到回调守卫。
    // 换线换体后迟到的属性图不得贴到新剖面上，也不得顶替可登记结果。
    const quint64 generation = m_generation;
    QPointer<SeismicSectionDockWidget> guard(this);
    PaleoTask *task = m_taskService->startAttributeSlice(
        m_volume, kind, params, type, index,
        [guard, generation, params, sourcePath](
            bool ok, const SeismicTaskService::SeismicAttrResult &r) {
            if (!guard || !guard->m_attrPanel)
                return;
            SeismicSectionDockWidget *self = guard.data();
            if (self->m_generation != generation) {
                self->m_attrPanel->showResult(
                    false, self->tr("剖面已切换，本次属性结果丢弃（请在当前剖面重新计算）"));
                return;
            }
            if (ok && r.image) {
                self->m_lastAttrResult = r;
                self->m_lastAttrParams = params;
                self->m_lastAttrSourcePath = sourcePath;
                self->m_canvas->setAttrOverlay(*r.image);
                self->m_attrPanel->showResult(
                    true, self->tr("✓ %1 完成（读 %2ms / 算 %3ms，有效道 %4/%5）")
                              .arg(r.attrId)
                              .arg(int(r.readMs))
                              .arg(int(r.computeMs))
                              .arg(r.validTraceCount)
                              .arg(r.traceCount));
            } else {
                self->m_attrPanel->showResult(false, r.error);
            }
        });
    // 拒绝路径（缺线/边缘线/时间切片等）服务已同步回调具体原因——此处
    // 不覆盖状态；task 为空的场景 progress 连接跳过即可。
    m_attrTask = task;
    if (task) {
        connect(task, &PaleoTask::changed, this, [this, task]() {
            if (m_attrPanel && task->running())
                m_attrPanel->updateProgress(task->percent(), task->stage());
        });
    }
}

QString SeismicSectionDockWidget::registerCurrentAttributeAsset(QString *error) {
    if (!m_catalog) {
        if (error)
            *error = tr("catalog 未注入（应用层需调 setInterpretationCatalog）");
        return QString();
    }
    if (!m_lastAttrResult.ok || !m_lastAttrResult.image) {
        if (error)
            *error = tr("无可登记的成功属性结果");
        return QString();
    }
    return SeismicTaskService::registerAttributeSliceAsset(
        m_catalog, m_catalogAssetId, m_catalogVersionId, m_lastAttrResult,
        m_lastAttrParams, m_lastAttrSourcePath, m_interpretationDir, error);
}

// goal/attr-volume 扫描产物目录：解释目录由 app 层随 catalog 一并注入
// （工程受管 artifacts/derived/interpretation——拾取/断层/属性扫描产物全部
// 落此并登记 DERIVED，对 catalog/治理/版本溯源可见）。
// #226：未注入时不再缺省落 <sgy>.attrs 伴生目录——SATV 可达 GB 级，外部
// 数据会把体量级产物堆进源数据目录且对 catalog 完全不可见；返回空由服务
// 如实拒绝（「属性体扫描需产物目录」），时间切片扫描仍可无目录纯扫描。
QString SeismicSectionDockWidget::attrScanOutputDir() const {
    return m_interpretationDir;
}

// goal/attr-volume：时间切片扫描编排——完成即登记（DERIVED + 层树声明）；
// 取消/失败如实回面板不登记（取消无半成品 DERIVED）。登记上下文按扫描
// 启动时的快照取（回调期 catalog/装配可能换装——不读当时的成员状态）。
void SeismicSectionDockWidget::computeTimeSliceAttribute(
    SeismicTaskService::SeismicAttrKind kind,
    const SeismicTaskService::SeismicAttrParams &params, int sampleIndex) {
    if (!m_attrPanel)
        return;
    if (!m_taskService) {
        m_attrPanel->showResult(false, tr("任务服务未注入"));
        return;
    }
    if (!m_volume) {
        m_attrPanel->showResult(false, tr("地震体未加载（先打开 SEG-Y）"));
        return;
    }
    const QString sourcePath = paleo::fromFsPath(m_volume->Path());
    const QString outputDir = attrScanOutputDir();
    const QPointer<DataCatalog> catalog = m_catalog;
    const QString assetId = m_catalogAssetId;
    const QString versionId = m_catalogVersionId;
    // 扫描产物自动登记——「登记资产」按钮只服务剖面结果；清空旧剖面
    // 结果防「新参数 + 旧图像」的 SATR 错配登记。
    m_lastAttrResult = {};
    m_lastAttrParams = params;
    m_lastAttrSourcePath = sourcePath;
    m_attrPanel->setBusy(true);
    QPointer<SeismicSectionDockWidget> guard(this); // 迟到回调守卫（体传播同范式）
    PaleoTask *task = m_taskService->startTimeSliceAttribute(
        m_volume, kind, params, sampleIndex, outputDir,
        [this, guard, kind, params, sourcePath, outputDir, catalog, assetId,
         versionId](bool ok,
            const SeismicTaskService::SeismicAttrTimeSliceResult &r) {
            if (!guard)
                return; // dock 已亡：丢弃
            if (!m_attrPanel)
                return;
            // #236：工程/体世代守卫——请求时快照的体路径与当前不符（工程
            // 切换后旧在途扫描迟到）即丢弃：旧工程产物不得登记/上图到新
            // 工程。面板如实收尾忙态，不冒充成功。
            const QString currentPath =
                m_volume ? QString::fromStdString(m_volume->Path().string())
                         : QString();
            if (currentPath != sourcePath)
            {
                m_attrPanel->showResult(
                    false, tr("剖面/工程已切换，本次时间切片扫描结果丢弃"));
                return;
            }
            if (ok) {
                const QString suffix =
                    r.cacheHit ? tr("（缓存命中）") : QString();
                m_attrPanel->showResult(
                    true, tr("✓ %1 时间切片 t=%2ms 完成 %3（有效 %4/%5）")
                              .arg(r.attrId)
                              .arg(int(r.timeMs))
                              .arg(suffix)
                              .arg(r.validCells)
                              .arg(qint64(r.nIl) * r.nXl),
                    /*registrable=*/false);
                // 登记 + 层树声明（扫描产物缓存命名，登记幂等按 param_hash）。
                // 登记失败：面板如实报错——不冒充已上图。
                if (catalog) {
                    LayerDeclaration decl;
                    QString err;
                    const QString path = SeismicTaskService::
                        registerTimeSliceAttributeAsset(
                            catalog, assetId, versionId, kind, params, r,
                            sourcePath, outputDir, &err, &decl);
                    if (path.isEmpty()) {
                        m_attrPanel->showResult(
                            false, tr("✓ 扫描完成但登记失败：%1").arg(err));
                    } else {
                        emit timeSliceAttrLayerReady(decl);
                    }
                } else {
                    m_attrPanel->showResult(
                        false, tr("✓ 扫描完成（catalog 未注入，未登记上图）"));
                }
            } else {
                m_attrPanel->showResult(false, r.error);
            }
        });
    m_attrTask = task;
    if (task) {
        connect(task, &PaleoTask::changed, this, [this, task]() {
            if (m_attrPanel && task->running())
                m_attrPanel->updateProgress(task->percent(), task->stage());
        });
    }
}

// goal/attr-volume：整体属性体扫描编排——完成即登记（SATV DERIVED）；
// 3D 喂入走静默预览任务（SATV 可达 GB 级，主线程同步读会冻 UI——取数
// 归服务线程，视口只收烘焙好的图像面）。
void SeismicSectionDockWidget::computeAttributeVolume(
    SeismicTaskService::SeismicAttrKind kind,
    const SeismicTaskService::SeismicAttrParams &params) {
    if (!m_attrPanel)
        return;
    if (!m_taskService) {
        m_attrPanel->showResult(false, tr("任务服务未注入"));
        return;
    }
    if (!m_volume) {
        m_attrPanel->showResult(false, tr("地震体未加载（先打开 SEG-Y）"));
        return;
    }
    const QString sourcePath = paleo::fromFsPath(m_volume->Path());
    const QString outputDir = attrScanOutputDir();
    const QPointer<DataCatalog> catalog = m_catalog;
    const QString assetId = m_catalogAssetId;
    const QString versionId = m_catalogVersionId;
    m_lastAttrResult = {};
    m_lastAttrParams = params;
    m_lastAttrSourcePath = sourcePath;
    m_attrPanel->setBusy(true);
    QPointer<SeismicSectionDockWidget> guard(this); // 迟到回调守卫（体传播同范式）
    PaleoTask *task = m_taskService->startAttributeVolume(
        m_volume, kind, params, outputDir,
        [this, guard, taskSvc = QPointer<SeismicTaskService>(m_taskService), kind,
         params, sourcePath, outputDir, catalog, assetId,
         versionId](bool ok, const SeismicTaskService::SeismicAttrVolumeResult &r) {
            if (!guard || !m_attrPanel)
                return;
            // #236：工程/体世代守卫——快照体路径与当前不符（中途关/换工程，
            // 新工程回调照常走）即静默丢弃：旧工程属性体不得贴进已重置的
            // 3D 视口、不得登记进新工程 catalog。放在 !ok 分支之前——取消
            // （含工程边界取消）同样走此守卫，不弹误导性的 3D 失败提示。
            const QString currentPath =
                m_volume ? QString::fromStdString(m_volume->Path().string())
                         : QString();
            if (currentPath != sourcePath)
            {
                m_attrPanel->showResult(
                    false, tr("剖面/工程已切换，本次属性体扫描结果丢弃"));
                return;
            }
            if (!ok) {
                emit attrVolumeReady({}, false, r.error);
                m_attrPanel->showResult(false, r.error);
                return;
            }
            if (!guard || !m_attrPanel)
                return;
            const QString suffix = r.cacheHit ? tr("（缓存命中）") : QString();
            QString message;
            if (catalog) {
                QString err;
                if (SeismicTaskService::registerAttributeVolumeAsset(
                        catalog, assetId, versionId, kind, params, r,
                        sourcePath, &err)
                        .isEmpty())
                    message = tr("登记失败：%1").arg(err);
            } else {
                message = tr("catalog 未注入，未登记");
            }
            m_attrPanel->showResult(
                true, tr("✓ %1 属性体完成 %2（%3×%4×%5，有效 %6）%7")
                          .arg(r.attrId)
                          .arg(suffix)
                          .arg(r.nIl)
                          .arg(r.nXl)
                          .arg(r.nS)
                          .arg(r.validCells)
                          .arg(message.isEmpty() ? QString()
                                                 : QStringLiteral("；") + message),
                /*registrable=*/false);
            // 3D 预览取数：静默任务（不占任务面板注意力），完成经信号
            // 喂视口；取数失败如实随信号报因。世代守卫：新预览发布即取消
            // 旧预览并丢弃其迟到回调（陈旧属性体不得覆盖新结果上图）。
            if (!taskSvc)
                return;
            if (m_attrPreviewTask)
                m_attrPreviewTask->requestCancel();
            const quint64 previewGen = ++m_attrPreviewGen;
            auto previewPtr =
                std::make_shared<SeismicTaskService::AttributeVolumePreview>();
            PaleoTask *previewTask = taskSvc->startBounded(
                QObject::tr("属性体 3D 预览取数"),
                [r, previewPtr](PaleoTask *) -> QString {
                    *previewPtr =
                        SeismicTaskService::loadAttributeVolumePreview(r.path);
                    return previewPtr->ok ? QString() : previewPtr->error;
                },
                QString(), /*quiet=*/true);
            m_attrPreviewTask = previewTask;
            if (previewTask) {
                connect(previewTask, &PaleoTask::finished, this,
                        [this, guard, previewGen, previewPtr]() {
                          if (!guard || previewGen != m_attrPreviewGen)
                            return; // dock 已亡/已被更新预览顶替：丢弃
                          m_attrPreviewTask.clear();
                          emit attrVolumeReady(*previewPtr, previewPtr->ok,
                                               previewPtr->error);
                        });
            }
        });
    m_attrTask = task;
    if (task) {
        connect(task, &PaleoTask::changed, this, [this, task]() {
            if (m_attrPanel && task->running())
                m_attrPanel->updateProgress(task->percent(), task->stage());
        });
    }
}

} // namespace seismic
