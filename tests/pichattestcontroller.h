#pragma once
#include "services/paleotaskservice.h"
#include "workflow/aichatcontroller.h"

// Supply the task service normally wired by the application. Model and tool
// requests still traverse the packaged Pi runtime and the production bridge.
class PiTestChatController : public AiChatController {
public:
  explicit PiTestChatController(QObject *parent = nullptr) : AiChatController(parent) {
    toolRunner()->setTaskService(new PaleoTaskService(nullptr, this));
  }
};
