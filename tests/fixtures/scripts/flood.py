"""测试夹具：输出无换行的巨量数据（#479 半行缓冲上限）。"""
import sys

sys.stdout.write("X" * (2 * 1024 * 1024))
sys.stdout.flush()
