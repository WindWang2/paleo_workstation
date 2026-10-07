// 层：视图
#include "layerpropertiesdialog.h"
#include "../uienv_internal.h"

#include <QCoreApplication>
#include <QComboBox>
#include <QDialog>
#include <QDomDocument>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include "../notifications/paleonotify.h"
#include <QPushButton>
#include <QVBoxLayout>

#include <qgslayerpropertiesdialog.h>
#include <qgsmapcanvas.h>
#include <qgsmaplayer.h>
#include <qgsmaplayerconfigwidget.h>
#include <qgsmaplayerconfigwidgetfactory.h>
#include <qgsmaplayerstylemanager.h>
#include <qgsmessagebar.h>
#include <qgsproject.h>
#include <qgsrasterlayer.h>
#include <qgsrasterlayerproperties.h>
#include <qgsreadwritecontext.h>
#include <qgsvectorlayer.h>
#include <qgsvectorlayerproperties.h>

#include "../paleotheme.h"
#include "metadata/layermanifest.h"
#include "qgis/qgislayerservice.h"

namespace
{

using paleo::ui_detail::isOffscreen;

const QString kDash = QStringLiteral("—");

// layerId 解析链：a) QgisLayerService 实例缓存；b) QgsProject 工程图层按
// customProperty("paleoLayerId")==layerId 匹配（覆盖手工加层/缺源层——
// qgislayerservice.cpp:148 的登记约定；测试里服务回退的 QgsProject 单例
// 与 instance() 一致）。都没有 → nullptr。
QgsMapLayer *resolveLayer(QgisLayerService *svc, const QString &layerId)
{
    if (layerId.isEmpty())
        return nullptr;
    if (svc)
        if (QgsMapLayer *cached = svc->layer(layerId))
            return cached;
    const auto layers = QgsProject::instance()->mapLayers();
    for (auto it = layers.cbegin(); it != layers.cend(); ++it)
        if (it.value()->customProperty(QStringLiteral("paleoLayerId")).toString() == layerId)
            return it.value();
    return nullptr;
}

struct DeclMatch
{
    bool found = false;
    LayerDeclaration decl;
};

// declared()（manifest 全量）里按 layerId 匹配；无声明 → found=false。
DeclMatch findDeclaration(QgisLayerService *svc, const QString &layerId)
{
    DeclMatch m;
    if (!svc || layerId.isEmpty())
        return m;
    const QVector<LayerDeclaration> all = svc->declared();
    for (const LayerDeclaration &d : all)
        if (d.layerId == layerId)
        {
            m.found = true;
            m.decl = d;
            break;
        }
    return m;
}

// layerId ↔ assetId 的关联面（C4/wave-deepen-perf 激活）：数据链在生成侧——
// 派生产物 commit 后 workflow 把 catalog assetId 盖到已实例化图层对象
//（paleoAssetId 自定义属性，workflows.cpp stampLayerAssetLink；随 .qgz
// 持久化）。此处按同一解析链（实例缓存 → 工程 paleoLayerId 扫描）读回；
// 层未实例化或非派生产物 → 空 = 未关联（按钮禁用 + reason tooltip）。
// catalog 侧显式关联表（LayerDeclaration.asset 字段 / 链表）仍是递延项
//（TODOS「图层平台」节），本函数即其预留单点扩展。
QString assetIdForLayer(QgisLayerService *svc, const QString &layerId)
{
    QgsMapLayer *layer = resolveLayer(svc, layerId);
    if (!layer)
        return QString();
    const QString assetId = layer->customProperty(QStringLiteral("paleoAssetId")).toString();
    return assetId;
}

// ---- Paleo 业务页：只读字段 + 样式管理（QgsMapLayerStyleManager） ----
// 只渲染 + 发意图；样式改动全部落 QgsMapLayer 对象（QGIS 持久化进 .qgz），
// 不复制路径/CRS/provider 到 Paleo 侧任何存储。业务字段只读展示。
class PaleoLayerConfigPage : public QgsMapLayerConfigWidget
{
    Q_OBJECT

