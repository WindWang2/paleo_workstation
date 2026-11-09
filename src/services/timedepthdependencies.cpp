// 层：数据
#include "timedepthdependencies.h"

#include "catalog/datacatalog.h"
#include "documentrelationresolver.h"
#include "io/areamodel.h"

#include <paleo_data/object_descriptor.h>
#include <paleo_data/service.h>
#include <utility>

namespace paleo::document {
namespace {
constexpr auto kDependencies = "timeDepthDependencies";
enum class Currentness { Current, Changed, Unavailable };

bool fail(QString *error, const QString &detail)
{
    if (error)
        *error = detail;
    return false;
}

class DependencyCheck
{
public:
    explicit DependencyCheck(const DataCatalog &catalog) : m_catalog(catalog) {}

    Currentness check(const TimeDepthDependency &dependency, QString *error)
    {
        const auto &use = dependency.use;
        const auto &object = dependency.object;
        const auto well = m_catalog.storedEntityById(use.wellEntityId);
        if (well.entityType != QLatin1String("well") ||
            well.extra.value("areaModelWellId").toLongLong() != use.modelWellId ||
            !selected(dependency))
            return Currentness::Changed;

        if (!m_open)
        {
            std::string detail;
            if (!m_sdk.openProjectReadOnly(area_model::directory(m_catalog.projectDir()).toUtf8().toStdString(), detail))
            {
                fail(error, QString::fromStdString(detail));
                return Currentness::Unavailable;
            }
            m_open = true;
        }
        const auto state = checkObject(object, use.modelWellId, error);
        if (state != Currentness::Current || !use.context.targetVolume)
            return state;
        return checkObject(*use.context.targetVolume, 0, error);
    }

private:
    bool selected(const TimeDepthDependency &dependency)
    {
        // Binding changes and locator-only revisions can exist on different
        // versions of this same asset. Match the highest binding revision.
        if (!m_bindings.contains(dependency.assetId))
        {
            QHash<QString, SourceBinding> bindings;
            for (const auto &version : m_catalog.versionsForAsset(dependency.assetId))
            {
                const auto payload = version.extra.value("documentSourceBindings").toMap();
                for (auto it = payload.cbegin(); it != payload.cend(); ++it)
                {
                    SourceBinding binding;
                    if (!sourceBindingFromJson(QJsonObject::fromVariantMap(it.value().toMap()), binding) ||
                        binding.id != it.key() || !hasRelationBinding(binding))
                        continue;
                    const auto previous = bindings.constFind(binding.id);
                    if (previous == bindings.cend() || binding.revision >= previous->revision)
                        bindings.insert(binding.id, binding);
                }
            }
            QHash<QString, QList<SourceBinding>> primarySlots;
            for (const auto &binding : std::as_const(bindings))
                if (binding.primary && binding.decision == BindingDecision::Confirmed)
                    primarySlots[binding.slotKey()].append(binding);
            m_bindings.insert(dependency.assetId, std::move(primarySlots));
        }

        const auto choices = m_bindings[dependency.assetId].value(dependency.slotKey());
        RelationUseSpec use;
        ModelObjectRef object;
        return choices.size() == 1 && relationBinding(choices.front(), use, object) &&
            use == dependency.use && object == dependency.object;
    }

    Currentness checkObject(const ModelObjectRef &object, qlonglong well, QString *error)
    {
        std::string detail;
        const auto sourceId = object.source.sourceId;
        if (!m_sources.contains(sourceId))
        {
            data::SourceIndexEntry source;
            if (!m_sdk.readSourceIndexEntry(sourceId, source, detail))
            {
                if (detail == "来源记录不存在")
                    return Currentness::Changed;
                fail(error, QString::fromStdString(detail));
                return Currentness::Unavailable;
            }
            m_sources.insert(sourceId, source);
        }
        const auto &source = m_sources[sourceId];
        if (!source.active || QString::fromStdString(source.sha256) != object.source.fileRevisionId)
            return Currentness::Changed;

        const bool volume = object.kind == ObjectKind::SeismicVolume;
        auto &objects = volume ? m_volumes : m_tables;
        if (!objects.contains(object.id))
        {
            data::DataObjectDescriptor descriptor;
            const auto kind = volume ? data::DataObjectKind::SeismicVolume : data::DataObjectKind::TimeDepth;
            if (!m_sdk.describeDataObject(kind, object.id, descriptor, detail))
            {
                if (detail == "指定对象不存在或类别不匹配")
                    return Currentness::Changed;
                fail(error, QString::fromStdString(detail));
                return Currentness::Unavailable;
            }
            objects.insert(object.id, descriptor);
        }
        const auto &descriptor = objects[object.id];
        if (!descriptor.revisionKnown)
        {
            fail(error, QStringLiteral("模型缺少时深依赖修订元数据"));
            return Currentness::Unavailable;
        }
        return descriptor.sourceId == sourceId && descriptor.wellId == well &&
               QString::fromStdString(descriptor.revision) == object.revision
            ? Currentness::Current : Currentness::Changed;
    }

