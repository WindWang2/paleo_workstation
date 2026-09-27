#pragma once
#include <QObject>
#include <QString>
#include <QStringList>

class QgisLayerService;
class QgisProjectService;
class QgsLayoutItemMap;

// qgis/qgislayerprofile.h — 页面图层档案服务（m1-layer-platform 接缝的消费面）。
// 任务契约：predict/constraint/compose/validate 四页 pageId 固定；本文件是
// m1 未合并期间的本地最小实现——页面档案表 + QgsMapThemeCollection 封装。
// m1 合流时若交付同名类，以 m1 版本为准做 merge（保持本 API 兼容）。
// 层：QGIS 封装
class QgisLayerProfileService : public QObject
{
  Q_OBJECT
  public:
    QgisLayerProfileService(QgisLayerService *layers, QgisProjectService *projectSvc,
                            QObject *parent = nullptr);

    // 页面档案表：pageId → 该页可见的 manifest 组词（01_Base…07_Validation）。
    // 未知 pageId → 空表（调用方不应用）。
    static QStringList profileGroups(const QString &pageId);
    // 固定词表（顺序 = 页序）。
    static const QStringList &knownPageIds();

    // 主题名（QgsMapThemeCollection 键）："paleo.page.<pageId>"。
    static QString themeNameForPage(const QString &pageId);

    // 应用页面档案：按组收集声明图层（激活层位非空时取该层位 + 层位无关
    // 图层），逐个 instantiate，把结果写入 QgsMapThemeCollection 主题
    // "paleo.page.<pageId>"，再对齐图层树勾选态（主题内打勾、组外取消）。
    // 返回主题内 layerId 集（失败/空档案 → 空表）。
    QStringList applyPageProfile(const QString &pageId);

    // m1 setLayoutMapTheme 接缝：版面地图项钉页面主题（follow visibility
    // preset）。m1 缺席期间直写 QgsLayoutItemMap::setFollowVisibilityPresetName。
    bool setLayoutMapTheme(QgsLayoutItemMap *map, const QString &pageId);

  signals:
    // 应用完成（含空档案应用）；visibleLayerIds 为本次主题内图层集。
    void profileApplied(const QString &pageId, const QStringList &visibleLayerIds);

  private:
    QgisLayerService *m_layers;
    QgisProjectService *m_projectSvc;
};