  public:
    PaleoLayerConfigPage(QgsMapLayer *layer, QgsMapCanvas *canvas, const QString &layerId,
                         const LayerDeclaration &decl, bool declFound,
                         const QString &assetId, QWidget *parent = nullptr)
        : QgsMapLayerConfigWidget(layer, canvas, parent)
        , m_decl(decl)
        , m_declFound(declFound)
        , m_assetId(assetId) // C4：assetIdForLayer（生成侧 paleoAssetId 盖章链）
    {
        setObjectName(QStringLiteral("paleoBusinessPage"));
        // surface 底走应用 palette（PaleoTheme::designLightPalette 的 Window=
        // surface 白）；视图代码不写颜色字面量（paleotheme.h 契约）。
        setAutoFillBackground(true);

        auto *root = new QVBoxLayout(this);
        root->setContentsMargins(PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm); // DESIGN.md spacing sm=8
        root->setSpacing(PaleoTheme::tokens().spacingSm);

        // —— 只读字段（两列 QForm；次级标签 8pt，DESIGN.md label token）——
        auto *form = new QFormLayout();
        form->setVerticalSpacing(PaleoTheme::tokens().spacingSm);
        form->setHorizontalSpacing(PaleoTheme::tokens().spacingSm);
        m_propLayerId = addFieldRow(form, tr("图层 ID"), layerId,
                                    QStringLiteral("paleoPropLayerId"));
        m_propGroup = addFieldRow(form, tr("所属组"), declField(m_decl.group),
                                  QStringLiteral("paleoPropGroup"));
        m_propHorizon = addFieldRow(form, tr("层位"), declField(m_decl.horizon),
                                    QStringLiteral("paleoPropHorizon"));
        m_propAsset = addFieldRow(form, tr("关联资产"),
                                  m_assetId.isEmpty() ? tr("未关联") : m_assetId,
                                  QStringLiteral("paleoPropAsset"));
        m_propSource = addFieldRow(form, tr("来源"), declField(m_decl.source),
                                   QStringLiteral("paleoPropSource"));
        // 主线5：创建时间 = instantiate() 落的 paleoCreatedAt 图层自定义属性
        //（随 .qgz 持久化）；无该属性的层外挂图层降级为占位符。
        const QString createdAt = layer
            ? layer->customProperty(QStringLiteral("paleoCreatedAt")).toString()
            : QString();
        m_propCreated = addFieldRow(form, tr("创建时间"),
                                    createdAt.isEmpty() ? kDash : createdAt,
                                    QStringLiteral("paleoPropCreated"));
        root->addLayout(form);

        // 「在数据页查看资产」：assetId 非空才启用（当前恒「未关联」禁用态，
        // DESIGN.md：禁用控件必须带 reason tooltip）。
        m_inspectAssetButton = new QPushButton(tr("在数据页查看资产"), this);
        m_inspectAssetButton->setObjectName(QStringLiteral("paleoInspectAssetButton"));
        if (m_assetId.isEmpty())
        {
            m_inspectAssetButton->setEnabled(false);
            m_inspectAssetButton->setToolTip(
                tr("该图层未关联数据资产（图层清单暂无关联登记）"));
        }
        root->addWidget(m_inspectAssetButton, 0, Qt::AlignLeft);

        // —— 样式管理（QgsMapLayerStyleManager；预设与 .qml 全落图层对象）——
        buildStyleGroup(root);
        root->addStretch(1);
    }

    QPushButton *inspectAssetButton() const { return m_inspectAssetButton; }
    QString assetId() const { return m_assetId; }

    // 只读页：无对话框级 apply 语义（样式按钮即时生效；其余属性由原生页负责）。
    void apply() override {}

  private:
    QLabel *addFieldRow(QFormLayout *form, const QString &caption, const QString &value,
                        const QString &objectName)
    {
        auto *cap = new QLabel(caption, this);
        QFont f = cap->font();
        f.setPointSize(PaleoTheme::tokens().labelPt); // 8pt 次级标签
        cap->setFont(f);
        // text-muted：走 palette 的 PlaceholderText 槽（design palette 已映射
        // 到 #5D6E80）——不落颜色字面量。
        cap->setForegroundRole(QPalette::PlaceholderText);

        auto *lbl = new QLabel(value, this);
        lbl->setObjectName(objectName); // 测试/接线锚点（paleoProp* 约定）
        lbl->setTextInteractionFlags(Qt::TextSelectableByMouse);
        lbl->setWordWrap(true);
        form->addRow(cap, lbl);
        return lbl;
    }

    static QString declField(const QString &value)
    {
        return value.isEmpty() ? kDash : value;
    }

