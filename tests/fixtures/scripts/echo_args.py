"""测试夹具：回显 argv（方向68 scriptrunner 单测）。"""
import sys

print("ARGS_BEGIN")
for arg in sys.argv[1:]:
    print("ARG:" + arg)
print("ARGS_END")
