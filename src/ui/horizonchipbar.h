#pragma once
#include <QStringList>
#include <QWidget>

class SelectionContext;
class QgisLayerService;

// ui/horizonchipbar — 阶段E 的层位 chip 条（wave/mapping-pipeline）。
//
// 列出当前工程的有序层序界面（domain/mappingHorizons() = AreaRules
// sequenceBoundaries，浅→深），不受井分层或图层清单里的其他名字影响——
// 集合外的层位永远不进 chip。层位名单是工程级参数：工程打开后由
// reloadHorizons() 按新词表重建。
// 点击 chip：SelectionContext::setActiveHorizon（联动广播）+
// QgisLayerService::setActiveHorizon（沿用既有按层位懒加载：物化目标层位、
// 释放其他层位实例）。反向同步：chip 跟随 activeHorizonChanged 高亮，
// 集合外的 activeHorizon 不点亮任何 chip。
// 视觉按 DESIGN.md chip/chip-active token：胶囊形、surface 底/描边/muted
// 文本；选中 primary 底/on-primary 文本。
class HorizonChipBar : public QWidget
{
  Q_OBJECT
  public:
    HorizonChipBar(SelectionContext *selection, QgisLayerService *layers, QWidget *parent = nullptr);

    // chip 顺序 = mappingHorizons()（供测试断言固定集合）。
    QStringList chipNames() const;
    // 指定层位 chip 是否处于选中态（集合外层位恒 false）。
    bool isChipActive(const QString &horizon) const;
    // 工程打开/切换后调用：按 mappingHorizons() 当前集合重建 chip。
    void reloadHorizons();

  private:
    void buildChips();
    void applyActive(const QString &horizon);
    // 阶段E — 无栅格声明的层位 chip 禁用（「这一阶段还没有这个层位的栅格」），
    // layerDeclared 后重算；可用性以 LayerManifest 的 raster 声明为准。
    void applyAvailability();
    SelectionContext *m_selection = nullptr;
    QgisLayerService *m_layers = nullptr;
};
