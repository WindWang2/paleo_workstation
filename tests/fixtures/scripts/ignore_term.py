"""测试夹具：忽略 SIGTERM，验证 terminate→kill 递进（方向68）。"""
import signal
import time

if hasattr(signal, "SIGTERM"):
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
print("TERM_IGNORED", flush=True)
while True:
    time.sleep(0.2)
