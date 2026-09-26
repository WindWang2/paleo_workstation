# testdata/onnx — D15 端到端推理冒烟夹具

`grid.onnx`（186 字节，由 `gen_grid_models.py` 生成，同 spikes/onnx）：
`x: float32[1] -> y: float32[411, 641]`，语义 `y = x + 40.0`（Expand+Add，
opset 13）。tst_onnxworkflow 的 D15 冒烟用例从本目录复制模型进临时
model root，经 PaleoOnnxService（真 vendored ORT 会话）→ PredictionWorkflow
→ 411×641 GeoTIFF 全链验证「差异化路径可用」，只断言尺寸/有限值比例，
不断言数值精度。
