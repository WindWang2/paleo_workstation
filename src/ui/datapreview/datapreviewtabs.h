// 层：视图
#pragma once
#include <QHash>
#include <QPointer>
#include <QString>
#include <QWidget>
#include <functional>
#include <memory>

#include "../../services/previewdoc.h"   // 数据页预览的唯一数据门面（W1）

class DataImportService;
class PaleoTaskService;
class DataCatalog;
class QLabel;
class QTabWidget;
struct CatalogAsset;
class QgsProject;
class QgsMapCanvas;
class PreviewMapPage;

// ui/datapreview — 数据页页内预览标签栏（docs/PROJECT_AREA_PLAN.md §4）。
// 普通 QTabWidget（可关闭标签），样式走 DESIGN.md dock 面板，不用工作流
// 标签的蓝色下划线。预览只出现在数据管理页，不泄漏到其他页。
//
// 状态文案（§4）：
//   空态   「还没有打开的预览 — 在列表中选择一条数据」
//   读取中 「正在读取」+文件名（同步读取前置状态，不做线程）
//   失败   「读取失败」+原因+文件名+「重试」；外链缺失 「找不到源文件」+路径
// 标题：「文件名 · 井名」/「文件名 · 测线」；无过滤时只有文件名。
// 多井资产（井口表、DC.dat、TD）每标签自带「井」下拉框，只列已决链接的井；
// 未选井时正文是「先选择一口井」。重选已开资产聚焦已有标签，不改它已选的井。
class DataPreviewTabs : public QWidget
{
  Q_OBJECT
  public:
    explicit DataPreviewTabs(QWidget *parent = nullptr);
    ~DataPreviewTabs() override;

    // T27 中文化：coordinate_status 枚举显示串（ok/untransformed/invalid/
    // missing → 中文，§4 文案）。未知/空按「没有坐标」。
    static QString coordinateStatusText(const QString &status);

    // T29 双向同步：图层可见性 → 「在地图上显示」按钮态。shell 在图层
    // visibilityChanged / 显示成功时调用；on=true 按钮写「已在地图上」。
    void setHorizonOnMap(const QString &layerId, bool on);

    // 服务绑定（mainwindow 接线处调用；为空时 openAsset 显示空态）。
    // setImportService 自建门面（测试/小环境）；setDocService 挂壳共享的
    // 门面实例（壳持有一个 PreviewDocService，页属性与本组件共用同一份
    // 读者缓存/SHA 已验集）。
    void setImportService(DataImportService *svc);
    void setDocService(PreviewDocService *doc);
    void setProject(QgsProject *project);
    void setDetailsHost(QWidget *host);

    // 打开测区全景地图画布（可以用 QGIS 画布）
    void openSurveyArea();

    // 任务服务（wave2 D1）：接上后地震标签的「索引+SHA 复验+测线解码」进
    // 任务池异步执行（任务页有进度条/ETA/取消）；不接线保持同步旧行为
    //（小夹具测试环境用）。异步模式下索引按 assetId 缓存——换测线只重解码
    // 该线，不再整文件重建（T23）。
    void setTaskService(PaleoTaskService *svc);

    // 从资产列表选中一条资产：已有标签则聚焦，否则新开一个可关闭标签。
    void openAsset(const QString &assetId);
    // 打开指定资产并选定特定井（如 DC.dat 多井分层表或井口表预选该井）
    void openAssetForWell(const QString &assetId, const QString &wellId);

    // 验证页「在数据页看这条剖面」（预览壳重排 §4）：打开/聚焦地震资产
    // 标签并把测线控件（lineMode/lineSpin）拨到 kind+line——控件自己的
    // decode 链路负责换测线，不在此处解析。kind: "inline"|"crossline"。
    // timeMs 当前只透传记录——剖面已有的 D61 标定线就是同一时间轴标注，
    // 不在剖面上另画第二条线。非地震资产/无测线控件时静默返回。
    void openSeismicLine(const QString &assetId, const QString &kind,
                        int line, double timeMs);

    int tabCount() const;
    QString assetIdAt(int index) const;   // "" 越界
    void closeAssetTab(const QString &assetId);
    bool isMissingSourceState(const QString &assetId) const; // 外链缺失态（测试/诊断）

    // wave4：外链「重新定位文件…」按钮的动作面（对话框只产出 pickedPath，
    // 这里是可测的执行半边）。成功 → 该标签重建、真预览加载；失败 → 错误
    // 如实写到死胡同面上（保留「找不到源文件」前缀与按钮，可换文件再试）。
    bool relocateMissingSourceWith(const QString &assetId, const QString &versionId,
                                   const QString &pickedPath);

  signals:
    // well_head 标签被选中/聚焦时，地图高亮该井（§4）。
    void wellSelected(const QString &wellEntityId);
    // horizon 标签「在地图上显示」按钮（§4）。
    void showHorizonOnMapRequested(const QString &layerId);
    // D7：角落「最大化预览/还原预览」切换——分栏尺寸归 shell（PaleoMainWindow
    // 持有 splitter），这里只报意图。
    void previewMaximizeToggled(bool maximized);
    // D11 临时配准：GeoJSON 标签「临时配准（手工仿射）…」确认后发射。
    // params 携带 tx/ty/sx/sy/rotDeg；DERIVED 登记、图层实例化与水印由壳办。
    void provisionalRegistrationRequested(const QString &assetId,
                                          const QVariantMap &params);
    // 请求切换到主工作区全屏 QGIS 画布查看
    void requestShowOnMainCanvas();

