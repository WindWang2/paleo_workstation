"""测试夹具：JSON 行协议三型 + 产出文件落盘（方向68）。

ensure_ascii 保持默认 True：协议行在任何平台/locale 下都是纯 ASCII，
避免 Windows 遗留代码页下的 stdout 编码失败。
"""
import json
import os
import sys


def emit(**fields):
    fields["paleo"] = "1"
    print(json.dumps(fields), flush=True)


out_dir = sys.argv[1] if len(sys.argv) > 1 else os.getcwd()
os.makedirs(out_dir, exist_ok=True)

emit(type="progress", percent=0, message="start")
geojson_path = os.path.join(out_dir, "result.geojson")
with open(geojson_path, "w", encoding="utf-8") as fh:
    json.dump({"type": "FeatureCollection", "features": []}, fh)
emit(type="progress", percent=50, message="geojson written")
csv_path = os.path.join(out_dir, "result.csv")
with open(csv_path, "w", encoding="utf-8") as fh:
    fh.write("x,y\n1,2\n")
emit(type="result", path="result.geojson", kind="geojson", message="empty features")
emit(type="result", path="result.csv", kind="csv", message="two-column points")
print("PLAIN_NOT_PROTOCOL")
emit(type="progress", percent=100, message="done")
