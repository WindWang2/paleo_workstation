// 层：视图
#pragma once
#include "domain/wellsection.h"
#include "wellsectionscene.h"
#include "wellsectionstyle.h"

#include <QImage>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

// ui/wellsection — WellSectionPanel：连井剖面图件面板。工具行（纯图标
// QToolButton，全部设置收在图标后）+ 吸顶版头 + 剖面视图 + 空态层 +
// 状态行。视图只发意图（dataRequested/seismicRequested/wellIdsChanged/
// wellClicked），取数与地震编排由壳层接 workflow。
// 图件本身是纸面文档，不随 UI 暗色翻转；工具行/状态行是 chrome，随 token。
class QLabel;
class QPushButton;
class QToolButton;
class QGraphicsScene;
class QMenu;
class QAction;
class SelectionContext;

class WellSectionPanel : public QWidget
{
  Q_OBJECT
  public:
    struct WellChoice {
      QString id, name;
      bool hasCoordinates = false;
      double x = qQNaN(), y = qQNaN(); // 井位排序（PCA）用
    };
    explicit WellSectionPanel(SelectionContext *ctx, QWidget *parent = nullptr);

    void setWellChoices(const QVector<WellChoice> &choices);
    void setMnemonicChoices(const QStringList &mnemonics);
    // 程序化恢复：发 dataRequested，不发 wellIdsChanged。
    void setWellIds(const QStringList &ids);
    // 平面/图层树选井 → PCA 井序一键成剖面（用户动作：发 wellIdsChanged）。
    // 返回实际采纳的井序（空 = 选井不可用/未选中）。
    QStringList generateFromSelection();
    QStringList wellIds() const { return m_ids; }
    // 数据回填：按回复顺序接管井集；地震开且 ≥2 井时发 seismicRequested。
    void setSection(const QVector<wellsection::Well> &wells);
    QVector<wellsection::Well> wells() const { return m_wells; }
    // 缝数与当前井数不吻合 → 忽略。
    void setSeismicStrip(const wellsection::SeismicStrip &strip);
    void setSeismicAvailable(bool available, const QString &reason);
    void setBusy(bool busy);
    // 程序化选中某井（栅状图交点井联动用；不发 wellClicked/ctx 回声）。
    void selectWell(const QString &wellId);
    void setWarnings(const QStringList &warnings);
    wellsection::SectionTemplate sectionTemplate() const { return m_tpl; }
    // 仅应用不换数据请求语义：mnemonics 变了才发 dataRequested。
    void setSectionTemplate(const wellsection::SectionTemplate &t);
    QString themeId() const { return m_themeId; }
    void setThemeId(const QString &id);
    bool highlightEnabled() const { return m_highlightOn; }
    void setHighlightEnabled(bool on);
    bool seismicEnabled() const { return m_seismicOn; }
    void setSeismicEnabled(bool on); // on + ≥2 井 → seismicRequested
    // 断层投绘开关：on + ≥2 井 → faultsRequested（数据由壳层接 workflow）。
    bool faultsEnabled() const { return m_faultsOn; }
    void setFaultsEnabled(bool on);
    // 投绘结果回填：status 非空 = 无线可画（按钮 tooltip/状态行提示）。
    void setFaultTraces(const QVector<wellsection::FaultTrace> &traces,
                        const QString &status);
    void setFaultsAvailable(bool available, const QString &reason);
    QString flattenTop() const { return m_datum.flattenTop; }
    void setFlattenTop(const QString &top); // "" = 不拉平（糖接口：切 Flatten）
    wellsection::Datum datum() const { return m_datum; }
    // 基准面三模式（井深/海拔/拉平）：只改视图偏移与轴标签，井数据不动。
    void setDatum(const wellsection::Datum &d);
    // 井距模式：等距 / 按井口距离比例（视图偏好，QSettings 持久化）。
    wellsection::SpacingMode spacingMode() const { return m_spacing; }
    void setSpacingMode(wellsection::SpacingMode mode);
    // 深度显示域：MD / TVD（井斜换算在渲染映射，井数据不动）。切域重发
    // 数据请求——在途旧结果按 workflow 世代丢弃（一致性口径）。
    wellsection::DepthDomain depthDomain() const { return m_domain; }
    void setDepthDomain(wellsection::DepthDomain domain);
    // 层位连线改接（用户编辑产物；井对无序键，井序重排不失效）。
    QVector<wellsection::LinkOverride> linkOverrides() const { return m_linkOverrides; }
    void setLinkOverrides(const QVector<wellsection::LinkOverride> &overrides);
    // 用户右键断开/重连某缝某顶（gap = 左井序号）。
    void toggleLink(int gap, const QString &topName, bool connect);
    void fitToView();
    // 版头 + 全幅图体导出到 paper 底图（scale 缩放像素）。
    QImage renderImage(double scale = 1.0) const;
    bool exportTo(const QString &path) const; // .png / .pdf
    // 层位井深表 CSV 文本（基准面模式列在表头；MD 值不随模式变）。
    QString topsCsv() const;

