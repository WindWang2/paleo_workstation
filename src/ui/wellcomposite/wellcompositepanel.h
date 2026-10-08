// 层：视图
#pragma once

#include <QComboBox>
#include <QLabel>
#include <QToolButton>
#include <QWidget>
#include <QPointer>

#include <limits>
#include <memory>
#include <functional>
#include "domain/welllogfacies.h"

#include "wellcompositecanvas.h"
#include "domain/wellcompositemodel.h"
#include "wellcompositestore.h"
#include "depthtools.h"
#include "depthtransform.h"
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

class PaleoTaskService; // F2：两段式 XML 任务池（全局作用域——勿入 WellComposite）
class WellFaciesWorkflow;

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
  // 组装根注入功能层；面板只发意图、接收可用性与绘图数据。
  using FaciesWorkflowFactory = std::function<WellFaciesWorkflow *(QObject *)>;
  static void setFaciesWorkflowFactory(FaciesWorkflowFactory factory);
  void bindFaciesWorkflow(WellFaciesWorkflow *workflow);
  void showFaciesPrediction(const WellFaciesResult &result);
  void showWellAttributes(const ComprehensiveWellData &data);

  WellCompositeCanvas *canvas() const { return m_canvas; }
  WellPositionLegendWidget *legendWidget() const { return m_legendWidget; }

  // 加载并装配中国石油标准综合柱状图 XML (SpreadsheetML)
  bool loadComprehensiveXml(const QString &xmlPath);
  // 已解析的完整井数据共用入口；测区井可携带曲线、段与岩性，参考井同样适用。
  bool loadWellData(const ComprehensiveWellData &data, const QString &sourcePath = {}, bool reference = false);
  // F2（goal/perf-systematize 簇2）：两段式——XML 在任务池解析（综合图可含
  // 数 MB 曲线数据，同步解析阻塞 UI 线程），结果 GUI 线程装配并发射
  // comprehensiveXmlLoaded(ok)；无任务服务时同步执行、返回前信号已发
  //（测试/小环境行为与旧路径一致）。文件不存在同步返回 false（快速失败）。
  bool loadComprehensiveXmlAsync(const QString &xmlPath, PaleoTaskService *svc);

  // 加载并装配单井 LAS 曲线（支持关联地层分层道与 1-4 根曲线分道合并显示）
  bool loadLasCurves(const QString &wellName, const QVector<CurveData> &curves,
                     const QVector<FormationInterval> &formations = {});
  // 图片道（catalog core/lab_analysis 井附件照片，depthMd 锚）——LAS 装配后
  // 由壳喂入：留档 m_data.images、深度范围扩到盖住锚位、追加一条 ImageTrack
  //（默认标题「岩心照片」，可删/复制走既有 trackops）。空集 = 空操作。
  void setCoreImages(const QVector<ImageDepthItem> &items);

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

  // ---- D1 深度装配链（wave/deepen-perf）：壳/derivedsink 经 io 解析后喂 ----
  // 井斜表（MD/井斜/方位）→ DepthTransform TVD；时深对（TVD m, TWT ms）→
  // TWT 副刻度 + 读数条。任一为空 = 该能力禁用（不猜表）。
  void applyDepthTables(const QVector<DeviationStation> &stations,
                        const QVector<QPair<double, double>> &tvdTwtPairs,
                        double kbElevation = std::numeric_limits<double>::quiet_NaN());
  void applyTimeDepthAlignment(const std::optional<seismic::TimeDepthModel> &model,
                              double shiftMs, const QString &status);
  bool hasDeviationSurvey() const { return m_depthTransform.hasDeviationSurvey(); }
  bool hasTimeDepthTable() const { return m_depthTransform.hasTimeDepthTable(); }
  double mdToTvd(double md) const { return m_depthTransform.mdToTvd(md); }
  double twtAtDepth(double md) const
  {
    return m_depthTransform.twtAtMd(md);
  }
  void clearDepthTables(); // 换井/换源时复位
  // D1：读数条尾缀（TVD/TWT；无表回空）——测试与壳侧状态显示共用
  QString depthReadoutSuffix(double md) const;

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
  void faciesDataChanged(const WellComposite::ComprehensiveWellData &data);
  void faciesPredictionRequested();
  void faciesCancelRequested();
  void faciesModelsRequested();
  void faciesModelSelected(const QString &id);
  void faciesConfigurationRequested(const QString &url, const QString &key,
                                    bool allowInsecureHttp);
  void wellLoaded(const QString &wellName);
  // F2（goal/perf-systematize 簇2）：两段式 XML 装配终态（含同步路径；
  // ok=false = 解析失败——页面侧据此换装失败面）。
  // D3.3 派生版本意图：编辑落盘由壳/测试接（视图不写工程目录）。
  // D1（wave/deepen-perf）：追加 auditLines——壳侧 DERIVED 登记把逐条审计
  // 写进派生 XML「编辑审计」工作表（摘要字符串只够展示，不够落档）。
  void derivedDocumentReady(const WellComposite::ComprehensiveWellData &doc,
                            const QString &auditSummary,
                            const QStringList &auditLines);
  void comprehensiveXmlLoaded(bool ok); // F2：两段式 XML 装配终态

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
  void clearFaciesPrediction();
  void setupUi();
  void setupTracksFromData(const ComprehensiveWellData &data);
  // F2：已解析数据的 GUI 线程装配（同步/异步路径共用；不碰文件）。
  void applyComprehensiveData(const ComprehensiveWellData &data, const QString &xmlPath, bool reference = true);
  void rebuildLegendData();
  void syncSessionToTracks(); // 编辑会话数据 → 画布道重同步
  void refreshTwtLabels();    // D1：时深表 → 深度标尺道 TWT 副刻度

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
  DepthTransform m_depthTransform; // D1：井斜/时深装配（壳喂）

  QList<DepthBookmark> m_bookmarks;
  std::unique_ptr<WellCompositeStore> m_store;

  // D3.x 编辑会话（undo 栈/脏状态/审计；Phase 3 交付物）
  std::unique_ptr<EditSession> m_editSession;
  int m_xmlLoadSeq = 0; // F2：两段式 XML 世代号（换源/重入后旧结果丢弃）

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
  QToolButton *m_btnSaveDerived = nullptr; // D1：保存派生版本（→壳 DERIVED 登记）
  QLabel *m_lblStatus = nullptr;
  QLabel *m_lblReadout = nullptr;       // D2.11
  QToolButton *m_btnPredictFacies = nullptr, *m_btnCancelFacies = nullptr;
  QToolButton *m_btnFaciesService = nullptr, *m_btnRefreshFacies = nullptr, *m_btnShowFacies = nullptr;
  QComboBox *m_faciesModel = nullptr;
  QLabel *m_faciesStatus = nullptr;
  QPointer<WellFaciesWorkflow> m_faciesWorkflow;
  std::shared_ptr<WellTrack> m_predictionTrack, m_confidenceTrack;

  WellCompositeCanvas *m_canvas = nullptr;
  WellPositionLegendWidget *m_legendWidget = nullptr;
  HiddenTrackBar *m_hiddenBar = nullptr;
};

} // namespace WellComposite
