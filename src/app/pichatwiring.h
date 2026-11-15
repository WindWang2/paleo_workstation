// 层：组装根
#pragma once
#include "services/piruntime.h"

// Only the explicit host Pi opt-in and complete root/SHA pair. Model keys and
// project/operation arguments never participate in optional engine selection.
paleo::pi::RuntimeConfig capturePiChatRuntime();
