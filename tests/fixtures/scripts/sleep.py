"""测试夹具：可被取消/超时的长睡眠（方向68）。argv[1] = 秒数。"""
import sys
import time

seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 30.0
print("SLEEP_BEGIN", flush=True)
time.sleep(seconds)
print("SLEEP_END", flush=True)
