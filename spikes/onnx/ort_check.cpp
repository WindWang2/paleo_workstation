// Paleo Workbench — Phase 0 spike 3 (ET4): ONNX Runtime in-process inference.
//
// Loads vendor/onnxruntime (official linux-x64 binary release), runs a toy
// graph (y = x + 40.0, float32[1]) in-process via the ORT C++ API, prints
// input/output, and exits non-zero unless the result is deterministic
// (2.0 -> 42.0). This is spike code: single TU, no error-pageantry.
//
// Build: ./build.sh   Run: ./ort_check

#include <cmath>
#include <cstdio>
#include <vector>

#include <onnxruntime_cxx_api.h>

int main(int argc, char** argv) {
  const char* model_path = (argc > 1) ? argv[1] : "toy.onnx";

  Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "ort_check");
  Ort::SessionOptions session_options;
  session_options.SetIntraOpNumThreads(1);
  session_options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_BASIC);

  std::printf("[ort_check] api version: %s\n",
              OrtGetApiBase()->GetVersionString());
  std::printf("[ort_check] model: %s\n", model_path);

  Ort::Session session(env, model_path, session_options);
  Ort::AllocatorWithDefaultOptions allocator;

  // -- Introspect: single float input, single float output -------------------
  const size_t num_inputs = session.GetInputCount();
  const size_t num_outputs = session.GetOutputCount();
  std::printf("[ort_check] inputs=%zu outputs=%zu\n", num_inputs, num_outputs);
  if (num_inputs != 1 || num_outputs != 1) {
    std::fprintf(stderr, "[ort_check] FAIL: expected 1 in / 1 out\n");
    return 2;
  }

  auto input_name = session.GetInputNameAllocated(0, allocator);
  auto output_name = session.GetOutputNameAllocated(0, allocator);
  std::printf("[ort_check] input[0]=\"%s\" output[0]=\"%s\"\n",
              input_name.get(), output_name.get());

  auto input_type_info = session.GetInputTypeInfo(0);
  auto input_tensor_info = input_type_info.GetTensorTypeAndShapeInfo();
  const std::vector<int64_t> input_shape = input_tensor_info.GetShape();
  const size_t element_count = input_tensor_info.GetElementCount();
  if (input_tensor_info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
    std::fprintf(stderr, "[ort_check] FAIL: expected float32 input\n");
    return 2;
  }

  // -- Feed 2.0, expect 42.0 -------------------------------------------------
  std::vector<float> input_values(element_count, 2.0f);
  Ort::MemoryInfo memory_info =
      Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
      memory_info, input_values.data(), element_count,
      input_shape.data(), input_shape.size());

  const char* input_names[] = {input_name.get()};
  const char* output_names[] = {output_name.get()};

  std::printf("[ort_check] running inference, x=%.1f ...\n", input_values[0]);
  auto output_tensors = session.Run(
      Ort::RunOptions{nullptr}, input_names, &input_tensor, 1,
      output_names, 1);

  float* out = output_tensors[0].GetTensorMutableData<float>();
  const float y = out[0];
  std::printf("[ort_check] y = %.1f (expected 42.0)\n", y);

  constexpr float kExpected = 42.0f;
  if (!std::isfinite(y) || std::fabs(y - kExpected) > 1e-5f) {
    std::fprintf(stderr, "[ort_check] FAIL: y=%f, expected %.1f\n",
                 y, kExpected);
    return 1;
  }

  std::printf("[ort_check] PASS: in-process inference deterministic\n");
  return 0;
}