    // ---- 测试钩子 ----
    int linkCount() const;
    // 高亮真正画出时的 active 顶名，否则 ""。
    QString highlightedFormation() const;
    bool isWellSelected(const QString &id) const;
    // 某井某顶线的场景 y；缺失 → NaN。
    qreal topLineY(const QString &wellId, const QString &top) const;
    QString gapReason(int gap) const;
    QString statusText() const;
    // 深度道题注文本（随基准面/域；导出图口径标签的字符串断言通道）。
    QString depthCaption() const;
    // TVD 域名行角标文本（无测斜/坏表如实标注；MD 域/正常井 → 空串）。
    QString headerBadgeText(const QString &wellId) const;
    // hover 井柱读数文案（TVD 域三态如实口径：数值/无测斜注明按井深绘
    // 制/坏表无读数）——字符串断言通道，与 View 的 hoverChanged 同一单源。
    QString hoverReadoutTextFor(const QString &wellId, double md,
                                const QString &zoneName = QString()) const;
    // 岩性道题注文本（方向 69 来源标注：解释段带资产来源 / 无资产回落
    // 「推断·<曲线> 截断」）——字符串断言通道，与版头绘制同一口径。
    QString lithoTrackCaption(const QString &wellId) const;
    double pxPerMeter() const { return m_st.pxPerMeter; }
    qreal gapWidth() const { return m_st.gapPx; }
    // 测试钩子：列左缘 x / 第 i 缝宽（比例井距模式的断言面）。
    qreal columnX(int i) const { return m_st.columnLeft(i); }
    qreal gapWidthAt(int i) const { return m_st.gapWidth(i); }
    int faultTraceCount() const { return m_st.faultTraces.size(); }
    // 解释来源按钮可见性（fence 剖面隐藏——选择态语义面在主剖面 workflow
    // + 工程库，栅内不放死按钮；方向 98）。
    void setLithoSourceButtonVisible(bool visible);

  signals:
    void dataRequested(const QStringList &wellIds, const QStringList &mnemonics);
    void seismicRequested();
    void faultsRequested();
    void fenceRequested(); // 打开栅状图（剖面网）——壳层装配 WellSectionFenceWidget
    // 仅用户驱动（选井/拖排/移除）——持久化钩子。
    void wellIdsChanged(const QStringList &wellIds);
    // 仅用户驱动（连线断开/重连）——持久化钩子（store 版本推进）。
    void linkOverridesChanged(const QVector<wellsection::LinkOverride> &overrides);
    void wellClicked(const QString &wellId);
    // 图片道锚双击（方向 79）：壳层打开锚深编辑对话框（catalog 经壳持有，
    // 视图不碰数据）。depthMd 为当前锚深（双击语境即已锚定照片）。
    void imageAnchorEditRequested(const QString &wellId,
                                  const QString &assetId, double depthMd);
    // 解释岩性来源编辑（方向 98）：壳层枚举该井解释资产（workflow 供给）、
    // 弹选择对话框、落库并重取。wellId = 当前选中井（无选中 = 首井）。
    void lithoSourceEditRequested(const QString &wellId);
    // 仅用户驱动（域/井距菜单动作）——fence 三处一致性传播钩子（程序化
    // setter 不发，防回声环路）。
    void depthDomainChanged(wellsection::DepthDomain domain);
    void spacingModeChanged(wellsection::SpacingMode mode);