    const DataCatalog &m_catalog;
    data::DataService m_sdk;
    bool m_open = false;
    QHash<QString, QHash<QString, QList<SourceBinding>>> m_bindings;
    QHash<qlonglong, data::SourceIndexEntry> m_sources;
    QHash<qlonglong, data::DataObjectDescriptor> m_tables, m_volumes;
};

bool refresh(DataCatalog &catalog, const QSet<QString> &consumers, QString *error)
{
    if (error)
        error->clear();
    if (consumers.isEmpty())
        return true;

    DependencyCheck check(catalog);
    QSet<QString> stale;
    for (const auto &id : consumers)
    {
        const auto version = catalog.versionById(id);
        if (version.extra.value("stale").toBool())
            continue;
        const auto receipts = version.extra.value(kDependencies).toList();
        for (const auto &receipt : receipts)
        {
            const auto dependency = TimeDepthDependency::fromVariant(receipt);
            if (!dependency)
                return fail(error, QStringLiteral("成果 %1 的时深依赖格式无效").arg(id));
            switch (check.check(*dependency, error))
            {
            case Currentness::Current: break;
            case Currentness::Changed: stale.insert(id); break;
            case Currentness::Unavailable: return false;
            }
            if (stale.contains(id))
                break;
        }
    }
    return catalog.markDerivedInputsStale(stale, QStringLiteral("时深关系或目标时间基准已变化"), error);
}
}

QVariantMap timeDepthDependency(const RelationResolveRequest &request)
{
    const TimeDepthDependency dependency{request.use, request.object, request.selected.assetId};
    if (request.selectedBindingId.isEmpty() || request.selected.ref != request.object.source || !dependency.valid())
        return {};
    return dependency.toVariantMap();
}

bool validateTimeDepthDependencies(const DataCatalog &catalog, const QVariantList &receipts, QString *error)
{
    if (error)
        error->clear();
    DependencyCheck check(catalog);
    for (const auto &receipt : receipts)
    {
        const auto dependency = TimeDepthDependency::fromVariant(receipt);
        if (!dependency)
            return fail(error, QStringLiteral("成果时深依赖格式无效"));
        switch (check.check(*dependency, error))
        {
        case Currentness::Current: break;
        case Currentness::Changed: return fail(error, QStringLiteral("时深关系选择或对象修订已变化，请重新计算"));
        case Currentness::Unavailable: return false;
        }
    }
    return true;
}

bool invalidateTimeDepthSelection(DataCatalog &catalog, const RelationUseSpec &use,
                                  const ModelObjectRef *selected, QString *error)
{
    QSet<QString> stale;
    const auto slot = timeDepthSelectionKey(use);
    for (const auto &id : catalog.timeDepthConsumersForSlot(slot))
    {
        const auto version = catalog.versionById(id);
        for (const auto &receipt : version.extra.value(kDependencies).toList())
        {
            const auto dependency = TimeDepthDependency::fromVariant(receipt);
            if (dependency && dependency->slotKey() == slot &&
                (!selected || dependency->use != use || dependency->object != *selected))
            {
                stale.insert(id);
                break;
            }
        }
    }
    return catalog.markDerivedInputsStale(stale, QStringLiteral("所用时深关系选择已变化"), error);
}

bool refreshTimeDepthDependencies(DataCatalog &catalog, QString *error)
{
    return refresh(catalog, catalog.timeDepthConsumers(), error);
}

bool refreshTimeDepthObjectDependencies(DataCatalog &catalog, qlonglong tableId, QString *error)
{
    return refresh(catalog, catalog.timeDepthConsumersForTable(tableId), error);
}

bool refreshTimeDepthDependencies(DataCatalog &catalog, const QSet<qlonglong> &sources, qlonglong volume,
                                  QString *error)
{
    return refresh(catalog, catalog.timeDepthConsumersForObjects(sources, volume), error);
}
}