    void buildStyleGroup(QVBoxLayout *root)
    {
        m_styleGroup = new QGroupBox(tr("样式"), this);
        m_styleGroup->setObjectName(QStringLiteral("paleoStyleGroup"));
        QFont gf = m_styleGroup->font();
        gf.setPointSize(PaleoTheme::tokens().labelPt);
        m_styleGroup->setFont(gf);

        auto *grid = new QGridLayout(m_styleGroup);
        grid->setContentsMargins(PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm);
        grid->setVerticalSpacing(PaleoTheme::tokens().spacingSm);
        grid->setHorizontalSpacing(PaleoTheme::tokens().spacingSm);

        m_savePresetButton = new QPushButton(tr("保存当前样式为预设"), m_styleGroup);
        m_savePresetButton->setObjectName(QStringLiteral("paleoSavePresetButton"));
        m_presetCombo = new QComboBox(m_styleGroup);
        m_presetCombo->setObjectName(QStringLiteral("paleoPresetCombo"));
        m_restorePresetButton = new QPushButton(tr("从预设恢复"), m_styleGroup);
        m_restorePresetButton->setObjectName(QStringLiteral("paleoRestorePresetButton"));
        m_exportQmlButton = new QPushButton(tr("导出 .qml…"), m_styleGroup);
        m_exportQmlButton->setObjectName(QStringLiteral("paleoExportQmlButton"));
        m_importQmlButton = new QPushButton(tr("导入 .qml…"), m_styleGroup);
        m_importQmlButton->setObjectName(QStringLiteral("paleoImportQmlButton"));

        grid->addWidget(m_savePresetButton, 0, 0);
        grid->addWidget(m_presetCombo, 0, 1);
        grid->addWidget(m_restorePresetButton, 0, 2);
        grid->addWidget(m_exportQmlButton, 1, 0);
        grid->addWidget(m_importQmlButton, 1, 1);
        grid->setColumnStretch(1, 1);

        root->addWidget(m_styleGroup);

        if (!mLayer || !mLayer->styleManager())
        {
            // 未实例化/无样式管理器：整组隐藏（无样式可管，不摆禁用件）。
            m_styleGroup->hide();
            return;
        }
        refreshPresets();

        connect(m_savePresetButton, &QPushButton::clicked, this, [this] { savePreset(); });
        connect(m_restorePresetButton, &QPushButton::clicked, this, [this] { restorePreset(); });
        connect(m_presetCombo, &QComboBox::activated, this, [this](int) { restorePreset(); });
        connect(m_exportQmlButton, &QPushButton::clicked, this, [this] { exportQml(); });
        connect(m_importQmlButton, &QPushButton::clicked, this, [this] { importQml(); });
    }

    void refreshPresets()
    {
        QgsMapLayerStyleManager *sm = mLayer ? mLayer->styleManager() : nullptr;
        if (!sm)
            return;
        m_presetCombo->blockSignals(true);
        m_presetCombo->clear();
        const QStringList names = sm->styles();
        m_presetCombo->addItems(names);
        const int idx = m_presetCombo->findText(sm->currentStyle());
        if (idx >= 0)
            m_presetCombo->setCurrentIndex(idx);
        m_presetCombo->blockSignals(false);
    }

    // 默认预设名 = manifest styleRef 的 basename 去目录去 .qml 后缀
    // （styles/facies.T1.qml → facies.T1）；无声明 → 「预设1」。
    QString defaultPresetName() const
    {
        if (m_declFound && !m_decl.styleRef.isEmpty())
            return QFileInfo(m_decl.styleRef).completeBaseName();
        return tr("预设1");
    }

    void savePreset()
    {
        QgsMapLayerStyleManager *sm = mLayer ? mLayer->styleManager() : nullptr;
        if (!sm)
            return;
        QString name = defaultPresetName();
        if (!isOffscreen())
        {
            bool ok = false;
            name = QInputDialog::getText(this, tr("保存样式预设"), tr("预设名："),
                                         QLineEdit::Normal, name, &ok);
            if (!ok || name.trimmed().isEmpty())
                return;
            name = name.trimmed();
        }
        // addStyleFromLayer 拒绝非唯一名 → 数字后缀递增避免静默失败。
        if (sm->styles().contains(name))
        {
            const QString base = name;
            int i = 2;
            while (sm->styles().contains(QStringLiteral("%1 %2").arg(base).arg(i)))
                ++i;
            name = QStringLiteral("%1 %2").arg(base).arg(i);
        }
        if (!sm->addStyleFromLayer(name))
        {
            if (isOffscreen())
                qWarning("paleo: addStyleFromLayer('%s') failed",
                         qPrintable(name));
            else
                PaleoNotify::warning(this, tr("保存样式预设"),
                                     tr("保存预设失败：%1").arg(name));
            return;
        }
        refreshPresets();
    }

