#!/usr/bin/env python3
"""Dev-only generator for spikes/onnx/toy.onnx. NOT shipped product code.

Emits a minimal model: y = x + 40.0  (x, y : float32[1])
Deterministic I/O for ort_check: input 2.0 -> output 42.0.
"""
import onnx
from onnx import helper, TensorProto

node = helper.make_node("Add", inputs=["x", "forty"], outputs=["y"], name="add_forty")
forty = helper.make_tensor("forty", TensorProto.FLOAT, [1], [40.0])
graph = helper.make_graph(
    [node],
    "toy_add_forty",
    inputs=[helper.make_tensor_value_info("x", TensorProto.FLOAT, [1])],
    outputs=[helper.make_tensor_value_info("y", TensorProto.FLOAT, [1])],
    initializer=[forty],
)
model = helper.make_model(
    graph,
    opset_imports=[helper.make_opsetid("", 13)],
    producer_name="paleo-spike",
)
model.ir_version = 8  # conservative; ORT 1.30 accepts IR <= 12
onnx.checker.check_model(model)
onnx.save(model, "toy.onnx")
print("wrote toy.onnx,", model.ByteSize() if hasattr(model, "ByteSize") else "?", "bytes proto")
