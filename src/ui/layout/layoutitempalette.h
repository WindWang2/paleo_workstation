// 层：视图
#pragma once
#include <QList>
#include <QMetaObject>
#include <QPointer>
#include <QString>
#include <QWidget>

class QToolButton;
class QgsLayout;
class QgsLayoutItem;
class QgsLayoutView;
class QgsLayoutViewToolAddItem;

// ui/layout/ — PaleoLayoutItemPalette: element-tool palette for the layout
// designer's left dock (subtask A).
//
// QGIS 4.2 contract this class is built on (verified by probe against the
// installed 4.2.2 headers + runtime):
//   * QgsGui::layoutItemGuiRegistry() starts EMPTY. The default item GUI
//     metadata (18 entries) is registered by
//     QgsLayoutGuiUtils::registerGuiForKnownItemTypes(), which lives in
//     qgis_gui since 4.x — the embedder's counterpart of what the QGIS app
//     does at startup.
//   * GUI metadata ids are sequential ints assigned at registration time
//     (mMetadata.count()); they are NOT QgsLayoutItemRegistry::ItemType
//     values. Everything in this palette — the itemRequested signal,
//     attach(), addItemNow() — speaks GUI-registry metadata ids, which is
//     what QgsLayoutViewToolAddItem::setItemMetadataId() and
//     QgsLayoutItemGuiRegistry::createItem() expect.
//   * LayoutTextTable has no default GUI metadata in 4.2; the palette
//     registers one (multiframe factory, mirroring upstream's attribute
//     table metadata).
//   * Multiframe metadata create functions (attribute/manual/text tables)
//     return a QgsLayoutFrame that is ALREADY added to the layout through
//     QgsLayoutMultiFrame::addFrame(); addItemNow guards the second add the
//     same way QgsLayoutViewToolAddItem does (item->scene() != layout).
//
// Layout3DMap is deliberately absent: its GUI metadata is registered by
// QGIS's 3D app integration, which an embedder process does not have; the
// task allows trimming to Chart instead.
class PaleoLayoutItemPalette : public QWidget
{
    Q_OBJECT

  public:
    explicit PaleoLayoutItemPalette( QWidget *parent = nullptr );
    ~PaleoLayoutItemPalette() override;

    // Guarantees the default item GUI metadata exists: registers QGIS's
    // default 18 entries (via QgsLayoutGuiUtils) when the registry lacks
    // them, then registers the palette's own Text Table metadata when
    // missing. Idempotent; safe to call repeatedly and from any thread that
    // already owns the QGIS gui singletons (main thread in practice).
    static bool ensureDefaultItemMetadataRegistered();

    // Programmatic "drop now": creates the item through the GUI registry
    // factory (never a bare constructor — registry compatibility is a hard
    // requirement), sizes it with a sensible per-type default, centers it
    // on the first page (nominal A4 when the layout has no pages yet),
    // applies QGIS's new-item defaults (newItemAddedToLayout: legend/scale
    // bar map linking, label text, north arrow picture path...), adds it to
    // the layout and selects it — mirroring QgsLayoutViewToolAddItem's
    // release sequence exactly.
    //
    // Multiframe types (attribute/manual/text tables, HTML) are created via
    // their multiframe factory: the returned item is the first frame of a
    // QgsLayoutMultiFrame also owned by the layout.
    //
    // Returns false (and leaves *created null) for invalid ids, unknown
    // metadata or a null layout.
    static bool addItemNow( int itemMetadataId, QgsLayout *layout, QgsLayoutItem **created = nullptr );

    // Wires itemRequested to the QGIS-native interactive add flow: a shared
    // QgsLayoutViewToolAddItem whose metadata id follows the clicked button,
    // activated as the view's current tool (drag-a-rectangle creation).
    // Safe to re-attach; passing nullptr detaches.
    void attach( QgsLayoutView *view );

    //! The view attach() last wired, if it is still alive.
    QgsLayoutView *attachedView() const;

    //! The shared interactive add tool created by attach(), if attached.
    QgsLayoutViewToolAddItem *addItemTool() const;

    //! Metadata ids behind the item buttons, in visual order; -1 entries are
    //! buttons whose type is unavailable in this QGIS build.
    QList<int> itemMetadataIds() const { return m_buttonMetadataIds; }

    // Public triggers for hosts that surface the same interactions outside the
    // palette widget (e.g. the designer shell's Items menu): they emit the
    // signals below, which attach() has already wired to the interactive
    // QgsLayoutViewToolAddItem path. One source of truth — the menu never
    // re-implements item creation.
  public slots:
    void requestItem( int itemMetadataId );
    void requestPageProperties();

  signals:
    //! A palette button was clicked; carries the GUI-registry metadata id of
    //! the requested element type (NOT a QgsLayoutItemRegistry::ItemType).
    void itemRequested( int itemMetadataId );

    //! The "Page Properties" entry was activated (no GUI metadata exists for
    //! LayoutPage, so page setup travels on its own signal).
    void pagePropertiesRequested();

  private:
    struct ButtonSpec
    {
        const char *objectName;
        int coreType;             // QgsLayoutItemRegistry::ItemType
        const char *variantName;  // untranslated QGIS visibleName for duplicate-type variants
        const char *fallbackName; // untranslated name used as button text if id resolution fails
    };

    QWidget *buildGroup( const QString &objectName, const QString &title,
                         const QList<ButtonSpec> &specs );
    int resolveMetadataId( int coreType, const QString &variantName ) const;

    QList<int> m_buttonMetadataIds;
    QPointer<QgsLayoutView> m_view;
    QPointer<QgsLayoutViewToolAddItem> m_addItemTool;
    QMetaObject::Connection m_attachConnection;
};
