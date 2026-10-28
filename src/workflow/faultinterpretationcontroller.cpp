// 层：功能
#include "faultinterpretationcontroller.h"

#include "../catalog/datacatalog.h"
#include "../linkage/selectioncontext.h"
#include "../metadata/faultsetstore.h"

#include <QPointer>

#include <functional>

#include <qgsfeature.h>
#include <qgsgeometry.h>
#include <qgsproject.h>
#include <qgsvectorlayer.h>

namespace paleo::fault {

namespace {
// 撤销自动新建断层时的收尾：该断层若已无任何内容则一并移除（否则保留
// 其他棒/切割）。
void dropFaultIfEmpty(FaultSet &set, const QString &faultId)
{
    const Fault *f = set.faultById(faultId);
    if (f && f->sticks.isEmpty() && f->cuts.isEmpty())
        set.removeFault(faultId);
}
} // namespace

// ---- FaultEditStack ----

FaultEditStack::FaultEditStack(QObject *parent)
    : QObject(parent)
{
}

bool FaultEditStack::push(const QString &text, std::function<bool()> undoFn,
                          std::function<bool()> redoFn)
{
    truncateRedo();
    Entry entry{text, std::move(undoFn), std::move(redoFn)};
    if (!entry.redoFn()) // push 即执行；失败不入账（#480 P3-4 机制化约定）
        return false;
    m_done.append(std::move(entry));
    emit canUndoChanged(canUndo());
    emit canRedoChanged(canRedo());
    return true;
}

bool FaultEditStack::undo()
{
    if (m_done.isEmpty())
        return false;
    Entry entry = m_done.takeLast();
    if (!entry.undoFn()) { // 重放失败：条目回 done 栈，历史不失序
        m_done.append(std::move(entry));
        return false;
    }
    mUndone.append(std::move(entry));
    emit canUndoChanged(canUndo());
    emit canRedoChanged(canRedo());
    return true;
}

bool FaultEditStack::redo()
{
    if (mUndone.isEmpty())
        return false;
    Entry entry = mUndone.takeLast();
    if (!entry.redoFn()) { // 重放失败：条目回 undone 栈，历史不失序
        mUndone.append(std::move(entry));
        return false;
    }
    m_done.append(std::move(entry));
    emit canUndoChanged(canUndo());
    emit canRedoChanged(canRedo());
    return true;
}

void FaultEditStack::clear()
{
    const bool hadUndo = !m_done.isEmpty();
    const bool hadRedo = !mUndone.isEmpty();
    m_done.clear();
    mUndone.clear();
    if (hadUndo)
        emit canUndoChanged(false);
    if (hadRedo)
        emit canRedoChanged(false);
}

void FaultEditStack::truncateRedo()
{
    mUndone.clear();
}

FaultInterpretationController::FaultInterpretationController(FaultSetStore *store,
                                                             SelectionContext *selection,
                                                             QObject *parent)
    : QObject(parent), m_store(store), m_selection(selection)
{
    m_editStack = new FaultEditStack(this);
    if (m_selection)
        connect(m_selection, &SelectionContext::selectionChanged, this,
                [this](const QStringList &ids, const QString &origin) {
                    onContextSelection(ids, origin);
                });
}

FaultInterpretationController::~FaultInterpretationController() = default;

bool FaultInterpretationController::reload(QString *error)
{
    m_set.clear();
    if (!m_store->load(m_set, error))
        return false;
    if (!m_activeFaultId.isEmpty() && !m_set.faultById(m_activeFaultId))
        m_activeFaultId.clear();
    refreshMapLayer();
    emit faultSetChanged();
    return true;
}

QString FaultInterpretationController::activeHorizon() const
{
    if (m_selection) {
        const QString h = m_selection->activeHorizon();
        if (!h.isEmpty())
            return h;
    }
    return QStringLiteral("H1");
}

// ---- 编辑原语 ----

QString FaultInterpretationController::addFault(const QString &name,
                                                const QString &interpreter)
{
    const QString faultId = QStringLiteral("f-%1").arg(m_set.nextFaultId());
    const QString unique = m_set.uniqueName(name.isEmpty() ? QStringLiteral("F") : name);
    if (!m_editStack->push(
        tr("新建断层 %1").arg(unique),
        [this, faultId] {
            m_set.removeFault(faultId);
            return commitModel();
        },
        [this, faultId, unique, interpreter] {
            Fault f;
            f.id = faultId;
            f.name = unique;
            f.interpreter = interpreter;
            f.visible = true;
            m_set.insertFault(f);
            return commitModel();
        }))
        return QString(); // 落盘失败：不入账、不拾取（#480 P3-4）
    m_activeFaultId = faultId; // 新建即拾取目标
    return faultId;
}

QPair<QString, QString> FaultInterpretationController::addStick(const FaultStick &stickIn)
{
    if (stickIn.points.size() < 2)
        return {};

    QString faultId = m_activeFaultId;
    if (!faultId.isEmpty() && !m_set.faultById(faultId))
        faultId.clear(); // 活动断层已被删 → 自动新建
    const bool createFault = faultId.isEmpty();
    QString name;
    if (createFault) {
        faultId = QStringLiteral("f-%1").arg(m_set.nextFaultId());
        name = m_set.uniqueName(QStringLiteral("F"));
    }
    FaultStick stick = stickIn;
    if (stick.id.isEmpty())
        stick.id = QStringLiteral("s-%1").arg(m_set.nextStickId());
    const QString interpreter = stick.interpreter;

    const bool ok = m_editStack->push(
        tr("拾取断层棒 %1").arg(stick.id),
        [this, faultId, stick, createFault] {
            m_set.removeStick(faultId, stick.id);
            if (createFault)
                dropFaultIfEmpty(m_set, faultId);
            return commitModel();
        },
        [this, faultId, name, interpreter, stick, createFault] {
            if (createFault && !m_set.faultById(faultId)) {
                Fault f;
                f.id = faultId;
                f.name = name;
                f.interpreter = interpreter;
                f.visible = true;
                m_set.insertFault(f);
            }
            m_set.addStick(faultId, stick, nullptr);
            return commitModel();
        });
    if (!ok)
        return {}; // 落盘失败：不入账、拒绝本次拾取（#480 P3-4）
    if (createFault)
        m_activeFaultId = faultId; // 后续拾取落同一断层
    return {faultId, stick.id};
}

bool FaultInterpretationController::removeStick(const QString &faultId, const QString &stickId)
{
    const Fault *f = m_set.faultById(faultId);
    const FaultStick *s = f ? f->stickById(stickId) : nullptr;
    if (!s)
        return false;
    const FaultStick snapshot = *s;
    const bool ok = m_editStack->push(
        tr("删除断层棒 %1").arg(stickId),
        [this, faultId, snapshot] {
            m_set.addStick(faultId, snapshot, nullptr);
            return commitModel();
        },
        [this, faultId, stickId] {
            m_set.removeStick(faultId, stickId);
            return commitModel();
        });
    return ok;
}

bool FaultInterpretationController::removeFault(const QString &faultId)
{
    const Fault *f = m_set.faultById(faultId);
    if (!f)
        return false;
    const Fault snapshot = *f;
    // 快照删除前的活动态：undo 必须对称恢复（redo 会清它），否则「删除→撤销」
    // 之后活动目标丢失，与头文件「undo/redo 对称重放」的契约不符（#194）。
    const QString activeSnapshot = m_activeFaultId;
    const bool ok = m_editStack->push(
        tr("删除断层 %1").arg(f->name),
        [this, snapshot, activeSnapshot] {
            m_set.insertFault(snapshot);
            m_activeFaultId = activeSnapshot;
            return commitModel();
        },
        [this, faultId] {
            m_set.removeFault(faultId);
            if (m_activeFaultId == faultId)
                m_activeFaultId.clear();
            return commitModel();
        });
    return ok;
}

bool FaultInterpretationController::renameFault(const QString &faultId, const QString &newName)
{
    const Fault *f = m_set.faultById(faultId);
    if (!f || newName.isEmpty() || f->name == newName)
        return false;
    if (m_set.hasFaultNamed(newName))
        return false; // 撞名预检：不产生空命令（模型层同样拒绝，此处防静默失败）
    const QString oldName = f->name;
    const bool ok = m_editStack->push(
        tr("重命名断层 %1→%2").arg(oldName, newName),
        [this, faultId, oldName] {
            m_set.renameFault(faultId, oldName);
            return commitModel();
        },
        [this, faultId, newName] {
            m_set.renameFault(faultId, newName);
            return commitModel();
        });
    return ok;
}

bool FaultInterpretationController::setFaultVisible(const QString &faultId, bool visible)
{
    const Fault *f = m_set.faultById(faultId);
    if (!f || f->visible == visible)
        return false;
    const bool ok = m_editStack->push(
        visible ? tr("显示断层 %1").arg(f->name) : tr("隐藏断层 %1").arg(f->name),
        [this, faultId, visible] {
            m_set.setFaultVisible(faultId, !visible);
            return commitModel();
        },
        [this, faultId, visible] {
            m_set.setFaultVisible(faultId, visible);
            return commitModel();
        });
    return ok;
}

bool FaultInterpretationController::setCut(const QString &faultIdIn, const FaultHorizonCut &cutIn)
{
    FaultHorizonCut cut = cutIn;
    if (cut.horizon.isEmpty())
        cut.horizon = activeHorizon();
    if (cut.horizon.isEmpty() || cut.wkt.isEmpty())
        return false;

    QString faultId = faultIdIn.isEmpty() ? m_activeFaultId : faultIdIn;
    if (!faultId.isEmpty() && !m_set.faultById(faultId))
        faultId.clear();
    const bool createFault = faultId.isEmpty();
    QString name;
    if (createFault) {
        faultId = QStringLiteral("f-%1").arg(m_set.nextFaultId());
        name = m_set.uniqueName(QStringLiteral("F"));
    }

    const FaultHorizonCut *old = m_set.cut(faultId, cut.horizon);
    const bool hadOld = old != nullptr;
    const FaultHorizonCut oldCut = hadOld ? *old : FaultHorizonCut{};

    const bool ok = m_editStack->push(
        tr("断层 %1 层位 %2 切割多边形")
            .arg(createFault ? name : m_set.faultById(faultId)->name, cut.horizon),
        [this, faultId, cut, hadOld, oldCut, createFault] {
            if (hadOld)
                m_set.setCut(faultId, oldCut);
            else
                m_set.removeCut(faultId, cut.horizon);
            if (createFault)
                dropFaultIfEmpty(m_set, faultId);
            return commitModel();
        },
        [this, faultId, name, cut, createFault] {
            if (createFault && !m_set.faultById(faultId)) {
                Fault f;
                f.id = faultId;
                f.name = name;
                f.visible = true;
                m_set.insertFault(f);
            }
            m_set.setCut(faultId, cut);
            return commitModel();
        });
    return ok;
}

bool FaultInterpretationController::removeCut(const QString &faultId, const QString &horizon)
{
    const FaultHorizonCut *c = m_set.cut(faultId, horizon);
    if (!c)
        return false;
    const FaultHorizonCut snapshot = *c;
    const bool ok = m_editStack->push(
        tr("删除 %1 层位切割").arg(horizon),
        [this, faultId, snapshot] {
            m_set.setCut(faultId, snapshot);
            return commitModel();
        },
        [this, faultId, horizon] {
            m_set.removeCut(faultId, horizon);
            return commitModel();
        });
    return ok;
}

bool FaultInterpretationController::setCutHangingSide(const QString &faultId,
                                                      const QString &horizon,
                                                      FaultHangingSide side)
{
    const FaultHorizonCut *c = m_set.cut(faultId, horizon);
    if (!c || c->hangingSide == side)
        return false;
    const FaultHorizonCut oldCut = *c;
    FaultHorizonCut newCut = *c;
    newCut.hangingSide = side;
    const bool ok = m_editStack->push(
        tr("%1 层位上盘方向 → %2").arg(horizon, FaultHorizonCut::hangingSideToString(side)),
        [this, faultId, oldCut] {
            m_set.setCut(faultId, oldCut);
            return commitModel();
        },
        [this, faultId, newCut] {
            m_set.setCut(faultId, newCut);
            return commitModel();
        });
    return ok;
}

// ---- 联动 ----

QString FaultInterpretationController::selectionId(const QString &faultId)
{
    return QStringLiteral("fault:") + faultId;
}

QString FaultInterpretationController::faultIdFromSelectionId(const QString &selectionId)
{
    return selectionId.startsWith(QLatin1String("fault:"))
        ? selectionId.mid(QString("fault:").size())
        : QString();
}

void FaultInterpretationController::selectFaults(const QStringList &faultIds,
                                                 const QString &origin)
{
    QStringList payload;
    for (const QString &id : faultIds) {
        if (m_set.faultById(id))
            payload.append(selectionId(id));
    }
    if (m_selection)
        m_selection->setSelection(payload, origin);
    else
        onContextSelection(payload, origin); // 无联动壳（测试）：本地回声
}

void FaultInterpretationController::onContextSelection(const QStringList &ids,
                                                       const QString &origin)
{
    QStringList faultIds;
    for (const QString &id : ids) {
        const QString f = faultIdFromSelectionId(id);
        if (!f.isEmpty() && m_set.faultById(f))
            faultIds.append(f);
    }
    m_selectedFaultIds = faultIds;

    // 地图镜像层回声（防自环：置选时短路边路信号）
    if (m_mapLayer) {
        m_mapLayer->blockSignals(true);
        QgsFeatureIds fids;
        if (m_mapLayer->isValid()) {
            QgsFeatureIterator it = m_mapLayer->getFeatures();
            QgsFeature feat;
            while (it.nextFeature(feat))
                if (faultIds.contains(feat.attribute(QStringLiteral("fault_id")).toString()))
                    fids.insert(feat.id());
        }
        m_mapLayer->selectByIds(fids);
        m_mapLayer->blockSignals(false);
    }
    emit faultSelectionChanged(faultIds, origin);
}

void FaultInterpretationController::onMapLayerSelectionChanged()
{
    if (!m_mapLayer || !m_mapLayer->isValid())
        return;
    QStringList faultIds;
    const QgsFeatureIds selected = m_mapLayer->selectedFeatureIds();
    if (!selected.isEmpty()) {
        QgsFeatureIterator it = m_mapLayer->getSelectedFeatures();
        QgsFeature feat;
        while (it.nextFeature(feat)) {
            const QString id = feat.attribute(QStringLiteral("fault_id")).toString();
            if (!faultIds.contains(id))
                faultIds.append(id);
        }
    }
    selectFaults(faultIds, QStringLiteral("fault_map"));
}

// ---- catalog 角色 ----

void FaultInterpretationController::setCatalogContext(DataCatalog *catalog,
                                                      const QString &entityId,
                                                      const QString &assetId)
{
    m_catalog = catalog;
    m_catalogEntityId = entityId;
    m_catalogAssetId = assetId;
    ensureFaultRoleLink();
}

void FaultInterpretationController::ensureFaultRoleLink()
{
    if (m_faultRoleLinked || !m_catalog || m_catalogEntityId.isEmpty() ||
        m_catalogAssetId.isEmpty() || m_set.faultCount() == 0)
        return;
    for (const EntityAssetLink &l : m_catalog->linksForAsset(m_catalogAssetId)) {
        if (l.entityId == m_catalogEntityId && l.role == QLatin1String("fault") &&
            !l.unresolved) {
            m_faultRoleLinked = true;
            return;
        }
    }
    EntityAssetLink link;
    link.entityType = QStringLiteral("seismic_survey");
    link.entityId = m_catalogEntityId;
    link.assetId = m_catalogAssetId;
    link.role = QStringLiteral("fault"); // 词表既有角色（roleregistry.cpp）
    link.isPrimary = false;
    link.note = QStringLiteral("断层解释（FaultSet）");
    QString err;
    if (m_catalog->addLink(link, &err))
        m_faultRoleLinked = true;
}

// ---- 层位图切割镜像层 ----

QgsVectorLayer *FaultInterpretationController::mapLayer() const
{
    return m_mapLayer;
}

QgsVectorLayer *FaultInterpretationController::ensureMapLayer(const QString &crsAuthId)
{
    if (m_mapLayer && m_mapLayer->isValid())
        return m_mapLayer;

    const QString kFaultCutLayerId = QStringLiteral("fault_cut");
    QgsProject *proj = QgsProject::instance();

    // 1. 优先通过 paleoLayerId 或图层名查找并复用工程已有图层
    if (proj) {
        // 先按 customProperty("paleoLayerId") 扫描
        QList<QgsVectorLayer *> cutLayers;
        for (QgsMapLayer *l : proj->mapLayers()) {
            if (auto *vl = qobject_cast<QgsVectorLayer *>(l)) {
                if (vl->isValid() &&
                    vl->customProperty(QStringLiteral("paleoLayerId")).toString() == kFaultCutLayerId) {
                    cutLayers.append(vl);
                }
            }
        }
        if (!cutLayers.isEmpty()) {
            m_mapLayer = cutLayers.first();
            for (int i = 1; i < cutLayers.size(); ++i) {
                proj->removeMapLayer(cutLayers[i]);
            }
        } else {
            // 若未找到，回退按名称 "断层切割" 扫描
            const auto candidates = proj->mapLayersByName(QStringLiteral("断层切割"));
            for (QgsMapLayer *l : candidates) {
                if (auto *vl = qobject_cast<QgsVectorLayer *>(l)) {
                    if (vl->isValid()) {
                        m_mapLayer = vl;
                        m_mapLayer->setCustomProperty(QStringLiteral("paleoLayerId"), kFaultCutLayerId);
                        break;
                    }
                }
            }
            // 清理多余的历史重复同名图层（若此前存在倍增残留）
            if (m_mapLayer && candidates.size() > 1) {
                for (QgsMapLayer *l : candidates) {
                    if (l != m_mapLayer)
                        proj->removeMapLayer(l);
                }
            }
        }
    }

    // 2. 若工程中尚无该图层，创建新 memory 图层并打上 paleoLayerId
    if (!m_mapLayer) {
        const QString uri = QStringLiteral(
            "Polygon?crs=%1&field=fault_id:string&field=fault_name:string&field=horizon:string")
                                 .arg(crsAuthId);
        QgsVectorLayer *layer =
            new QgsVectorLayer(uri, QStringLiteral("断层切割"), QStringLiteral("memory"));
        if (!layer->isValid()) {
            delete layer;
            return nullptr;
        }
        layer->setCustomProperty(QStringLiteral("paleoLayerId"), kFaultCutLayerId);
        m_mapLayer = layer;
        if (proj)
            proj->addMapLayer(m_mapLayer);
        else
            layer->setParent(this); // 无工程时由控制器持有，避免 memory 图层泄漏
    }

    // 3. 重新绑定选择信号（避免重复连接）并整刷要素
    disconnect(m_mapLayer, &QgsVectorLayer::selectionChanged, this, nullptr);
    connect(m_mapLayer, &QgsVectorLayer::selectionChanged, this,
            [this] { onMapLayerSelectionChanged(); });
    refreshMapLayer();
    return m_mapLayer;
}

void FaultInterpretationController::refreshMapLayer()
{
    if (!m_mapLayer || !m_mapLayer->isValid())
        return;
    QgsVectorLayer *layer = m_mapLayer;
    layer->blockSignals(true);
    layer->selectByIds(QgsFeatureIds());
    layer->dataProvider()->truncate();
    QgsFeatureList features;
    for (const Fault &f : m_set.faults()) {
        if (!f.visible)
            continue;
        for (const FaultHorizonCut &c : f.cuts) {
            QgsFeature feat(layer->fields());
            const QgsGeometry geom = QgsGeometry::fromWkt(c.wkt);
            if (geom.isNull() || geom.isEmpty()) // WKT 解析失败/空几何不入层
                continue;
            feat.setGeometry(geom);
            feat.setAttribute(QStringLiteral("fault_id"), f.id);
            feat.setAttribute(QStringLiteral("fault_name"), f.name);
            feat.setAttribute(QStringLiteral("horizon"), c.horizon);
            features.append(feat);
        }
    }
    layer->dataProvider()->addFeatures(features);
    layer->updateExtents();
    layer->blockSignals(false);
    layer->triggerRepaint();
}

void FaultInterpretationController::reset()
{
    m_set.clear();
    if (m_editStack)
        m_editStack->clear();
    m_catalog = nullptr;
    m_catalogEntityId.clear();
    m_catalogAssetId.clear();
    m_faultRoleLinked = false;
    m_activeFaultId.clear();
    m_selectedFaultIds.clear();
    m_lastError.clear();
    m_mapLayer = nullptr;
    emit faultSetChanged();
    // 工程关闭/会话清理：空选择广播。origin 不是 fault_panel——视图据此把
    // 「控制器复位」与「面板自己清空」区分开，照常清树。
    emit faultSelectionChanged({}, QStringLiteral("fault_controller"));
}

// ---- 内部 ----

bool FaultInterpretationController::commitModel()
{
    QString err;
    const bool ok = m_store->save(m_set, &err);
    m_lastError = ok ? QString() : err;
    ensureFaultRoleLink();
    refreshMapLayer();
    emit faultSetChanged();
    return ok;
}

} // namespace paleo::fault