  private:
    void syncDetails();
    void clearDetails(const QString &assetId);
    QPointer<QWidget> m_detailsHost;
    QHash<QString, QPointer<QWidget>> m_detailsOfAsset;
    QWidget *buildContent(const QString &assetId, QWidget *page);
    QWidget *buildSurveyAreaContent(QWidget *page);
    void rebuildAssetTab(const QString &assetId); // 「重试」/PDF 转换完成后重建内容
    void focusWellIfNeeded(const QString &assetId, QWidget *page);
    void updateTabTitle(const QString &assetId); // 文件名 + 过滤后缀（井/测线）
    // 井过滤型正文（well_head / well_stratification / time_depth）：
    // wellName 为空 → 「先选择一口井」占位；否则按该井过滤渲染。
    QWidget *buildWellBody(const CatalogAsset &asset, const QString &absPath,
                           const QString &wellEntityId, const QString &wellName,
                           QWidget *parent);
    // 「读取失败」+原因+文件名+「重试」（重试=重建该标签）。
    QWidget *failureState(const QString &assetId, const QString &reason, QWidget *parent);
    // 「正在读取」+文件名 标签（同步读取前置；buildContent 完成后隐藏）。
    QLabel *loadingLabel(const QString &fileName, QWidget *parent);
    // 测线解码结果应用（services/previewdoc 信号 → 当前挂起的控件组；
    // 陈旧结果已在服务内丢弃，这里只管最新一代）。
    void onSectionReady(const QString &assetId,
                        const PreviewDocService::SectionDoc &doc);
    void onSectionFailed(const QString &assetId, const QString &reason);
    // F1：LAS 数据行到达/失败/取消（requestLas；key=assetId）。
    void onLasReady(const QString &key, const QStringList &names, const QList<LasCurve> &curves);
    void onLasFailed(const QString &key, const QString &reason);

    // D2.10 同目录组图：把另一资产的地图层叠进本预览页（geojson/带配准
    // 图片/层位栅格；不支持的类型如实跳过）。
    void addSiblingOverlayLayer(PreviewMapPage *page, const QString &sibAssetId,
                                const QString &sibName, QWidget *owner);

    // 挂接门面（私有）：setImportService/setDocService 共用入口。
    void attachDoc(PreviewDocService *doc);

  // 视图侧持有的数据门面：一切解析/解码/SHA/PDF 编排都经它（W1 下沉）。
  PreviewDocService *m_doc = nullptr;             // 挂接的门面（非持有）
  std::unique_ptr<PreviewDocService> m_docOwned;  // setImportService 自建时持有
  PaleoTaskService *m_taskSvc = nullptr; // 仅记忆接线顺序，真用走 m_doc
  QPointer<QgsProject> m_project;
  // 「过时」徽标刷新接线（B 包 staleness-lite）：当前服务 catalog 的
  // changed() → 重算已开标签标题。换绑服务时先断开（见 setImportService）。
  DataCatalog *m_catalogForTitles = nullptr;
  QTabWidget *m_tabs = nullptr;
    QLabel *m_emptyLabel = nullptr;
    QHash<QString, QWidget *> m_pageOfAsset;
    QHash<QString, QString> m_wellEntityOfAsset; // assetId → 该标签已选井（多井下拉框）
    QHash<QString, QString> m_titleSuffixOfAsset; // assetId → 「 · 井名」/「 · IL1315」
    // P2 D2.9：assetId → 预览选中的版本 id（RAW/DERIVED 切换；空 = 缺省最新）。
    QHash<QString, QString> m_chosenVersionOfAsset;
    // 解码进行中挂起的控件组（服务发射结果时按 assetId 找回该把图像贴哪）。
    struct SectionPending
    {
      QPointer<QWidget> panel;      // SectionPanel（cpp 内类，按 QWidget 存）
      QPointer<QWidget> mode;       // 测线模式下拉框（解完恢复可用）
      QPointer<QWidget> spin;       // 测线号输入框（同上）
      QPointer<QWidget> tieCaption; // 标定线说明标签（setTieMarker 取其文案）
      QString tieText;
      double tieMs = 0.0;
      bool hasTie = false;
    };
    QHash<QString, SectionPending> m_pendingSection;

    // F1（goal/perf-systematize 簇2）：well_log 两段式——页骨架（曲线名/
    // 控件/分层）由 lasHeaderAt 秒铺（代价只与头部行数成正比），整份数据
    // 行 requestLas 池内解析，lasReady 到达后经 fill 回调补曲线数据与单位
    //（单道检视 + ResFormStar 综合柱状图两个消费方一次装齐）。fill 闭包内
    // 持 QPointer 护栏随页生死；页关闭/重建即作废，服务侧世代号保证到达的
    // 是最新代。无任务服务时 requestLas 同步执行、返回前信号已发——小夹具
    // 测试环境与旧同步路径行为一致。
    struct LasPending
    {
      QPointer<QWidget> page; // 页根（失败换装用）
      std::function<void(const QList<LasCurve> &)> fill;
    };
    QHash<QString, LasPending> m_pendingLas;

    // 瓦片渐进时间片（wave/seismic-engine-deep 主线2）：服务信号是广播的，
    // 这里只记最新一次瓦片请求的目标画布 + 世代（采样号）——旧请求/其他
    // 资产标签的瓦片不进当前画布。m_tiledSignalService 记已接线的服务，
    // 门面重挂（换服务实例）时自动重接。
    QPointer<QObject> m_tiledSignalService;
    QPointer<QWidget> m_tiledCanvas; // SeismicSectionCanvas（cpp 内 qobject_cast）
    int m_tiledSample = -1;
};
