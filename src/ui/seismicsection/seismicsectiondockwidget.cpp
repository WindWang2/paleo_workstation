// 层：视图
// 方向 65：剖面 dock 主体 TU——构造/析构（在途任务取消）+ IL/XL/Time 切片
// 导航与异步提取 + 任意线提取。世代号 + 请求号双守卫、QPointer 迟到回调守卫
// 是 seismic-runtime-closure 建立的红线，逐条原样保留（tst_seismic_sectionui）。
#include "ui/seismicsection/seismicsectiondockwidget.h"
#include "ui/seismicsection/seismicattrpanel.h"

#include <QComboBox>
#include <QFileDialog>
#include <QImage>
#include <QLabel>
#include <QProgressBar>
#include <QSlider>
#include "../notifications/paleonotify.h"
#include <QSpinBox>

#include "domain/seismic/sectiongeometry.h"
#include "domain/seismic/sgycoordinatemapper.h"
#include "domain/seismic/sgysectionbuilder.h"
#include "services/seismictaskservice.h"

#include <algorithm>
#include <limits>

namespace seismic {

SeismicSectionDockWidget::SeismicSectionDockWidget(QWidget *parent)
    : SeismicSectionDockWidget(tr("地震剖面 / 井震综合"), parent)
{
}

SeismicSectionDockWidget::SeismicSectionDockWidget(const QString &title, QWidget *parent)
    : QDockWidget(title, parent)
{
    setObjectName(QStringLiteral("seismicSectionDock"));
    setAllowedAreas(Qt::AllDockWidgetAreas);
    setupUi();
    auto *tasks = new PaleoTaskService(nullptr, this);
    m_taskService = new SeismicTaskService(tasks, 256, this);
}

// 迟到回调由 QPointer 守卫兜底（无 UAF）；这里取消是为归还并发闸——
// 注入共享服务时（app 装配），排队/在途读取不随 dock 析构消失，
// 不主动取消会占住 ≤4 闸直到读完。
SeismicSectionDockWidget::~SeismicSectionDockWidget() {
    if (m_sliceTask) {
        m_sliceTask->requestCancel();
        m_sliceTask.clear();
    }
    if (m_compareTask) {
        m_compareTask->requestCancel();
        m_compareTask.clear();
    }
    if (m_trackTask) {
        m_trackTask->requestCancel();
        m_trackTask.clear();
    }
    if (m_attrTask) {
        m_attrTask->requestCancel();
        m_attrTask.clear();
    }
    if (m_attrPreviewTask) {
        m_attrPreviewTask->requestCancel();
        m_attrPreviewTask.clear();
    }
    if (m_extraction)
        m_extraction->requestCancel();
}

void SeismicSectionDockWidget::clearRoute() {
  m_route.clear();
  m_distances.clear();
}

void SeismicSectionDockWidget::setVolume(std::shared_ptr<const SgyVolume> volume) {
  if (m_extraction)
    m_extraction->requestCancel();
  // 切片/卷帘在途任务一并取消并摘牌：切体后旧体的读取不再占并发闸，
  // 迟到的结果由世代号+任务身份双重守卫丢弃（不出错、不闪旧图）。
  if (m_sliceTask) {
    m_sliceTask->requestCancel();
    m_sliceTask.clear();
  }
  if (m_compareTask) {
    m_compareTask->requestCancel();
    m_compareTask.clear();
  }
  m_sliceIndex = -1; // 去抖键随体身份作废（同号线也不再等价）
  ++m_generation;
  m_route.clear();
  m_distances.clear();
  m_canvas->clearData();
  m_canvas->setWells({});
  m_progressBar->hide();
  m_volume = volume;
  if (!m_volume || !m_volume->IsLoaded()) {
    m_sliceGroup->setEnabled(false);
    return;
  }
  // goal/attr-volume：时间切片采样位范围随体回填（缺省中位采样）。
  if (m_attrPanel)
    m_attrPanel->setVolumeSampleRange(m_volume->SampleCount());

    m_sliceGroup->setEnabled(true);
    loadBookmarksFromSettings(); // D2.12：体身份确定后才有 settings 键
    // D4.8：会话锚定到 SEG-Y 伴生文件（存在即自动恢复）
    m_session = SeismicInterpretationSession{};
    m_session.sourceSgyPath = QString::fromStdString(m_volume->Path().string());
    SeismicTaskService::loadSession(m_session.sourceSgyPath, m_session, nullptr);
    refreshInterpretationOverlay();
    // 任意线（模式 3）保持待编辑态，不强制重提剖面（wave/sections）
    if (m_cboSectionMode->currentIndex() != 3)
        onSectionModeChanged(m_cboSectionMode->currentIndex());
}

void SeismicSectionDockWidget::onSectionModeChanged(int modeIndex) {
    if (modeIndex == 3) {
        if (m_extraction) m_extraction->requestCancel();
        // 任意线顶替切片显示：在途切片/卷帘一并取消摘牌、去抖键作废——
        // 只推世代号不清任务会让切片在途结果被自己的世代号毒化丢弃
        //（3→0 回切时去抖命中一个已注定被丢弃的请求 → 静默空白）。
        if (m_sliceTask) {
            m_sliceTask->requestCancel();
            m_sliceTask.clear();
        }
        if (m_compareTask) {
            m_compareTask->requestCancel();
            m_compareTask.clear();
        }
        m_sliceIndex = -1;
        ++m_generation;
        m_progressBar->hide();
        m_sliceGroup->hide();
        emit setupRequested();
        return;
    }
    if (!m_volume || !m_volume->IsLoaded()) {
        m_sliceGroup->setVisible(modeIndex != 3);
        return;
    }

    m_sliderSlice->blockSignals(true);
    m_spinSlice->blockSignals(true);

    if (modeIndex == 0) { // Inline
        m_sliceGroup->setVisible(true);
        m_lblSliceIndex->setText(tr("纵测线:"));
        m_sliderSlice->setRange(m_volume->InlineMin(), m_volume->InlineMax());
        m_spinSlice->setRange(m_volume->InlineMin(), m_volume->InlineMax());
        const int mid = m_volume->FindNearestInlineValue(
            (m_volume->InlineMin() + m_volume->InlineMax()) / 2.0); // 吸附真实线号
        m_sliderSlice->setValue(mid);
        m_spinSlice->setValue(mid);
        m_lblTimeMs->setVisible(false);
        m_sliderSlice->blockSignals(false);
        m_spinSlice->blockSignals(false);
        extractSliceAsync(SgySliceType::Inline, mid);
    } else if (modeIndex == 1) { // Crossline
        m_sliceGroup->setVisible(true);
        m_lblSliceIndex->setText(tr("横测线:"));
        m_sliderSlice->setRange(m_volume->XlineMin(), m_volume->XlineMax());
        m_spinSlice->setRange(m_volume->XlineMin(), m_volume->XlineMax());
        const int mid = m_volume->FindNearestXlineValue(
            (m_volume->XlineMin() + m_volume->XlineMax()) / 2.0);
        m_sliderSlice->setValue(mid);
        m_spinSlice->setValue(mid);
        m_lblTimeMs->setVisible(false);
        m_sliderSlice->blockSignals(false);
        m_spinSlice->blockSignals(false);
        extractSliceAsync(SgySliceType::Xline, mid);
    } else if (modeIndex == 2) { // Time Slice
        m_sliceGroup->setVisible(true);
        m_lblSliceIndex->setText(tr("时间采样:"));
        m_sliderSlice->setRange(0, m_volume->SampleMax());
        m_spinSlice->setRange(0, m_volume->SampleMax());
        const int mid = m_volume->SampleMax() / 2;
        m_sliderSlice->setValue(mid);
        m_spinSlice->setValue(mid);
        const double ms =
            m_timeOriginMs + mid * (m_volume->SampleIntervalUs() / 1000.0);
        m_lblTimeMs->setText(QStringLiteral("%1 ms").arg(ms, 0, 'f', 1));
        m_lblTimeMs->setVisible(true);
        m_sliderSlice->blockSignals(false);
        m_spinSlice->blockSignals(false);
        extractSliceAsync(SgySliceType::Time, mid);
    } else { // Arbitrary line
        m_sliceGroup->setVisible(false);
        m_sliderSlice->blockSignals(false);
        m_spinSlice->blockSignals(false);
    }
}

void SeismicSectionDockWidget::onSliceSliderChanged(int value) {
    if (!m_volume || !m_volume->IsLoaded())
        return;

    const int mode = m_cboSectionMode->currentIndex();
    if (mode == 0) {
        // 吸附到真实测线号（测网步长>1 时滑杆中点/拖动值可能不存在）
        const int snapped = m_volume->FindNearestInlineValue(value);
        if (snapped != value) {
            m_sliderSlice->setValue(snapped); // 重发 valueChanged，spin 同步后本函数再入
            return;
        }
        extractSliceAsync(SgySliceType::Inline, value);
    } else if (mode == 1) {
        const int snapped = m_volume->FindNearestXlineValue(value);
        if (snapped != value) {
            m_sliderSlice->setValue(snapped);
            return;
        }
        extractSliceAsync(SgySliceType::Xline, value);
    } else if (mode == 2) {
      const double ms =
          m_timeOriginMs + value * (m_volume->SampleIntervalUs() / 1000.0);
      m_lblTimeMs->setText(QStringLiteral("%1 ms").arg(ms, 0, 'f', 1));
      extractSliceAsync(SgySliceType::Time, value);
    }
}

void SeismicSectionDockWidget::setSectionMode(int modeIndex) {
    if (m_cboSectionMode && m_cboSectionMode->currentIndex() != modeIndex) {
        m_cboSectionMode->setCurrentIndex(modeIndex);
    } else if (m_volume && m_volume->IsLoaded()) {
        onSectionModeChanged(modeIndex);
    }
}

void SeismicSectionDockWidget::extractSliceAsync(SgySliceType type, int index) {
    if (!m_volume || !m_volume->IsLoaded() || !m_taskService)
        return;

    // 切片顶替任意线显示：取消其在途任务——迟到的任意线结果不再覆盖已
    // 应用的切片（旧实现无此守卫，属真实缺陷）。世代号在去抖判定之后才
    // 推进：重复请求走早退，不能毒化它本想去重的那个在途任务。
    if (m_extraction) {
        m_extraction->requestCancel();
        m_extraction.clear();
    }

    // 同型同号在途时去抖（滑杆吸附重入 / slider 与 spin 双发同值）。
    if (m_sliceTask && m_sliceType == type && m_sliceIndex == index)
        return;
    // 走到这里必然是顶替或新请求：推进世代号作废被取消的任意线迟到回调，
    // 再顶替旧切片在途（协作取消——worker 逐线检查点退出，不再占并发闸）。
    // 先摘牌再启新：服务的立即失败路径会同步回调，此时请求号守卫以
    // 「计数已推进」放行如实报错。
    ++m_generation;
    if (m_sliceTask) {
        m_sliceTask->requestCancel();
        m_sliceTask.clear();
    }
    m_sliceType = type;
    m_sliceIndex = index;
    // #224：换线即清属性叠加——同体同尺寸的相邻线几何全等，画布按尺寸
    // 清叠加的防线挡不住旧线属性图残留。
    m_canvas->clearAttrOverlay();

    // 常规 IL/XL/Time 切换：丢弃任意线旧状态（route/井叠加），
    // hasRoute() 复归 false（wave/sections 语义）。
    m_route.clear();
    m_distances.clear();
    m_lastPathPoints.clear(); // 断层剖面身份随之失效（goal/fault-interpretation）
    m_canvas->setWells({});
    const double origin = m_timeOriginMs;

    QString title;
    if (type == SgySliceType::Inline) {
        title = tr("纵测线剖面 IL %1").arg(index);
    } else if (type == SgySliceType::Xline) {
        title = tr("横测线剖面 XL %1").arg(index);
    } else {
        const double ms = origin + index * (m_volume->SampleIntervalUs() / 1000.0);
        title = tr("水平时间切片 TWT %1 ms").arg(ms, 0, 'f', 1);
    }
    setLineTitle(title);

    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    m_progressBar->setVisible(true);

    // 体快照（SgyVolume 拷贝廉价：索引经 shared_ptr 不可变共享，服务通道
    // 只走 const 面）。经 SeismicTaskService：≤4 并发闸 + 协作取消 +
    // 切片 LRU + 引擎 Auto 后端（sf3c 工作区热切换后自动吃随机访问红利）。
    auto vol = std::make_shared<SgyVolume>(*m_volume);
    const auto generation = m_generation;
    const auto request = ++m_sliceRequest;
    QPointer<SeismicSectionDockWidget> guard(this);

    PaleoTask *rawTask = m_taskService->startSliceExtraction(
        vol, type, index,
        [guard, generation, request, vol, type, index, title, origin](
            bool ok, std::shared_ptr<const SgySliceImage> image, const QString &error) {
            // 双重守卫：世代号（切体/重开）+ 请求号（被更新的切片请求
            // 顶替）。被顶替/取消的旧任务静默丢弃——cancelled 不是失败，
            // 不弹误导错误、不覆盖新请求已应用的画面。
            if (!guard || guard->m_generation != generation || guard->m_sliceRequest != request)
                return;
            guard->m_sliceTask.clear();
            guard->m_progressBar->setVisible(false);

            if (!ok) {
                // D2.14：原因态——画布显示可读原因而非空白
                guard->setLineTitle(guard->tr("切片提取失败: %1").arg(error));
                guard->m_canvas->clearData();
                guard->m_canvas->setNoDataReason(guard->tr("剖面不可用\n%1").arg(error));
                emit guard->sectionExtractionFinished(false, error);
                return;
            }
            if (!image || image->width <= 0 || image->height <= 0 || image->values.empty()) {
                // D2.14：空数据原因态（如无有效道的线号）
                guard->setLineTitle(title);
                guard->m_canvas->clearData();
                guard->m_canvas->setNoDataReason(
                    guard->tr("%1\n该线无有效地震道（工区覆盖范围外）").arg(title));
                emit guard->sectionExtractionFinished(false, guard->tr("空切片"));
                return;
            }

            guard->setLineTitle(title);
            if (guard->m_btnCurtain && guard->m_btnCurtain->isChecked())
                guard->updateCompareSlice(); // D2.10：当前线变了，相邻线 B 图同步
            if (type == SgySliceType::Time) {
                const double ms = origin + index * (vol->SampleIntervalUs() / 1000.0);
                guard->m_canvas->setTimeSliceData(*image, ms, vol->InlineMin(),
                                                  vol->InlineMax(), vol->XlineMin(),
                                                  vol->XlineMax());
            } else {
                const float dtMs = vol->SampleIntervalUs() > 0
                                       ? (vol->SampleIntervalUs() / 1000.0f) : 2.0f;
                guard->m_canvas->setSectionData(*image, dtMs, origin);
            }
            // D4：剖面身份 + 最近切片（拾取解析/追踪原料）
            {
                SectionRef ref;
                ref.valid = true;
                ref.type = type;
                ref.index = index;
                if (type == SgySliceType::Inline)
                    ref.colMin = vol->XlineMin(), ref.colMax = vol->XlineMax(),
                    ref.colLines = vol->XlineValues();
                else if (type == SgySliceType::Xline)
                    ref.colMin = vol->InlineMin(), ref.colMax = vol->InlineMax(),
                    ref.colLines = vol->InlineValues();
                else
                    ref.colMin = vol->XlineMin(), ref.colMax = vol->XlineMax();
                // 列轴长度与切片列数不符（理论上不会）→ 不用轴表，避免错位映射。
                if (!ref.colLines.empty() && int(ref.colLines.size()) != image->width)
                    ref.colLines.clear();
                ref.t0Ms = origin; // #146：剖面时间原点（记录延迟）
                guard->m_canvas->setSectionRef(ref);
                if (type != SgySliceType::Time)
                    guard->m_lastSlice = *image;
            }
            guard->refreshInterpretationOverlay();
            emit guard->sectionExtractionFinished(true, QString());
        });
    m_sliceTask = rawTask;

    // 进度条由任务字节进度驱动（quiet 任务照常 reportBytes；直读后端逐线
    // 上报、引擎后端完成时一次上报）。按请求号守卫：被顶替的旧任务其
    // changed() 连接仍存活到任务终态，无守卫会把旧线的百分比写进新请求
    // 的进度条（fast sweep 时可见回跳）。
    if (rawTask) {
        QPointer<PaleoTask> progressTask = rawTask;
        connect(rawTask, &PaleoTask::changed, this, [this, progressTask, request]() {
            if (!progressTask || m_sliceRequest != request)
                return;
            if (progressTask->bytesTotal() > 0)
                m_progressBar->setValue(std::max(0, progressTask->percent()));
        });
    }
}

void SeismicSectionDockWidget::setSectionData(
    const SgySliceImage &image,
    float sampleIntervalMs,
    double startSampleMs,
    const std::vector<float> &columnDistancesM,
    const std::vector<glm::dvec2> &mapCoords)
{
    m_canvas->setSectionData(image, sampleIntervalMs, startSampleMs, columnDistancesM, mapCoords);
}

void SeismicSectionDockWidget::setWells(const std::vector<SectionWellInfo> &wells) {
    m_canvas->setWells(wells);
}

void SeismicSectionDockWidget::setTimeDepthModel(const TimeDepthModel &model) {
    m_canvas->setTimeDepthModel(model);
}

void SeismicSectionDockWidget::setLineTitle(const QString &title) {
    m_lblTitle->setText(title.isEmpty() ? tr("测线: 未加载") : title);
}

void SeismicSectionDockWidget::onZoomChanged(double) {
    // Zoom update
}

void SeismicSectionDockWidget::onTraceHovered(
    int traceIndex, double twtMs, double depthM, float amplitude, double mapX, double mapY)
{
    if (m_canvas->traceCount() <= 0)
        return;

    if (m_canvas->orientation() == SectionOrientation::TimeSlice) {
        m_lblCoordinates->setText(
            tr("横测线 (XL): %1 | 纵测线 (IL): %2 | 时间: %3 ms | 深度: %4 m | 振幅: %5")
                .arg(qRound(mapX))
                .arg(qRound(mapY))
                .arg(twtMs, 0, 'f', 1)
                .arg(depthM, 0, 'f', 1)
                .arg(amplitude, 0, 'f', 4));
        return;
    }

    QString text = tr("道: %1/%2 | TWT: %3 ms | 深度: %4 m | 振幅: %5")
        .arg(traceIndex + 1)
        .arg(m_canvas->traceCount())
        .arg(twtMs, 0, 'f', 1)
        .arg(depthM, 0, 'f', 1)
        .arg(amplitude, 0, 'f', 4);

    if (std::abs(mapX) > 1e-3 || std::abs(mapY) > 1e-3) {
        text += QStringLiteral(" | (X: %1, Y: %2)").arg(mapX, 0, 'f', 1).arg(mapY, 0, 'f', 1);
    }

    m_lblCoordinates->setText(text);
}

void SeismicSectionDockWidget::onExportSnapshot() {
    const QString filePath = QFileDialog::getSaveFileName(
        this, tr("导出剖面图件"), QStringLiteral("seismic_section.png"),
        tr("PNG 图像 (*.png);;JPEG 图像 (*.jpg *.jpeg)"));
    if (filePath.isEmpty())
        return;

    QImage img(m_canvas->size(), QImage::Format_ARGB32_Premultiplied);
    m_canvas->render(&img);
    if (img.save(filePath)) {
        PaleoNotify::information(this, tr("导出成功"), tr("剖面图件已成功保存到:\n%1").arg(filePath));
    } else {
        PaleoNotify::critical(this, tr("导出失败"), tr("保存图像文件失败，请检查文件写入权限。"));
    }
}

void SeismicSectionDockWidget::refreshWellOverlay(
    const std::vector<SectionWellInfo> &wells) {
  if (m_route.size() < 2)
    return;
  std::vector<double> distances(m_distances.begin(), m_distances.end());
  // Keep every candidate's offset; buffer changes must not require
  // re-extraction.
  auto projected = SectionWellProjector::ProjectWells(
      m_route, {}, distances, wells, std::numeric_limits<double>::max(),
      m_canvas->timeDepthModel());
  m_canvas->setWells(projected);
}

void SeismicSectionDockWidget::extractSectionFromVolumeAsync(
    std::shared_ptr<const SgyVolume> volume,
    const std::vector<glm::ivec2> &pathPoints, const QString &lineTitle,
    const std::vector<glm::dvec2> &mapPolyline,
    const std::vector<SectionWellInfo> &candidateWells) {
  if (!volume || pathPoints.size() < 2 || !m_taskService)
    return;
  if (m_extraction)
    m_extraction->requestCancel();
  // 任意线顶替切片显示：取消在途切片/卷帘任务并作废去抖键——迟到的
  // 切片结果由世代号丢弃，旧体读取不再占并发闸。
  if (m_sliceTask) {
    m_sliceTask->requestCancel();
    m_sliceTask.clear();
  }
  if (m_compareTask) {
    m_compareTask->requestCancel();
    m_compareTask.clear();
  }
  m_sliceIndex = -1;
  const auto generation = ++m_generation;
  m_volume = volume;
  m_route.clear();
  m_distances.clear();
  // goal/fault-interpretation：任意线剖面身份 = 路径点串；同时作废 IL/XL
  // 陈旧 SectionRef（旧实现任意线不设 ref，拾取/断层会误归属上一条线）。
  m_lastPathPoints = pathPoints;
  m_canvas->setSectionRef(SectionRef{});
  m_canvas->setWells({});
  m_cboSectionMode->blockSignals(true);
  m_cboSectionMode->setCurrentIndex(3);
  m_cboSectionMode->blockSignals(false);
  m_sliceGroup->hide();
  setLineTitle(tr("%1 · 提取中…").arg(lineTitle));
  m_progressBar->setRange(0, 0);
  m_progressBar->show();
  const double origin = m_timeOriginMs;
  SgySectionOptions options;
  options.maxColumns = 2048;
  options.interpolate = false;
  QPointer<SeismicSectionDockWidget> guard(this);
  m_extraction = m_taskService->startSectionExtraction(
      std::make_shared<SgyVolume>(*volume), pathPoints, options,
      [guard, generation, volume, pathPoints, lineTitle, mapPolyline,
       candidateWells,
       origin](bool ok, std::shared_ptr<const SgySliceImage> image,
               const SgySectionStats &stats, const QString &error) {
        if (!guard || guard->m_generation != generation)
          return;
        guard->m_progressBar->hide();
        if (!ok) {
          guard->setLineTitle(guard->tr("剖面提取失败：%1").arg(error));
          emit guard->sectionExtractionFinished(false, error);
          return;
        }
        std::vector<glm::dvec2> route = mapPolyline;
        if (route.size() != pathPoints.size() && volume && volume->Index()) {
          const auto mapper = SgyCoordinateMapper::Fit(*volume->Index());
          if (mapper.valid()) {
            route.clear();
            for (const auto &p : pathPoints) {
              double x = 0.0, y = 0.0;
              if (mapper.MapInlineXline(p.x, p.y, x, y))
                route.push_back({x, y});
            }
          }
        }
        const auto geometry = SectionGeometry::fromColumns(
            pathPoints, route, stats.columnDistances);
        guard->m_route = route;
        guard->m_distances = geometry.distancesM;
        guard->m_canvas->setSectionData(
            *image, volume->SampleIntervalUs() / 1000.0f, origin,
            geometry.distancesM, geometry.coordinates);
        guard->refreshWellOverlay(candidateWells);
        guard->setLineTitle(lineTitle);
        // #225：入参候选井写入成员——computeWellTrajectories /
        // computeSyntheticOverlays / 井旁道 / 子波提取 / 反演低频井读的都是
        // m_candidateWells，旧实现只按入参判空、成员恒空，四条链生产恒死。
        guard->m_candidateWells = candidateWells;
        // D5.3/D5.4：井轨迹投影 + 合成记录（任意线链路，wave/seismic-chain-deep）
        if (!candidateWells.empty()) {
          guard->computeWellTrajectories(route);
          guard->computeSyntheticOverlays();
        }
        // 与切片路径同拍刷新解释叠加：剖面身份变了（任意线），
        // FaultSet 棒按新身份重新过滤回显（goal/fault-interpretation）。
        guard->refreshInterpretationOverlay();
        emit guard->sectionExtractionFinished(true, QString());
      });
}

} // namespace seismic
