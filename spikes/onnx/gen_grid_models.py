#!/usr/bin/env python3
"""Dev-only generator for spikes/onnx grid fixtures. NOT shipped product code.

Emits two variants of the toy semantics (y = x + 40.0) with a fixed-size 2D
output so the prediction workflow's D61-grid gate can be exercised:

  grid.onnx     — x : float32[1] -> y : float32[411, 641]  (the project-area grid)
  tile2x2.onnx  — x : float32[1] -> y : float32[2, 2]      (wrong-size refusal)

Deterministic I/O: input 2.0 -> output 42.0 in every cell.
"""
import onnx
from onnx import helper, TensorProto


def make(rows, cols, path):
    shape_t = helper.make_tensor("outshape", TensorProto.INT64, [2], [rows, cols])
    forty = helper.make_tensor("forty", TensorProto.FLOAT, [], [40.0])
    expand = helper.make_node("Expand", inputs=["x", "outshape"], outputs=["e"], name="expand")
    add = helper.make_node("Add", inputs=["e", "forty"], outputs=["y"], name="add_forty")
    graph = helper.make_graph(
        [expand, add],
        "expand_add_forty",
        inputs=[helper.make_tensor_value_info("x", TensorProto.FLOAT, [1])],
        outputs=[helper.make_tensor_value_info("y", TensorProto.FLOAT, [rows, cols])],
        initializer=[shape_t, forty],
    )
    model = helper.make_model(
        graph,
        opset_imports=[helper.make_opsetid("", 13)],
        producer_name="paleo-spike",
    )
    model.ir_version = 8  # conservative; ORT 1.30 accepts IR <= 12
    onnx.checker.check_model(model)
    onnx.save(model, path)
    print("wrote", path)


make(411, 641, "grid.onnx")
make(2, 2, "tile2x2.onnx")
