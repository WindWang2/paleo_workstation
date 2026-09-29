// 层：视图
#pragma once

#include <QComboBox>
#include <QLabel>
#include <QToolButton>
#include <QWidget>

#include <memory>

#include "wellcompositecanvas.h"
#include "domain/wellcompositemodel.h"
#include "wellcompositestore.h"
#include "depthtools.h"
#include "exportengine.h"

// ui/wellcomposite/ — WellCompositePanel: ResFormStar 风格单井综合柱状图总装面板
//
// 包含置顶工具条（比例尺、缩放控制、井名、深度读数条、书签/跳转/吸附/编辑开关）
// 与多井道画布（WellCompositeCanvas）+ 隐藏道管理条（HiddenTrackBar）+
// 底部位置图例条，统一对接 LAS 曲线与 XML/Excel 综合柱状图数据源。
//
// 深度升级（wave/wellcomposite-deep）：
//   D1.5/D1.6/D1.10 道菜单/配置/隐藏条接线；D1.8 会话记忆；
//   D2.2 区间统计对话框；D2.3 标注钉；D2.6 书签；D2.7 Ctrl+G；
//   D2.11 深度读数条；D2.12 gap 阈值；D3.14 编辑模式工具条。

namespace WellComposite
{

class WellPositionLegendWidget;
class HiddenTrackBar;
class EditSession;

class WellCompositePanel : public QWidget
{
  Q_OBJECT

public:
  explicit WellCompositePanel(QWidget *parent = nullptr);
  ~WellCompositePanel() override;

  WellCompositeCanvas *canvas() const { return m_canvas; }
  WellPositionLegendWidget *legendWidget() const { return m_legendWidget; }

  // 加载并装配中国石油标准综合柱状图 XML (SpreadsheetML)
  bool loadComprehensiveXml(const QString &xmlPath);

  // 加载并装配单井 LAS 曲线（支持关联地层分层道与 1-4 根曲线分道合并显示）
  bool loadLasCurves(const QString &wellName, const QVector<CurveData> &curves,
                     const QVector<FormationInterval> &formations = {});

  // 设置并显示井名；reference=true 时徽章标识为参考井（辅助资料内的井，非测区井序列）
  void setWellName(const QString &name, bool reference = false);
  QString wellName() const { return m_wellName; }
  bool isReferenceWell() const { return m_referenceWell; }

  // D1.8 会话键的项目段（未设置时 "default"）
  void setProjectName(const QString &project);
  QString projectName() const { return m_projectName; }

  // 获取当前装配的综合数据
  ComprehensiveWellData currentData() const { return m_data; }

  // 打开曲线道组合/解散管理对话框
  void openCurveConfigDialog();

  // ---- D2.x 深度交互面 ----
  QList<DepthBookmark> bookmarks() const { return m_bookmarks; }
  void setBookmarks(const QList<DepthBookmark> &bms);          // 测试/模板恢复入口
  bool addBookmark(const QString &name, double depth);         // D2.6
  bool jumpToBookmark(const QString &name);                    // 点击跳转
  void setDepthUnitFeet(bool feet);                            // D6.3
  bool depthUnitFeet() const { return m_depthFeet; }
  void setGapThresholdMeters(double meters);                   // D2.12
  double gapThresholdMeters() const { return m_gapThresholdM; }

  // D2.3 标注钉（含持久化）
  QList<DepthPin> pins() const { return m_canvas ? m_canvas->pins() : QList<DepthPin>(); }
  void addPinAt(double depth, const QString &text);
  void clearPins();

  // 源数据路径（sidecar 挂靠；缺省空 = 无 sidecar 持久化）
  QString sourceDataPath() const { return m_sourceDataPath; }
  void setSourceDataPath(const QString &path);

  // ---- D3.x 编辑模式面（D3.14 工具条随 editMode 显隐） ----
  void setEditMode(bool on);
  bool editMode() const;
  EditSession *editSession() const { return m_editSession.get(); }

  // D3.9/D3.3 保存编辑：产派生文档意图信号 + sidecar 审计追加 + 标记已保存。
  // 返回 false = 无会话/无脏编辑。
  bool saveDerived();

