// 层：视图
// Seismic3DViewportWidget 场景内容设置器：叠层/传递函数/井/断层/层位/次级体（file-budget 拆分）

#include "seismic3dviewportwidget.h"
#include <QKeyEvent>
#include <QPainter>
#include "../paleotheme.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <glm/ext/matrix_projection.hpp>
#include <limits>
#include <glm/gtc/matrix_inverse.hpp>

namespace seismic {

void Seismic3DViewportWidget::setStackVisible(bool visible) {
    sliceRenderer_.SetStackVisible(visible);
    update();
}

bool Seismic3DViewportWidget::isStackVisible() const {
    return sliceRenderer_.IsStackVisible();
}

void Seismic3DViewportWidget::setStackLayerCount(int count) {
    const int clamped = std::clamp(count, 0, SeismicSliceRenderer::kMaxStackLayers);
    if (clamped == stackLayerCount_)
        return;
    // 只显示前 N 层（层序 = 体积分布序）
    for (int i = 0; i < SeismicSliceRenderer::kMaxStackLayers; ++i)
        sliceRenderer_.SetStackLayerVisible(i, i < clamped);
    stackLayerCount_ = clamped;
    update();
}

void Seismic3DViewportWidget::setSliceAlpha(float alpha) {
    sliceRenderer_.SetSliceAlpha(alpha);
    update();
}

// D7.1：LUT 重传（256B）。enable 翻转时切片/堆叠层纹理需按新模式重喂——
// 由面板持有 values 缓存方（cachedSlices_/堆叠缓存）负责，视口只管 LUT。
void Seismic3DViewportWidget::setTransferFunction(const std::vector<unsigned char> &lutRgba, bool enable) {
    tfLutBytes_ = lutRgba;
    tfActive_ = enable;
    if (!glInitialized_ || lutRgba.size() != 256 * 4)
        return;
    makeCurrent();
    sliceRenderer_.SetTransferFunction(this, lutRgba, enable);
    doneCurrent();
    update();
}

QImage Seismic3DViewportWidget::grabViewportImage() {
    if (!glInitialized_)
        return QImage();
    return grabFramebuffer();
}

void Seismic3DViewportWidget::setWells(const std::vector<Seismic3DWell> &wells) {
    wellsForLabels_ = wells; // D7.3 标注叠绘原料（无 GL 也更新）
    if (glInitialized_ && volume_ && volume_->IsLoaded()) {
        makeCurrent();
        frameRenderer_.UpdateWells(this, *volume_, wells);
        doneCurrent();
        update();
        wellsPending_ = false;
    } else {
        // GL 未就绪（或体未到）不得静默丢弃：暂存副本，initializeGL /
        // setVolume 就绪后补传（同 horizonsPending_ 契约）
        wellsPending_ = true;
    }
}

// ---- D7.3 层位面 ----
void Seismic3DViewportWidget::setFaultSceneMesh(const FaultSceneMesh &mesh) {
    faultMesh_ = mesh;
    faultRenderer_.setMesh(mesh);
    if (glInitialized_) {
        makeCurrent();
        faultRenderer_.Update(this, mesh);
        doneCurrent();
        update();
    } else {
        faultMeshPending_ = true;
    }
}

void Seismic3DViewportWidget::clearFaultSceneMesh() {
    setFaultSceneMesh(FaultSceneMesh{});
    faultFit_ = {};
}

int Seismic3DViewportWidget::faultSceneTriangleCount() const {
    return faultRenderer_.triangleCount();
}

void Seismic3DViewportWidget::fitFaultSurfaces(float aspect) {
    const float safeAspect = aspect > 0.f ? aspect : 1.f;
    faultFit_ = fitFaultSceneCamera(camera_, faultMesh_, safeAspect);
}

bool Seismic3DViewportWidget::faultSceneContainsBounds(float aspect) const {
    const float safeAspect = aspect > 0.f ? aspect : 1.f;
    return faultSceneBoundsInsideFrustum(camera_, faultFit_, safeAspect);
}

void Seismic3DViewportWidget::setHorizons(const std::vector<Seismic3DHorizonSurface> &items) {
    horizonItems_ = items;
    if (!volume_ || !volume_->IsLoaded())
        return;
    if (glInitialized_) {
        makeCurrent();
        horizonRenderer_.UpdateHorizons(this, *volume_, horizonItems_);
        doneCurrent();
        update();
    } else {
        horizonsPending_ = true; // initializeGL 补传
    }
}

void Seismic3DViewportWidget::setHorizonVisible(int index, bool visible) {
    if (index >= 0 && index < int(horizonItems_.size()))
        horizonItems_[static_cast<std::size_t>(index)].visible = visible;
    horizonRenderer_.SetHorizonVisible(index, visible);
    update();
}

bool Seismic3DViewportWidget::isHorizonVisible(int index) const {
    if (index < 0 || index >= int(horizonItems_.size()))
        return false;
    return horizonItems_[static_cast<std::size_t>(index)].visible;
}

void Seismic3DViewportWidget::setHorizonsVisible(bool visible) {
    horizonRenderer_.SetVisible(visible);
    update();
}

void Seismic3DViewportWidget::setWellLabelsVisible(bool visible) {
    wellLabelsVisible_ = visible;
    update();
}

void Seismic3DViewportWidget::setSecondaryVolume(std::shared_ptr<const SgyVolume> secondary) {
    secondaryVolume_ = std::move(secondary);
    if (glInitialized_ && volume_ && volume_->IsLoaded()) {
        makeCurrent();
        frameRenderer_.SetSecondaryVolume(this, *volume_, secondaryVolume_);
        doneCurrent();
        update();
        secondaryPending_ = false;
    } else {
        // 同 setWells：GL/体未就绪时暂存，就绪后补传（次级体轮廓不得静默丢失）
        secondaryPending_ = true;
    }
}

} // namespace seismic
