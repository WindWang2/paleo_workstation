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
    QStringList wellIds() const { return m_ids; }
    // 数据回填：按回复顺序接管井集；地震开且 ≥2 井时发 seismicRequested。
    void setSection(const QVector<wellsection::Well> &wells);
    QVector<wellsection::Well> wells() const { return m_wells; }
    // 缝数与当前井数不吻合 → 忽略。
    void setSeismicStrip(const wellsection::SeismicStrip &strip);
    void setSeismicAvailable(bool available, const QString &reason);
    void setBusy(bool busy);
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
    QString flattenTop() const { return m_flattenTop; }
    void setFlattenTop(const QString &top); // "" = 不拉平
    void fitToView();
    // 版头 + 全幅图体导出到 paper 底图（scale 缩放像素）。
    QImage renderImage(double scale = 1.0) const;
    bool exportTo(const QString &path) const; // .png / .pdf

    // ---- 测试钩子 ----
    int linkCount() const;
    // 高亮真正画出时的 active 顶名，否则 ""。
    QString highlightedFormation() const;
    bool isWellSelected(const QString &id) const;
    // 某井某顶线的场景 y；缺失 → NaN。
    qreal topLineY(const QString &wellId, const QString &top) const;
    QString gapReason(int gap) const;
    QString statusText() const;
    double pxPerMeter() const { return m_st.pxPerMeter; }
    qreal gapWidth() const { return m_st.gapPx; }

  signals:
    void dataRequested(const QStringList &wellIds, const QStringList &mnemonics);
    void seismicRequested();
    // 仅用户驱动（选井/拖排/移除）——持久化钩子。
    void wellIdsChanged(const QStringList &wellIds);
    void wellClicked(const QString &wellId);

  private:
    void rebuildFiltered();   // wells → 过滤 + 偏移 + 窗口 + 顶名序
    void rebuildItems();      // 井集变化 → 重建列/缝项
    void applyLayout();       // 布局参数落位（版本+1、项 relayout、sceneRect）
    void updateStatus();
    void updateSelection(const QStringList &ids, const QString &origin);
    void updateHighlight();
    void syncGapToolTips();    // 缝 tooltip 随状态（reason）刷新
    void clearStrip();
    void moveWell(int from, int to); // 用户拖排
    void removeWellAt(int index);    // 用户右键移除
    void openWellsDialog();
    void openTracksDialog();
    void applyThemeFromMenu(const QString &id); // 用户动作 → 写设置
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
    bool m_busy = false;
    bool m_autofit = true;
    bool m_refitPending = false; // resize 触发的 refit 合并标志（0ms singleShot）
    QStringList m_warnings;
    QString m_hoverText;
    QString m_selectedId;
    QString m_flattenTop; // 空 = 不拉平

    QGraphicsScene *m_scene = nullptr;
    wellsectionui::View *m_view = nullptr;
    wellsectionui::HeaderWidget *m_header = nullptr;
    QVector<wellsectionui::ColumnItem *> m_colItems;
    QVector<wellsectionui::GapItem *> m_gapItems;

    QToolButton *m_wellsBtn = nullptr;
    QToolButton *m_tracksBtn = nullptr;
    QToolButton *m_themeBtn = nullptr;
    QToolButton *m_flattenBtn = nullptr;
    QToolButton *m_seismicBtn = nullptr;
    QToolButton *m_fitBtn = nullptr;
    QToolButton *m_exportBtn = nullptr;
    QLabel *m_status = nullptr;
    QWidget *m_empty = nullptr;
    QAction *m_highlightAct = nullptr;
    QMenu *m_themeMenu = nullptr;
    QMenu *m_flattenMenu = nullptr;
};
