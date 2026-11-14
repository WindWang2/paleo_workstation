# ONNX 首输出预算补口

Tile v2 原先仅用 manifest 声明 classes 检查输出预算，真实输出通道数直到 ORT Run
并复制到 QVector 后才检查。小型 512 通道模型若误声明为两类，1024² tile 可先分配
约 2 GiB 输出并再次复制，超过已有 32M 元素预算。

`OnnxModelMeta` 现记录同一实际 Session 首输出的 shape/type。v2 prepare 和 tile
执行在 fetch 前要求 float32、四维、N=1、已知 C∈[1,255]，且 C 与声明相同；
现有模型字节钉版、特征/语义、指纹复核与 Legacy 推理接口保持原路径。
静态输出超过 32M 元素时跳过 warmup，并在显式 Run 前拒绝；所有实际首输出均在
QVector resize/memcpy 前检查 32M 硬上限。这不是对动态 graph 内部申请的通用沙箱。

独立真实 `paleo_ai_onnx` 编译/链接后，串行 CTest 三套 3/3（0.51s）：

- `tst_pinned_inference` 12/12：真实 512/3 通道但声明两类，以及真实未知 C 均在
  1024² 请求 fetch 次数为零时拒绝。动态 H/W 仅以 1×1 warmup，无 GiB 分配。
  另以静态 64×1024² 输出验证 warmup 跳过和显式 Run 预算拒绝；输入仅 4 MiB。
- `tst_modelregistry` 20/20、`tst_modelcontractquery` 9/9 保持通过。

合计 41 通过（含 fixture），0 失败、0 跳过。完整 Source gates 32/32，含锁版 QGIS
传递依赖；没有新增/提高基线。新增 `tst_tile_operation` prepare 场景由累计树的真实
目标执行，本独立树未为该目标另开服务/SDK 全量构建。
