#pragma once
#include <QString>
#include <QVector>

#include "../metadata/layermanifest.h"

class QgsProject;

// §37 projection seam — the manifest is the layer-SET authority and the .qgz is
// only its rendered projection. QgsProject::write() persists instantiated
// QgsMapLayers natively, so declared-but-uninstantiated layers would silently
// vanish from the saved file. ManifestProjection closes that loop by mirroring
// the FULL declared set into project custom properties, which QGIS serializes
// inside the .qgz regardless of layer instantiation state.
//
// Design choice: JSON array in a custom property (scope "paleo", key
// "manifest") rather than synthetic legend entries — no phantom layer-tree
// nodes, no provider side effects, and the payload round-trips verbatim.
class ManifestProjection
{
  public:
    // Serializes the entire declared set (instantiated or not) into the
    // project's custom properties. Must be called BEFORE QgsProject::write()
    // so the payload is captured by the save. Returns false (+error) on a null
    // project or a failed writeEntry.
    static bool embedDeclarations( QgsProject *project,
                                   const QVector<LayerDeclaration> &decls,
                                   QString *error = nullptr );

    // Reads the embedded declaration array back from a (reopened) project.
    // 'instantiated' is runtime-only and always returns false — the layer
    // service decides what to materialize after load. A missing or malformed
    // payload yields an empty vector (treat as "no projection stored").
    static QVector<LayerDeclaration> extractDeclarations( const QgsProject *project );

    // Property coordinates, exposed so tests/tools can inspect the raw value.
    static QString scope() { return QStringLiteral( "paleo" ); }
    static QString key() { return QStringLiteral( "manifest" ); }
};
