## 问题

linux-asan 车道结构性磁盘耗尽：构建到 2,231 个链接目标时 runner 磁盘归零，链接直接失败，**ASAN/UBSAN 车道长期给不出任何结论**。

## 证据

run 38835970978（linux-asan job）：

```
##[warning]You are running out of disk space. The runner will stop working when the machine runs out of disk space. Free space left: 27 MB
FAILED: [code=1] tst_wellcomposite
/usr/bin/x86_64-linux-gnu-ld.bfd: final link failed: No space left on device
collect2: error: ld returned 1 exit status
FAILED: [code=1] tst_smoke_realdata
```

- 全部目标在 `-fsanitize=address -fsanitize=undefined` 下链接，共 2,231 个。
- 非偶发：10-15 的 run 38756099683 linux-asan 同为 `failure`。

## 影响

ASAN/UBSAN 是唯一能捕获堆越界读、生命周期与未定义行为的车道——#460（tileinference 张量越界读）、#467（DLIS/LIS 无界累积）、#468（SGS 组合内存闸）这类缺陷都靠它兜底。该车道无结论 = 这类缺陷的检测形同虚设。且与 #484 记录的「continue-on-error job 静默腐烂」叠加，失败更容易被忽略。

## 建议

- 缩减 asan 车道目标集：只链受影响的 label 子集（如 `-L 'core|io|algorithms'`），或拆成多个 job 分片。
- 降链接期体积：`-gsplit-dwarf`、换 lld、构建后清理非 asan 产物。
- CI 侧加磁盘水位检查：低于阈值时**早失败并报明确原因**（当前报的是 ld 错误，容易被误判为代码问题）。
- 若 continue-on-error 仍在，至少加「连续 N 次失败即转红」的晋升检查。

## 验证

linux-asan job 能完整跑完并给出测试结论（而非链接失败）。

## 关联

父项：本批 A；相关：#484（continue-on-error 腐烂）
