// 层：视图
#pragma once
#include <QMetaType>
#include <QPointer>
#include <QString>
#include <QWidget>

class QLabel;
class QPushButton;
class QStackedWidget;
class QgsLayoutItem;
class QgsPanelWidgetStack;

// ui/layout/ — PaleoLayoutItemPanel: item-properties dock host for the layout
// designer (subtask B).
//
// API deviation from the original spec (runtime-verified against QGIS 4.2.2):
// the per-type property widgets are obtained through the canonical embedder
// channel QgsGui::layoutItemGuiRegistry()->createItemWidget(item), which
// returns a QgsLayoutItemBaseWidget (a QgsPanelWidget; qgslayoutitemwidget.h).
// The 3.x app-side hosting pattern around it is gone from public API. Two
// runtime facts shape this host:
//
//   1. In an embedder process the GUI registry starts EMPTY (verified:
//      itemMetadataIds().size() == 0, createItemWidget() == nullptr), because
//      upstream registers the default item GUI metadata from src/app only.
//      This panel therefore registers fallback QgsLayoutItemGuiMetadata for
//      the known layout item types, whose widget is a QgsLayoutItemBaseWidget
//      hosting the public QgsLayoutItemPropertiesWidget (common properties:
//      position/size/rotation, background, frame, item id). Registration only
//      fills types with no metadata yet, so richer per-type widgets registered
//      later (app-level or vendored) win automatically.
//   2. QgsLayoutItemPropertiesWidget was NOT deleted in 4.2 — it exists
//      (qgslayoutitemwidget.h) but covers common properties only. The true
//      per-type widget classes (QgsLayoutLabelWidget etc.) are not exported
//      (headers not installed), so per-type native pages are unavailable to
//      distro-header embedders; the Paleo override区 below compensates for the
//      two types the paleo workflows need (label, scalebar).
//
// Change notification is coarse-grained: the panel forwards the hosted item's
// QgsLayoutObject::changed() as itemChanged(item). Native widget edits, panel
// actions and direct item API calls all surface there, but there is no
// per-property granularity in the public API.
//
// Lifetime: a QPointer plus QObject::destroyed() guards against item deletion
// while hosted; destruction clears the host and emits itemChanged(nullptr).
class PaleoLayoutItemPanel : public QWidget
{
    Q_OBJECT

  public:
    explicit PaleoLayoutItemPanel( QWidget *parent = nullptr );

    // Hosts the native property widget for \a item (no rebuild when the same
    // item is set again). A null item clears the host and shows a placeholder.
    void setItem( QgsLayoutItem *item );

    //! The currently hosted item, or nullptr. (Defined in the .cpp: the
    // QPointer<QgsLayoutItem> access needs the complete item type, which the
    // self-contained header must not force on every includer.)
    QgsLayoutItem *item() const;

    //! Records the active horizon title used by applyHorizonTitle().
    void setActiveHorizonTitle( const QString &title );

    //! The recorded active horizon title (empty when unset).
    QString activeHorizonTitle() const { return m_horizonTitle; }

    // Policy: full replacement. Sets the hosted label's text to the active
    // horizon title (no prefixing/templating). Returns true when applied;
    // false (no-op) when the current item is not a QgsLayoutItemLabel or no
    // title was recorded.
    bool applyHorizonTitle();

    // Scalebar presets (style/renderer + units + segments):
    //   0 单厢简洁  — 'Single Box', km, 4 segments / 2 left
    //   1 黑白双厢  — 'Double Box', km, 6 segments / 0 left, white/black fills
    //   2 线段刻度  — 'Line Ticks Down', m, 5 segments / 0 left
    // Only effective while the hosted item is a QgsLayoutItemScaleBar; any
    // other current item or an out-of-range preset is a no-op (the preset
    // buttons carry the §35 reason tooltip for the disabled case).
    void applyScalebarPreset( int preset );

  signals:
    // Coarse-grained: emitted whenever the hosted item changes (native widget
    // edit, panel action, or direct item API call), and with nullptr when the
    // hosted item is destroyed. setItem() itself does not emit.
    void itemChanged( QgsLayoutItem *item );

  private:
    void hostItemWidget();
    void clearHostedWidget();
    void updateBusinessControls();

    QPointer<QgsLayoutItem> m_item;
    QString m_horizonTitle;

    QStackedWidget *m_pages = nullptr;
    QgsPanelWidgetStack *m_stack = nullptr;
    QLabel *m_placeholder = nullptr;

    QLabel *m_horizonValue = nullptr;
    QPushButton *m_applyHorizonButton = nullptr;
    QPushButton *m_presetButtons[3] = {};
};

// Lets QSignalSpy / queued connections carry itemChanged(QgsLayoutItem*) args;
// the panel constructor registers the metatype once. Opaque-pointer form is
// required because QgsLayoutItem is (and stays) forward-declared here.
Q_DECLARE_OPAQUE_POINTER( QgsLayoutItem * )
