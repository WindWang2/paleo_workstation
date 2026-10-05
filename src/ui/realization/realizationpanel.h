// 层：视图
#pragma once

#include <QString>
#include <QStringList>
#include <QWidget>

class DataCatalog;
class QComboBox;
class QLabel;
class QListWidget;
class QPushButton;
class QSlider;
class QTimer;

// ui/realization/realizationpanel —— realization 集合查看面（方向 47）。
//
// 视图纪律：只发信号不干活——成员切换/统计面/差值/派生全是 intent 信号，
// 壳侧（paleomainwindow_attach）接 RealizationWorkflow + QgisLayerService
// 完成物化与画布切换。本面板对 catalog 只做只读查询
// （catalog/realizationset.h 的枚举面；catalog::changed 驱动刷新）。
//
// 诚实面（契约钉死，见 src/catalog/realizationset.h 头注）：
//   · 集合不全 → setCombo 条目带「缺成员 #i」标记，memberCombo 缺号位
//     如实置灰（可选中查看时发拒绝文案）；
//   · N=1 集合 → memberSlider 禁走、统计行标「单成员·无不确定性」；
//   · 统计口径词与 catalog token 同源（statisticDisplayLabel）——图签、
//     版本面板、本面逐字一致，不混称方差/分位。
//   · realization 是等概率实现族——文案不出现「概率校准」「置信度」。
class RealizationPanel : public QWidget
{
  Q_OBJECT
  public:
    explicit RealizationPanel( QWidget *parent = nullptr );

    // catalog 换绑（projectOpened 后调用；可空——空则面板如实空态）。
    // 内部接 catalog::changed 做刷新。
    void bindCatalog( DataCatalog *catalog );

    QString currentSetId() const { return m_currentSetId; }
    int currentMemberIndex() const { return m_currentMember; }
    bool isPlaying() const { return m_playing; }
    // 播放帧序（成员 index 升序）；供测试断言与缺号跳帧语义。
    QList<int> frameOrder() const;
    void setPlayIntervalMs( int ms );

  signals:
    // 同画布成员切换 intent：壳侧物化 realset.<setId>.m<index> 图层并切换
    // 可见性（同集合内互斥切换）。
    void memberShowRequested( const QString &setId, int index );
    // 统计面查看 intent：壳侧物化 realset.<setId>.stat.<token> 图层并把
    // 不确定性图签切到该口径。
    void statShowRequested( const QString &setId, const QString &token );
    // 派生统计 intent（成员 ≥2 才有意义，壳侧如实拒绝单成员集合）。
    void deriveStatsRequested( const QString &setId );
    // 两集合均值差 intent。
    void diffRequested( const QString &setIdA, const QString &setIdB );
    // 壳侧状态栏/图签用——面板内失败与缺席都以可读文案上报。
    void statusMessage( const QString &text );

  private:
    void rebuildSets();
    void rebuildMembers();
    void rebuildStats();
    void onTick();
    void setPlaying( bool playing );
    // 集合缺号汇总词：「缺 #3 · #5」；完整 → 空串。
    QString missingSummary() const;

    DataCatalog *m_catalog = nullptr; // 非拥有
    QString m_currentSetId;
    int m_currentMember = -1;
    bool m_playing = false;
    int m_framePos = -1; // frameOrder 内的位置
    QList<int> m_missing;

    QComboBox *m_setCombo = nullptr;
    QSlider *m_memberSlider = nullptr;
    QComboBox *m_memberCombo = nullptr;
    QLabel *m_memberState = nullptr;   // 单成员/缺号诚实行
    QListWidget *m_statList = nullptr; // 在场统计面 token 列表
    QPushButton *m_deriveButton = nullptr;
    QComboBox *m_diffA = nullptr;
    QComboBox *m_diffB = nullptr;
    QPushButton *m_diffButton = nullptr;
    QPushButton *m_playButton = nullptr;
    QLabel *m_frameLabel = nullptr;
    QTimer *m_timer = nullptr;
};