    void restorePreset()
    {
        QgsMapLayerStyleManager *sm = mLayer ? mLayer->styleManager() : nullptr;
        if (!sm)
            return;
        const QString name = m_presetCombo->currentText();
        if (name.isEmpty())
            return;
        if (!sm->setCurrentStyle(name))
        {
            if (!isOffscreen())
                PaleoNotify::warning(this, tr("从预设恢复"),
                                     tr("无法应用预设：%1").arg(name));
            return;
        }
        refreshPresets();
    }

    void exportQml()
    {
        if (isOffscreen() || !mLayer)
            return; // offscreen 守卫：跳过文件对话框（QTest 环境）
        const QString path = QFileDialog::getSaveFileName(
            this, tr("导出样式 .qml"),
            QStringLiteral("%1.qml").arg(mLayer->name()), tr("QGIS 样式文件 (*.qml)"));
        if (path.isEmpty())
            return;
        QDomDocument doc;
        QString styleErr;
        QgsReadWriteContext ctx;
        mLayer->exportNamedStyle(doc, styleErr, ctx);
        if (!styleErr.isEmpty())
        {
            PaleoNotify::warning(this, tr("导出样式 .qml"), styleErr);
            return;
        }
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly) || f.write(doc.toByteArray()) < 0)
            PaleoNotify::warning(this, tr("导出样式 .qml"),
                                 tr("无法写入文件：%1").arg(path));
    }

    void importQml()
    {
        if (isOffscreen() || !mLayer)
            return;
        const QString path = QFileDialog::getOpenFileName(
            this, tr("导入样式 .qml"), QString(), tr("QGIS 样式文件 (*.qml)"));
        if (path.isEmpty())
            return;
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
        {
            PaleoNotify::warning(this, tr("导入样式 .qml"),
                                 tr("无法读取文件：%1").arg(path));
            return;
        }
        QDomDocument doc;
        if (!doc.setContent(&f))
        {
            PaleoNotify::warning(this, tr("导入样式 .qml"), tr("不是有效的样式文件。"));
            return;
        }
        QString styleErr;
        if (!mLayer->importNamedStyle(doc, styleErr) || !styleErr.isEmpty())
            PaleoNotify::warning(this, tr("导入样式 .qml"), styleErr);
        refreshPresets();
    }

    LayerDeclaration m_decl;
    bool m_declFound = false;
    QString m_assetId;

    QLabel *m_propLayerId = nullptr;
    QLabel *m_propGroup = nullptr;
    QLabel *m_propHorizon = nullptr;
    QLabel *m_propAsset = nullptr;
    QLabel *m_propSource = nullptr;
    QLabel *m_propCreated = nullptr;
    QPushButton *m_inspectAssetButton = nullptr;

    QGroupBox *m_styleGroup = nullptr;
    QPushButton *m_savePresetButton = nullptr;
    QComboBox *m_presetCombo = nullptr;
    QPushButton *m_restorePresetButton = nullptr;
    QPushButton *m_exportQmlButton = nullptr;
    QPushButton *m_importQmlButton = nullptr;
};

// 业务页工厂：openLayerProperties 里把业务页挂进原生属性对话框的一页。
// 探针证据（/tmp/pw-layers-probe-b）：QGIS 4.2 的
// supportLayerPropertiesDialog() 是 virtual 且默认 return false——
// setSupportLayerPropertiesDialog() 只写成员、virtual 默认实现不读它，
// 不覆写则 addPropertiesPageFactory 直接 return，页挂不进去。
class PaleoPageFactory : public QgsMapLayerConfigWidgetFactory
{
  public:
    explicit PaleoPageFactory(LayerPropertiesDialog *host, const QString &layerId)
        : m_host(host), m_layerId(layerId)
    {
        // 选项列表页签名；工厂非 QObject，无 tr() 上下文——显式 context 走 translate。
        setTitle(QCoreApplication::translate("PaleoPageFactory", "Paleo 业务"));
    }

