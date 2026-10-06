"""测试夹具：stdout/stderr 双流交错输出（方向68）。"""
import sys
import time

for i in range(3):
    print("OUT_LINE_%d" % i, flush=True)
    time.sleep(0.05)
    print("ERR_LINE_%d" % i, file=sys.stderr, flush=True)
    time.sleep(0.05)
