// 层：视图
// seismic3dviewpanel_sweep — D7.4 切片动画扫掠 TU（自 viewpanel 主文件拆出）。
// 拆出缘由（file 预算纪律「只缩不涨」，AGENTS.md 闸门基线纪律）：主文件
// 1775 行 = 冻结值，任何净增行都判红；按 #502 既定收敛方向（编排主文件 +
// 域 TU，同 seismic3dviewpanel_volume 先例）把整段扫掠族析出，主文件随之
// 瘦身并在两规模基线同步下调冻结值。
#include "seismic3dviewpanel.h"
#include "seismic3dviewportwidget.h"

#include <QDir>
#include <QImage>
#include <QTimer>

#include <algorithm>

namespace seismic {

// ---- D7.4 切片动画扫掠 ----
int Seismic3DViewPanel::sweepOrdinalCount() const {
    auto vol = volume();
    if (!vol || !vol->IsLoaded())
        return 0;
    switch (sweepAxis_) {
    case SgySliceType::Inline: return int(vol->InlineValues().size());
    case SgySliceType::Xline: return int(vol->XlineValues().size());
    default: return vol->SampleMax() + 1;
    }
}

int Seismic3DViewPanel::sweepIndexAt(int ordinal) const {
    auto vol = volume();
    if (!vol || !vol->IsLoaded())
        return 0;
    switch (sweepAxis_) {
    case SgySliceType::Inline: {
        const auto &v = vol->InlineValues();
        return ordinal >= 0 && ordinal < int(v.size()) ? v[std::size_t(ordinal)]
                                                       : vol->InlineMin();
    }
    case SgySliceType::Xline: {
        const auto &v = vol->XlineValues();
        return ordinal >= 0 && ordinal < int(v.size()) ? v[std::size_t(ordinal)]
                                                       : vol->XlineMin();
    }
    default:
        return ordinal; // T：原始采样号
    }
}

SeismicSliceSlot Seismic3DViewPanel::sweepSlot() const {
    switch (sweepAxis_) {
    case SgySliceType::Inline: return SeismicSliceSlot::Inline;
    case SgySliceType::Xline: return SeismicSliceSlot::Crossline;
    default: return SeismicSliceSlot::Time;
    }
}

void Seismic3DViewPanel::startSweep(SgySliceType axis, int fps) {
    auto vol = volume();
    if (!vol || !vol->IsLoaded())
        return;
    sweepAxis_ = axis;
    if (sweepOrdinalCount() <= 0)
        return;
    if (!sweepTimer_) {
        sweepTimer_ = new QTimer(this);
        sweepTimer_->setTimerType(Qt::CoarseTimer);
        connect(sweepTimer_, &QTimer::timeout, this, [this]() { onSweepTick(); });
    }
    sweepTimer_->setInterval(1000 / std::clamp(fps, 1, 60));
    sweepOrdinal_ = 0;
    sweepPrefetchInFlight_ = 0;
    sweepTimer_->start();
    // #148：首帧与导出序列对齐——直接把滑杆拨到 ordinal 0（复用 setInline/
    // setCrossline/setTimeSample 链路：内建吸附/取数/changed 广播）。缺了这步
    // 首个显示帧是 ordinal 1，而 onSweepTick 先导后进，导出的 frame_00000
    // 是扫掠开始前的陈旧切片、ordinal 0 帧永远导不出去。
    const int firstIndex = sweepIndexAt(0);
    if (axis == SgySliceType::Inline)
        setInline(firstIndex);
    else if (axis == SgySliceType::Xline)
        setCrossline(firstIndex);
    else
        setTimeSample(firstIndex);
    emit sweepFrameChanged(firstIndex);
}

void Seismic3DViewPanel::pauseSweep() {
    if (sweepTimer_)
        sweepTimer_->stop();
}

void Seismic3DViewPanel::resumeSweep() {
    auto vol = volume();
    if (sweepTimer_ && !sweepTimer_->isActive() && vol && vol->IsLoaded() &&
        sweepOrdinalCount() > 0)
        sweepTimer_->start();
}

void Seismic3DViewPanel::stopSweep() {
    if (sweepTimer_)
        sweepTimer_->stop();
    sweepPrefetchInFlight_ = 0;
}

void Seismic3DViewPanel::setSweepExportDir(const QString &dir) {
    sweepExportDir_ = dir;
}

void Seismic3DViewPanel::onSweepTick() {
    auto vol = volume();
    const int count = sweepOrdinalCount();
    if (!vol || !vol->IsLoaded() || count <= 0) {
        stopSweep();
        return;
    }

    // 1. 导出上一帧（先导后进：grabFramebuffer 同步渲染当前态；无 GL 回退
    //    导缓存切片 rgba——数据帧序列，离屏/回退态仍可出片）
    if (!sweepExportDir_.isEmpty()) {
        QImage img = viewport_->grabViewportImage();
        if (img.isNull()) {
            const std::size_t si = static_cast<std::size_t>(sweepSlot()) % 3;
            if (cachedReady_[si]) {
                const SgySliceImage &c = cachedSlices_[si];
                img = QImage(c.rgba.data(), c.width, c.height,
                             c.width * 4, QImage::Format_RGBA8888).copy();
            }
        }
        if (!img.isNull()) {
            const QString path = QDir(sweepExportDir_).filePath(
                QStringLiteral("frame_%1.png").arg(sweepExportedCount_, 5, 10, QLatin1Char('0')));
            if (img.save(path, "PNG"))
                ++sweepExportedCount_;
        }
    }

    // 2. 帧推进（回绕）+ 滑杆跟手（slider 路径内建吸附/取数/changed 广播）
    sweepOrdinal_ = (sweepOrdinal_ + 1) % count;
    const int index = sweepIndexAt(sweepOrdinal_);
    if (sweepAxis_ == SgySliceType::Inline)
        setInline(index);
    else if (sweepAxis_ == SgySliceType::Xline)
        setCrossline(index);
    else
        setTimeSample(index);

    // 3. 前向预取窗口（结果丢弃、只暖 SgyDataCache；在途 > 2×窗口 时让路
    //    UI 交互取数——预取是播放加速，不该抢并发闸）
    if (taskSvc_ && sweepPrefetchInFlight_ < kSweepPrefetchWindow * 2) {
        for (int k = 1; k <= kSweepPrefetchWindow; ++k) {
            const int ord = (sweepOrdinal_ + k) % count;
            const int idx = sweepIndexAt(ord);
            ++sweepPrefetchInFlight_;
            QPointer<Seismic3DViewPanel> guard(this);
            taskSvc_->startSliceExtraction(
                vol, sweepAxis_, idx,
                [guard](bool, std::shared_ptr<const SgySliceImage>, const QString &) {
                    if (guard)
                        --guard->sweepPrefetchInFlight_;
                }, pagedPath_, true);
        }
    }
    emit sweepFrameChanged(index);
}

} // namespace seismic
