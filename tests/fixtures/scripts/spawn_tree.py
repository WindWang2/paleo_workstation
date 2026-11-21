# 进程树夹具（#960 回归）：父/子/孙三段真实进程，跨 Unix/Windows。
# 用法：spawn_tree.py <sentinel> <mode>
#   parent —— 启动 child，等 child 写好 sentinel（含 child/grand pid）后
#             立即退出：主进程先退、后代仍存活的窗口。
#   hold   —— 同 parent，但父进程继续睡眠：供取消/超时/析构用例。
#   child  —— 启动 grand，写 sentinel，睡眠 120s。
#   grand  —— 睡眠 120s。
# 子孙继承 stdout 管道（默认继承），覆盖「后代持有输出管道」路径。
import os
import subprocess
import sys
import time

sentinel = sys.argv[1]
mode = sys.argv[2] if len(sys.argv) > 2 else "parent"

if mode in ("parent", "hold"):
    child = subprocess.Popen(
        [sys.executable, os.path.abspath(__file__), sentinel, "child"]
    )
    print("TREE_PARENT:%d" % os.getpid(), flush=True)
    print("TREE_CHILD:%d" % child.pid, flush=True)
    deadline = time.time() + 30
    while not os.path.exists(sentinel):
        if time.time() > deadline:
            sys.exit(9)
        time.sleep(0.05)
    if mode == "parent":
        sys.exit(0)
    time.sleep(120)
elif mode == "child":
    grand = subprocess.Popen(
        [sys.executable, os.path.abspath(__file__), sentinel, "grand"]
    )
    # 原子落盘：父进程一见 sentinel 即代表 pid 内容完整（避免半写被回收）。
    tmp = sentinel + ".tmp"
    with open(tmp, "w") as f:
        f.write("%d %d" % (os.getpid(), grand.pid))
    os.replace(tmp, sentinel)
    time.sleep(120)
else:
    time.sleep(120)