  // D7.4 高对比模式
  void setHighContrast(bool on);
  bool highContrast() const { return m_highContrast; }

  // D1.10 隐藏道管理条（测试可达）
  HiddenTrackBar *hiddenBar() const { return m_hiddenBar; }

  // D2.11 读数条标签（测试可达）
  QLabel *readoutLabel() const { return m_lblReadout; }

signals:
  void wellLoaded(const QString &wellName);
  // D3.3 派生版本意图：编辑落盘由壳/测试接（视图不写工程目录）
  void derivedDocumentReady(const WellComposite::ComprehensiveWellData &doc, const QString &auditSummary);

public slots:
  // D2.7 Ctrl+G
  void openGotoDepthDialog();

private slots:
  void exportCurrent(ExportEngine::Format format);      // D4.5/D4.6/D4.7
  void printCurrent();                                  // D4.8/D4.9
  void manageExportPresets();                           // D4.10
  void onIntervalSelected(double top, double bottom);   // D2.2
  void onPinCreateRequested(double depth);              // D2.3
  void onPinEditRequested(int pinIndex);                // D2.3
  void onTrackCsvRequested(int trackIndex);             // D1.5
  void onTrackConfigRequested(int trackIndex);          // D1.6
  void onTrackDuplicateRequested(int trackIndex);       // D1.5
  void onTrackVisibilityChanged(int trackIndex);        // D1.10 + D1.8 记忆
  void onTrackOrderOrWidthChanged();                    // D1.8 记忆
  void onBookmarkMenuAboutToShow();                     // D2.6
  void onMarkerMoved(const QString &name, double newDepth); // D3.1 接编辑栈

private:
  void setupUi();
  void setupTracksFromData(const ComprehensiveWellData &data);
  void rebuildLegendData();
  void syncSessionToTracks(); // 编辑会话数据 → 画布道重同步

  // D1.8 会话记忆读写（按井+项目）
  void saveSessionState() const;
  void restoreSessionState();
  QList<TrackSpec> currentSpecs() const;
  void applySpecsIncrementally(const QList<TrackSpec> &specs);

  // sidecar
  void loadSidecar();
  void saveSidecar() const;

  // D3.6 指派应用（编辑会话内；分层名未识别时由用户显式指派）
  void applyStratAssignments(QVector<FormationInterval> *formations) const;

  QString m_wellName;
  bool m_referenceWell = false;
  QString m_projectName = QStringLiteral("default");
  QString m_sourceDataPath;
  ComprehensiveWellData m_data;

  bool m_depthFeet = false;   // D6.3
  double m_gapThresholdM = 0.0; // D2.12
  bool m_highContrast = false; // D7.4

  QList<DepthBookmark> m_bookmarks;
  std::unique_ptr<WellCompositeStore> m_store;

  // D3.x 编辑会话（undo 栈/脏状态/审计；Phase 3 交付物）
  std::unique_ptr<EditSession> m_editSession;

  QLabel *m_lblWellName = nullptr;
  QComboBox *m_scaleCombo = nullptr;
  QToolButton *m_btnZoomOut = nullptr;
  QLabel *m_lblZoom = nullptr;
  QToolButton *m_btnZoomIn = nullptr;
  QToolButton *m_btnResetZoom = nullptr;
  QToolButton *m_btnConfigCurves = nullptr;
  QToolButton *m_btnBookmarks = nullptr;
  QToolButton *m_btnGoto = nullptr;
  QToolButton *m_btnSnap = nullptr;
  QToolButton *m_btnExport = nullptr;   // D4.5–D4.10 导出菜单入口
  QToolButton *m_btnEdit = nullptr;     // D3.1/D3.14 TOPs 编辑模式
  QLabel *m_lblStatus = nullptr;
  QLabel *m_lblReadout = nullptr;       // D2.11

  WellCompositeCanvas *m_canvas = nullptr;
  WellPositionLegendWidget *m_legendWidget = nullptr;
  HiddenTrackBar *m_hiddenBar = nullptr;
};

} // namespace WellComposite
