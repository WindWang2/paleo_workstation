// 层：功能
#include "remotepredictionservice.h"

// 方向51：Mock 实现已迁到 tests/mockremotepredictionservice.h。本翻译单元
// 保留两件事：① vtable/元数据锚点（接口析构函数在这里实例化，避免每个
// 派生物的每个 TU 各生成一份）；② 让 AUTOMOC 在本模块里稳定产出
// moc_remotepredictionservice.cpp（头文件含有 Q_OBJECT，须有同目录源文件
// 把它带进 target 的 automoc 扫描面）。
RemotePredictionService::~RemotePredictionService() = default;