    bool supportLayerPropertiesDialog() const override { return true; }
    bool supportsLayer(QgsMapLayer *layer) const override
    {
        Q_UNUSED(layer);
        return true;
    }

    QgsMapLayerConfigWidget *createWidget(QgsMapLayer *layer, QgsMapCanvas *canvas,
                                          bool dockWidget = true,
                                          QWidget *parent = nullptr) const override
    {
        Q_UNUSED(layer);
        Q_UNUSED(canvas);
        Q_UNUSED(dockWidget);
        // 页面字段/样式上下文由 dialog 侧统一解析（与测试直取通道同一条路径）。
        return qobject_cast<QgsMapLayerConfigWidget *>(
            m_host->createBusinessPage(m_layerId, parent));
    }

  private:
    LayerPropertiesDialog *m_host = nullptr;
    QString m_layerId;
};

} // namespace

LayerPropertiesDialog::LayerPropertiesDialog(QgisLayerService *layerService,
                                             const Deps &deps, QObject *parent)
    : QObject(parent), m_layerService(layerService), m_canvas(deps.canvas),
      m_messageBar(deps.messageBar)
{
}

LayerPropertiesDialog::~LayerPropertiesDialog()
{
    if (m_ownMessageBar)
        delete m_messageBar; // 自造持有的 QgsMessageBar（Deps 缺省时）
}

QWidget *LayerPropertiesDialog::createBusinessPage(const QString &layerId, QWidget *parent)
{
    if (layerId.isEmpty())
        return nullptr;
    const DeclMatch decl = findDeclaration(m_layerService, layerId);
    QgsMapLayer *layer = resolveLayer(m_layerService, layerId);
    if (!decl.found && !layer)
        return nullptr; // 未知 layerId（既无声明也不在工程）

    auto *page = new PaleoLayerConfigPage(
        layer, m_canvas, layerId, decl.found ? decl.decl : LayerDeclaration(), decl.found,
        assetIdForLayer(m_layerService, layerId), parent);

    // 意图信号：按钮 → 数据页资产检视（接线 E 订阅；assetId 空 = 未关联，
    // 按钮已禁用，此守卫双保险）。
    const QString assetId = page->assetId();
    connect(page->inspectAssetButton(), &QPushButton::clicked, this, [this, assetId] {
        if (!assetId.isEmpty())
            emit assetInspectionRequested(assetId);
    });
    return page;
}

void LayerPropertiesDialog::openLayerProperties(const QString &layerId)
{
    // 解析链 a) 实例缓存 → b) 工程 paleoLayerId 扫描 → c) 都没有：无操作。
    QgsMapLayer *layer = resolveLayer(m_layerService, layerId);
    if (!layer)
        return;

    if (!m_messageBar)
    {
        m_messageBar = new QgsMessageBar(); // 无父对象：本类持有，析构删除
        m_ownMessageBar = true;
    }

    // 按类型分发原生壳（探针证据：两壳在嵌入式 offscreen 进程均可构造）；
    // mesh 等其他类型 → 无操作。
    QgsLayerPropertiesDialog *native = nullptr;
    if (auto *vl = qobject_cast<QgsVectorLayer *>(layer))
        native = new QgsVectorLayerProperties(m_canvas, m_messageBar, vl);
    else if (auto *rl = qobject_cast<QgsRasterLayer *>(layer))
        native = new QgsRasterLayerProperties(rl, m_canvas);
    else
        return;
    native->setObjectName(QStringLiteral("paleoLayerPropertiesDialog"));

    // 业务页挂进原生壳（须在 show/exec 之前）。工厂非 QObject——挂对话框
    // destroyed 删除，保证活到对话框销毁。
    auto *factory = new PaleoPageFactory(this, layerId);
    connect(native, &QObject::destroyed, native, [factory] { delete factory; });
    native->addPropertiesPageFactory(factory);

    if (isOffscreen())
    {
        // offscreen 纪律：构造不 exec（不模态阻塞），交 deleteLater 回收
        //（QTest 在 processEvents 前可检查再冲掉）。
        native->deleteLater();
        return;
    }

    const bool accepted = native->exec() == QDialog::Accepted;
    if (accepted)
        emit layerPropertiesApplied(layerId);
    native->deleteLater();
}

#include "layerpropertiesdialog.moc"
