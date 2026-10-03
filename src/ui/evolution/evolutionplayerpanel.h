// 层：视图
#pragma once
#include <QStringList>
#include <QWidget>

class SelectionContext;
class QgisLayerService;
class QLabel;
class QToolButton;
class QTimer;

// ui/evolution/evolutionplayerpanel — 多期演化动览（方向35）。
//
// 层位序连续切换的步进器：◀上一期 / ▶播放（QTimer 功能性步进，DESIGN.md
// Motion 禁编排动画——帧间即时切换无淡入）/ ▶下一期 / 定格导出。步进顺序
// 严格按 mappingHorizons()（浅→深）。切换动作与 HorizonChipBar 同口径：
// SelectionContext::setActiveHorizon（联动广播）+ QgisLayerService::
// setActiveHorizon（物化目标层位、释放其他层位）；编辑中拒切并给 reason。
// 视图只发信号不干活：定格导出为 intent（frameExportRequested），由壳侧
// 接画布抓帧与产物登记。
// 视觉按 DESIGN.md：dock-panel 面板底/描边、数值标签 JetBrains Mono 9pt
//（tnum）、工具按钮 hover=surface-alt。
class EvolutionPlayerPanel : public QWidget
{
  Q_OBJECT
  public:
    EvolutionPlayerPanel( SelectionContext *selection, QgisLayerService *layers,
                          QWidget *parent = nullptr );

    // 帧序 = mappingHorizons()（供测试断言步进顺序与层位序一致）。
    QStringList frameOrder() const;
    // 当前定格层位（空 = 尚无激活帧）。
    QString currentHorizon() const;
    bool isPlaying() const;
    // 播放帧间隔（毫秒）；只影响 QTimer 周期，测试可调短。
    void setPlayIntervalMs( int ms );

  signals:
    // 定格导出 intent：壳侧按当前画布抓帧登记 OUTPUT 资产。
    void frameExportRequested( const QString &horizon );
    // 与 HorizonChipBar 同语义：编辑中拦截切换的文案。
    void horizonSwitchRefused( const QString &reason );

  private:
    void buildUi();
    void applyFrame( const QString &horizon );
    // delta = +1 向深、-1 向浅，沿 mappingHorizons() 序；越界不动。
    void stepBy( int delta );
    void setPlaying( bool playing );
    // 与 HorizonChipBar 共用的切换口径（编辑守卫 + 双服务同步）。
    bool switchHorizon( const QString &horizon );

    SelectionContext *m_selection = nullptr;
    QgisLayerService *m_layers = nullptr;
    QStringList m_frames;
    int m_index = -1;
    QTimer *m_timer = nullptr;
    QLabel *m_frameLabel = nullptr;
    QToolButton *m_playButton = nullptr;
    QToolButton *m_prevButton = nullptr;
    QToolButton *m_nextButton = nullptr;
    QToolButton *m_exportButton = nullptr;
};
