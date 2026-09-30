// 层：视图
#pragma once
#include "../../io/lasdoc.h"
#include <QVariantList>
#include <QWidget>
class QComboBox;
class QTableWidget;
class QLabel;
class QPushButton;
namespace WellComposite {
class WellCompositeCanvas;
}
class WellPredictionPanel : public QWidget {
  Q_OBJECT
public:
  explicit WellPredictionPanel(QWidget *parent = nullptr);
  void setResult(const QString &id, const QVariantList &wells,
                 const QVariantList &schema);
  void setLog(const LasDoc &log);
  QString layerId() const { return m_layer; }
  void clear();
  void setUndoAvailable(bool available);

protected:
  void showEvent(QShowEvent *event) override;
signals:
  void logRequested(const QString &versionId);
  void reviseRequested(const QString &wellId, int interval, int code);
  void saveRequested();
  void undoRequested();
  void featureSelected(qint64 featureId);

private:
  void refreshWell();
  void rebuildTracks();
  QString m_layer;
  QVariantList m_wells, m_schema;
  LasDoc m_log;
  QComboBox *m_well, *m_facies;
  QTableWidget *m_intervals;
  QLabel *m_status;
  QPushButton *m_apply, *m_save, *m_undo;
  WellComposite::WellCompositeCanvas *m_canvas;
};
