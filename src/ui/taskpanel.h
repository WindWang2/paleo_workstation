#pragma once
#include <QWidget>

class PaleoProjectStore;

// ui/ — TaskPanel: bottom-dock view of the layer-busy registry (§35 tool
// gating backend). A busy row = a running background task holding a layer;
// the panel polls the store registry so late-marked entries still show up.
class TaskPanel : public QWidget
{
  Q_OBJECT
  public:
    explicit TaskPanel(PaleoProjectStore *store, QWidget *parent = nullptr);
    void refresh(); // repopulate rows from store->busyLayers()

  private:
    PaleoProjectStore *m_store;
};
