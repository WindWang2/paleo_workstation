// 层：功能
#include "aichatcontroller.h"
#include "pythonenvironmentcontroller.h"
#include "services/pythonenvironmentregistry.h"
#include <QTimer>

PythonEnvironmentController *AiChatController::createPythonEnvironmentController(QObject *owner) {
  auto registry = m_runner.pythonEnvironments();
  if (!registry) {
    registry = std::make_shared<paleo::python::PythonEnvironmentRegistry>();
    m_runner.setPythonEnvironments(registry);
  }
  auto *controller = new PythonEnvironmentController(registry, m_runner.taskService(), owner);
  QTimer::singleShot(0, controller, &PythonEnvironmentController::reload);
  return controller;
}
