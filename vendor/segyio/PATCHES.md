# vendor/segyio 本地补丁清单

基线：segyio **v2.0.0-alpha.1**（`CMakeLists.txt` 中 `paleo_segyio`）。下列为有意分叉，
升级上游时需逐条核对是否已被上游修复，未修复则重放。源码内以 `paleo local patch`
注释标出。

## S1 · `src/segy.c` — `trace_bsize` / `segy_trsize` 整数溢出

- `segy_collect_metadata`：`samplecount` 可来自 rev2 扩展字段（int32），
  `samplecount * elemsize` 在 int 上溢出属于 UB（审计 03 D3，libFuzzer+UBSan 复现：
  `segy.c:1120: signed integer overflow: 1289994496 * 4`），回绕后的 trace 尺寸
  继续参与 `segy_traces`/`segy_readsubtr` 的偏移与长度计算。改为 64 位求积，
  `samplecount < 0` 或 `bsize > INT_MAX - SEGY_TRACE_HEADER_SIZE` 时返回
  `SEGY_INVALID_FIELD_VALUE`。
- `segy_trsize`：同样 64 位求积，溢出返回 -1（与格式非法时同一错误约定）。
- 回归：`tests/tst_seismic_core.cpp::segyioRejectsOverflowingTraceSize`。
