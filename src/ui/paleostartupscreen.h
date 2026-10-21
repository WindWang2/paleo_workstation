// 层：视图
#pragma once
#include <QWidget>

class QLabel;

// Startup presentation only. The composition root supplies real loading stages.
class PaleoStartupScreen : public QWidget
{
  Q_OBJECT
public:
  explicit PaleoStartupScreen();
  void setStage(const QString &text);

private:
  QLabel *m_stage = nullptr;
};
