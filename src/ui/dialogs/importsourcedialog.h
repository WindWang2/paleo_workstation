// 层：视图
#pragma once
#include <QDialog>
#include <QStringList>
class QComboBox;
class QLineEdit;
class QListWidget;
class QLabel;
class QPushButton;
class QStackedWidget;

class ImportSourceDialog : public QDialog
{
  Q_OBJECT
public:
  explicit ImportSourceDialog(const QString &kind, QWidget *parent = nullptr);
  QStringList files() const;
  QString folder() const;
  QString fileFilter() const;
  static QString preferredType(const QString &kind, const QString &path, const QString &detected);
  void setSources(const QStringList &paths, bool directory = false);
  void accept() override;
private:
  void refresh();
  QString m_kind;
  QComboBox *m_mode;
  QListWidget *m_files;
  QLineEdit *m_folder;
  QStackedWidget *m_pages;
  QLabel *m_error, *m_count;
  QPushButton *m_confirm;
};
