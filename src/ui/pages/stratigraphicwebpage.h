// 层：视图
#pragma once
#include <QWidget>

class QLabel;
class QLineEdit;
class SARibbonCategory;
class StratigraphicWebSession;
class WebViewPanel;

class StratigraphicWebPage : public QWidget
{
  Q_OBJECT
public:
  explicit StratigraphicWebPage(StratigraphicWebSession *session, QWidget *parent = nullptr);
  void buildRibbon(SARibbonCategory *category);
  void activate();
private:
  StratigraphicWebSession *m_session;
  WebViewPanel *m_web;
  QLabel *m_status;
  QLineEdit *m_address = nullptr;
  bool m_activated = false;
};
