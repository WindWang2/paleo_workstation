// 层：QGIS 封装
#include "qgislayerprofile.h"

#include <qgslayertreemodel.h>
#include <qgsmapthemecollection.h>
#include <qgsproject.h>

QgisLayerProfileService::QgisLayerProfileService(QgsProject *project, QObject *parent)
    : QObject(parent), m_project(project)
{
}

QgisLayerProfileService::~QgisLayerProfileService() = default;

void QgisLayerProfileService::setLayerTreeModel(QgsLayerTreeModel *model) { m_model = model; }

void QgisLayerProfileService::setLayerService(QgisLayerService *service) { m_layerService = service; }

QStringList QgisLayerProfileService::defaultProfileGroups(const QString &pageId)
{
    if (pageId == QStringLiteral("predict"))
        return {QStringLiteral("01_Base"), QStringLiteral("02_Prediction")};
    if (pageId == QStringLiteral("constraint"))
        return {QStringLiteral("01_Base"), QStringLiteral("03_Constraints"),
                QStringLiteral("04_SingleFactor")};
    if (pageId == QStringLiteral("compose"))
        return {QStringLiteral("01_Base"), QStringLiteral("03_Constraints"),
                QStringLiteral("04_SingleFactor"), QStringLiteral("05_PaleoMap"),
                QStringLiteral("06_Reference")};
    if (pageId == QStringLiteral("validate"))
        return {QStringLiteral("01_Base"), QStringLiteral("07_Validation")};
    return {}; // data（或未知页）：不操作画布
}

void QgisLayerProfileService::setProfileGroupsOverride(const QString &pageId,
                                                       const QStringList &groups)
{
    m_groupOverrides.insert(pageId, groups);
}

QStringList QgisLayerProfileService::profileGroupsFor(const QString &pageId) const
{
    const auto it = m_groupOverrides.constFind(pageId);
    if (it != m_groupOverrides.constEnd())
        return it.value();
    return defaultProfileGroups(pageId);
}

QString QgisLayerProfileService::pageThemeName(const QString &pageId)
{
    return QStringLiteral("page:") + pageId;
}

// ---- 以下为骨架占位实现（wave/layer-platform 子任务 C 落地）----

bool QgisLayerProfileService::applyPageProfile(const QString &pageId)
{
    Q_UNUSED(pageId);
    return false;
}

bool QgisLayerProfileService::applyCurrentPageProfile() { return false; }

bool QgisLayerProfileService::captureCurrentAsTheme(const QString &name)
{
    Q_UNUSED(name);
    return false;
}

bool QgisLayerProfileService::applyTheme(const QString &name)
{
    Q_UNUSED(name);
    return false;
}

bool QgisLayerProfileService::removeMapTheme(const QString &name)
{
    Q_UNUSED(name);
    return false;
}

bool QgisLayerProfileService::hasTheme(const QString &name) const
{
    Q_UNUSED(name);
    return false;
}

QStringList QgisLayerProfileService::themes() const { return {}; }

void QgisLayerProfileService::setLayoutMapTheme(QgsLayoutItemMap *mapItem,
                                                const QString &themeName)
{
    Q_UNUSED(mapItem);
    Q_UNUSED(themeName);
}
