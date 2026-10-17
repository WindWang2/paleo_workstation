"""测试夹具：输出非 ASCII（中文）行（#479 子进程 IO 统一 UTF-8）。"""
import sys

sys.stdout.write("中文-数据-管线\n")
sys.stdout.flush()
