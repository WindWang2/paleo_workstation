"""测试夹具：产出导入词表外类型（方向68——面板须诚实禁用导入）。"""
import json
import os
import sys

out_dir = sys.argv[1] if len(sys.argv) > 1 else os.getcwd()
with open(os.path.join(out_dir, "result.xyz"), "w", encoding="utf-8") as fh:
    fh.write("0 0 0\n")
print(json.dumps({"paleo": "1", "type": "result", "path": "result.xyz",
                  "kind": "xyz"}), flush=True)
