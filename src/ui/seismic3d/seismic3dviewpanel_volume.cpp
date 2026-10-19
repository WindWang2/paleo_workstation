// 层：视图
// seismic3dviewpanel_volume — 换体入口 TU（full-audit-20261020 自 viewpanel
// 主文件拆出，file 预算纪律 1827 冻结值不涨）：#158 在途切片顶替 +
// D-05 面板侧派生内容弃置（缓存清空/世代号/回退弹回/§35 reason tooltip）。
#include "seismic3dviewpanel.h"
#include "seismic3dviewportwidget.h"

// SF-1（full-audit-20261020）：视图侧堆叠层清理入口定义在此 TU——
// viewportwidget 主文件 1003 冻结值不涨（file-budget/ui-size 只缩不涨纪律）。
void seismic::Seismic3DViewportWidget::clearStackLayers() {
    if (glInitialized_) {
        makeCurrent();
        sliceRenderer_.ClearStackLayers(this);
        doneCurrent();
    }
    update();
}

namespace seismic {

void Seismic3DViewPanel::setVolume(std::shared_ptr<SgyVolume> volume) {
    // #158：换体/清体先中止旧体在途的切片读取（结果按槽位句柄丢弃）。
    for (std::size_t si = 0; si < slotTasks_.size(); ++si)
        supersedeInFlightSlice(si);
    // D-05（full-audit-20261020）：换体同时弃置面板侧派生内容——旧体堆叠层/
    // 任意剖面不得贴新体几何（applyTransferFunction 也不会再重喂旧缓存）；
    // stackExtracting_ 残留 true 会永久挡住新体的堆叠请求；世代号让仍在途
    // 的旧体回调（无槽位句柄可顶替）整体作废。
    ++volumeGeneration_;
    cachedStackReady_.fill(false);
    cachedStackImages_.fill({});
    cachedStackSamples_.fill({});
    cachedLineReady_ = false;
    cachedLinePath_.clear();
    cachedLineImage_ = {};
    stackExtracting_ = false;
    slotCoarse_.fill(false);
    viewport_->setVolume(volume);
    viewport_->clearLineSection(); // 旧体任意剖面 GPU 槽位一并弃置（isLineSectionReady 归零）
    viewport_->clearStackLayers(); // SF-1：旧体堆叠层 GPU ready 态一并弃置（新体取数失败不残留）

    const bool loaded = volume && volume->IsLoaded();
    // 无体（如切到无地震的工程）时切片控件禁用，不再对旧 SEG-Y 发起读取。
    // §35/D-10（full-audit-20261020）：禁用必须带 reason tooltip。
    const QString sliceReason = loaded
        ? QString()
        : tr("先在数据管理导入并加载地震体（带坐标），切片控件方可使用");
    for (QWidget *w : {static_cast<QWidget *>(inlineSlider_), static_cast<QWidget *>(inlineSpin_),
                       static_cast<QWidget *>(xlineSlider_), static_cast<QWidget *>(xlineSpin_),
                       static_cast<QWidget *>(timeSlider_), static_cast<QWidget *>(timeSpin_)})
        if (w) {
            w->setEnabled(loaded);
            w->setToolTip(sliceReason);
        }
    if (!loaded) {
        return;
    }

    const int inlMin = volume->InlineMin();
    const int inlMax = volume->InlineMax();
    const int xlMin = volume->XlineMin();
    const int xlMax = volume->XlineMax();
    const int sampleMax = volume->SampleMax();

    inlineSlider_->blockSignals(true);
    inlineSpin_->blockSignals(true);
    inlineSlider_->setRange(inlMin, inlMax);
    inlineSpin_->setRange(inlMin, inlMax);
    // 数值中点未必是真实测线号（测网步长>1/边缘缺线很常见），吸附到
    // 最近真实线号——否则初始提取静默失败，视口只剩包围盒线框。
    const int midInl = volume->FindNearestInlineValue((inlMin + inlMax) / 2.0);
    inlineSlider_->setValue(midInl);
    inlineSpin_->setValue(midInl);
    inlineSlider_->blockSignals(false);
    inlineSpin_->blockSignals(false);

    xlineSlider_->blockSignals(true);
    xlineSpin_->blockSignals(true);
    xlineSlider_->setRange(xlMin, xlMax);
    xlineSpin_->setRange(xlMin, xlMax);
    const int midXl = volume->FindNearestXlineValue((xlMin + xlMax) / 2.0);
    xlineSlider_->setValue(midXl);
    xlineSpin_->setValue(midXl);
    xlineSlider_->blockSignals(false);
    xlineSpin_->blockSignals(false);

    timeSlider_->blockSignals(true);
    timeSpin_->blockSignals(true);
    timeSlider_->setRange(0, sampleMax);
    timeSpin_->setRange(0, sampleMax);
    const int midTime = sampleMax / 2;
    timeSlider_->setValue(midTime);
    timeSpin_->setValue(midTime);
    timeSlider_->blockSignals(false);
    timeSpin_->blockSignals(false);

    updateTimeMsLabel(midTime);

    // Initial 3D slices extraction
    requestSliceUpdate(SeismicSliceSlot::Inline, SgySliceType::Inline, midInl);
    requestSliceUpdate(SeismicSliceSlot::Crossline, SgySliceType::Xline, midXl);
    requestSliceUpdate(SeismicSliceSlot::Time, SgySliceType::Time, midTime);

    checkMemoryBudget(); // D3.8 体量 > RAM/2 → 提示分页通道

    // 体积变化时重发体渲染堆叠层
    if (stackMode_)
        requestStackLayers();
}

} // namespace seismic
