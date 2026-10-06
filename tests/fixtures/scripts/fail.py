"""测试夹具：非零退出码（方向68）。"""
import sys

print("FAIL_STDOUT")
print("FAIL_STDERR", file=sys.stderr)
sys.exit(3)
