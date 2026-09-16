# AD 2.1 时序矩阵 — 真机操作表

配置：`status_update` 开；`problem_classes=["Red","Red-green"]`；conf≥**0.3**；problem=3.0s；recovery=2.0s；cooldown=5.0s；zone=cam-001 全帧。
监控：`output/rule_lights/monitor.log` 中 `<<< RULE kind=status_changed/problem|recovered`。
画面：http://192.168.50.45:8080/webrtc-test-2.html （只开 cam-001）。

| ID | 你怎么操作灯 | 预期监控 | 结果 |
|---|---|---|---|
| AD-01 | 先灭红/切绿 2s 以上；再**持续红灯 ≥3.0s**（可多次） | ~3.0s 出现 `status_changed/problem` |  |
| AD-02 | 红灯亮约 **2.5–2.8s 后立刻灭/切绿** | **不**出现 problem |  |
| AD-03 | 红 1.5s → 绿 1.5s → 红 1.5s 循环 ≥3 轮 | **永不** problem |  |
| AD-04 | 先做成 AD-01 problem；再**无红 ≥2.0s**（灭或绿） | `status_changed/recovered` |  |
| AD-05 | problem 后灭红约 1.5–1.8s **再亮红** | 保持 problem，**不**发 recovered |  |
| AD-06 | problem 后只亮绿（无红）≥2s | 照常 recovered（绿不参与判定） |  |
| AD-07 | 红+绿同时（或模型 `Red-green`）持续 | 保持/触发 problem |  |
| AD-08 | problem 后画面里仍有另一个红 | **不** recovered |  |
| AD-16 | problem 后灯全灭（空检测）≥2s | 空帧推进恢复 → recovered |  |
