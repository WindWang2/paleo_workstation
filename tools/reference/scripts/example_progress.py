#!/usr/bin/env python3
"""方向68 示例脚本：JSON 行协议演示。

演示 progress/result 两型；在工作目录产出一个 GeoJSON 点要素集
（演示数据，非真实工区）。用法：

    python3 example_progress.py [输出目录]
"""
import json
import os
import sys
import time


def paleo(**fields):
    fields["paleo"] = "1"
    print(json.dumps(fields), file=sys.stdout, flush=True)


def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else os.getcwd()
    os.makedirs(out_dir, exist_ok=True)
    paleo(type="progress", percent=0, message="开始生成演示点集")

    features = []
    for i in range(5):
        time.sleep(0.2)  # 演示：假装在算
        features.append({
            "type": "Feature",
            "geometry": {"type": "Point", "coordinates": [110.0 + i * 0.1, 38.0 + i * 0.1]},
            "properties": {"name": "demo-%d" % i},
        })
        paleo(type="progress", percent=(i + 1) * 18, message="生成点 %d/5" % (i + 1))

    out_path = os.path.join(out_dir, "demo_points.geojson")
    with open(out_path, "w", encoding="utf-8") as fh:
        json.dump({"type": "FeatureCollection", "features": features}, fh,
                  ensure_ascii=False, indent=1)
    paleo(type="result", path="demo_points.geojson", kind="geojson",
          message="5 个演示点")
    paleo(type="progress", percent=100, message="完成")
    print("这一行不是协议，按纯文本呈现在控制台")


if __name__ == "__main__":
    main()