  private:
    void rebuildFiltered();   // wells → 过滤 + 偏移 + 窗口 + 顶名序
    void rebuildItems();      // 井集变化 → 重建列/缝项
    void applyLayout();       // 布局参数落位（版本+1、项 relayout、sceneRect）
    void updateStatus();
    void updateSelection(const QStringList &ids, const QString &origin);
    void updateHighlight();
    void syncGapToolTips();    // 缝 tooltip 随状态（reason）刷新
    void clearStrip();
    // 井对 id 间接寻址的改接（菜单动作重建安全）。
    void toggleLinkForPair(const QString &aId, const QString &bId,
                           const QString &topName, bool connect);
    void moveWell(int from, int to); // 用户拖排
    void removeWellAt(int index);    // 用户右键移除
    void rebuildGapWidths();         // 间距模式/井集/gapPx 变化后重算逐缝宽
    void openWellsDialog();
    void openTracksDialog();
    void applyThemeFromMenu(const QString &id); // 用户动作 → 写设置
    void applyDatumFromMenu(const wellsection::Datum &d); // 用户动作 → 写设置
    void applyTemplateFromDialog(const wellsection::SectionTemplate &t);
    void syncToolbarState();
    void ensureActiveIntervalVisible();

    SelectionContext *m_ctx = nullptr;
    QVector<WellChoice> m_choices;
    QStringList m_mnemonicChoices;
    QStringList m_ids;                       // 当前井序（== m_wells 顺序）
    QVector<wellsection::Well> m_wells;      // 原始（未过滤）
    wellsection::SectionTemplate m_tpl;      // 工作模板（用户可改）
    wellsectionui::RenderState m_st;
    QString m_themeId = QStringLiteral("classic");
    bool m_highlightOn = true;
    bool m_seismicOn = false;
    bool m_seismicAvailable = false;
    QString m_seismicReason;
    bool m_faultsOn = false;
    bool m_faultsAvailable = false;
    QString m_faultsReason;
    QString m_faultStatus; // 最近一次投绘状态（空 = 正常出线）
    bool m_busy = false;
    bool m_autofit = true;
    bool m_refitPending = false; // resize 触发的 refit 合并标志（0ms singleShot）
    QStringList m_warnings;
    QString m_hoverText;
    QString m_selectedId;
    wellsection::Datum m_datum; // 基准面（默认井深；空 flattenTop 的 Flatten 视作 Depth）
    wellsection::SpacingMode m_spacing = wellsection::SpacingMode::Equal;
    wellsection::DepthDomain m_domain = wellsection::DepthDomain::MD;
    QVector<wellsection::LinkOverride> m_linkOverrides;

    QGraphicsScene *m_scene = nullptr;
    wellsectionui::View *m_view = nullptr;
    wellsectionui::HeaderWidget *m_header = nullptr;
    QVector<wellsectionui::ColumnItem *> m_colItems;
    QVector<wellsectionui::GapItem *> m_gapItems;
    wellsectionui::FaultOverlayItem *m_faultItem = nullptr;

    QToolButton *m_wellsBtn = nullptr;
    QToolButton *m_fromSelBtn = nullptr;
    QToolButton *m_tracksBtn = nullptr;
    QToolButton *m_lithoBtn = nullptr;
    QToolButton *m_themeBtn = nullptr;
    QToolButton *m_flattenBtn = nullptr;
    QToolButton *m_spacingBtn = nullptr;
    QToolButton *m_seismicBtn = nullptr;
    QToolButton *m_faultBtn = nullptr;
    QToolButton *m_fenceBtn = nullptr;
    QToolButton *m_fitBtn = nullptr;
    QToolButton *m_exportBtn = nullptr;
    QLabel *m_status = nullptr;
    QWidget *m_empty = nullptr;
    QAction *m_highlightAct = nullptr;
    QMenu *m_themeMenu = nullptr;
    QMenu *m_flattenMenu = nullptr;
    QMenu *m_spacingMenu = nullptr;
};
