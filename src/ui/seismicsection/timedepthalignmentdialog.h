// 层：视图
#pragma once
#include <QDialog>
#include <QVariantMap>
class QListWidget;
class QComboBox;
class QLineEdit;
class QLabel;
class QPushButton;
class TimeDepthAlignmentDialog : public QDialog {
  Q_OBJECT
public:
  explicit TimeDepthAlignmentDialog(const QVariantMap &options,QWidget *parent=nullptr);
  void setMessage(const QString &message);
signals:
  void saveRequested(const QVariantMap &selection);
private:
  void updateSave();
  QListWidget *m_sources;
  QComboBox *m_unit,*m_type,*m_sign,*m_volume;
  QLineEdit *m_reference;
  QLabel *m_message;
  QPushButton *m_save;
  bool m_writable=false;
};
