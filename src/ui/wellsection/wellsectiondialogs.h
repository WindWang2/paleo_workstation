// 层：视图
#pragma once
#include "wellsectionpanel.h"
#include "wellsectionstyle.h"

#include <QDialog>
#include <QHash>
#include <QPointF>

// ui/wellsection — 连井剖面的两个设置对话框：选井/排序 与 井道配置。
class QListWidget;
class QLineEdit;
class QComboBox;
class QDoubleSpinBox;
class QSpinBox;
class QCheckBox;
class QToolButton;
class QRadioButton;

// 选井：当前井按剖面序在前（勾选），其余按名排序在后；支持搜索、内拖
// 排序、上移/下移、「地图选中」勾选追加、「按井位排序」（坐标 PCA 主轴
// 投影，无坐标的排末）。
class WellSectionWellsDialog : public QDialog
{
  Q_OBJECT
  public:
    WellSectionWellsDialog(const QVector<WellSectionPanel::WellChoice> &choices,
                           const QStringList &current,
                           const QStringList &mapSelection,
                           QWidget *parent = nullptr);
    QStringList selectedIds() const; // 勾选项按列表序

  private:
    void checkAppend(const QStringList &ids);   // 地图选中
    void sortCheckedByPosition();               // 按井位排序
    void moveCurrent(int delta);                // 上移/下移

    QListWidget *m_list;
    QLineEdit *m_search;
    QHash<QString, QPointF> m_coords; // id → 井位（hasCoordinates 才有）
};

// 井道配置：左 = 道列表（可拖排）+ 增删；右 = 选中道的表单 +「参与连井
// 的分层」过滤节。result() 取当前表单已回写的完整模板。
class WellSectionTracksDialog : public QDialog
{
  Q_OBJECT
  public:
    WellSectionTracksDialog(const wellsection::SectionTemplate &t,
                            const QStringList &mnemonics,
                            QWidget *parent = nullptr);
    void setTopNames(const QStringList &names); // 自定义分层的候选名单
    wellsection::SectionTemplate result() const;

  private:
    void loadTrack(int row);            // 表单 ← m_tracks[row]
    void storeTrack(int row);           // m_tracks[row] ← 表单
    void refreshList();                 // 列表 ← m_tracks
    void updateFormVisibility();
    void addTrack(wellsection::TrackKind kind);
    void removeCurrent();
    void restoreDefaults();

    wellsection::SectionTemplate m_tpl; // 工作副本（分层过滤即刻生效）
    QVector<wellsection::TrackSpec> m_tracks;
    QStringList m_mnemonics;
    int m_loading = 0;  // 表单装载期抑制回写/级联
    int m_formRow = -1; // 当前表单对应的道序号（切行前回写）

    QListWidget *m_trackList;
    QLineEdit *m_title;
    QSpinBox *m_width;
    // 曲线道表单（曲线2 由「叠加第二条」开关）。
    QWidget *m_curveForm;
    QComboBox *m_curve1;
    QDoubleSpinBox *m_min1;
    QDoubleSpinBox *m_max1;
    QCheckBox *m_log1;
    QToolButton *m_color1;
    QCheckBox *m_curve2On;
    QWidget *m_curve2Row;
    QComboBox *m_curve2;
    QDoubleSpinBox *m_min2;
    QDoubleSpinBox *m_max2;
    QCheckBox *m_log2;
    QToolButton *m_color2;
    QCheckBox *m_sandFill;
    QDoubleSpinBox *m_cutoff;
    // 岩性道表单。
    QWidget *m_lithoForm;
    QComboBox *m_lithoSource;
    QDoubleSpinBox *m_lithoCutoff;
    // 参与连井的分层。
    QRadioButton *m_filterMapping;
    QRadioButton *m_filterAll;
    QRadioButton *m_filterCustom;
    QListWidget *m_topList;
};
