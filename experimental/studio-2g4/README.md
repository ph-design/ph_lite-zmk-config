# EVO Studio over encrypted 2.4G

基线：`ph-design/zmks` commit `611132e871678a0aed9c1fd9258ec39033046b8e`；`ph-design/ph_lite-zmk-config` commit `08b45141e7d0f340d79e5ff8b20505667c46a94e`。已验证 Zephyr 3.5 / SDK 0.16.8 构建环境。

`zmk.patch` 包含所有已跟踪的核心改动；应用后复制 `studio-radio.h`、`studio-radio-link.h` 至 `app/include/`。`config.patch` 包含 AT / CMake 改动；将本目录 `studio-radio.c`、`dongle-led.c`、`led-*.h` 复制至 config 模块的 `src/`。共享 Studio 文件必须加入 app target，以取得生成的 protobuf include 依赖；其 CMake 路径应指向当前 config 模块的 `src/studio-radio.c`。

`evo.conf` 是已验证的附加配置。目标 `ph_lite`，shield `PH60-SC_V2_EVO`，snippet `studio-rpc-usb-uart`。其余 C 文件是源码快照；核心补丁是权威差异。

管理帧 type 0x40，含 open/close、32 位 session、双向 8 位序号及最多 12 字节数据，经已有 AES-CCM 包装为最多 32 字节 ESB payload。停止等待 ARQ 丢包重传使用新的 CCM counter，重复数据只递交一次。2Mbps ACK timeout 从 150µs 改为 350µs。`test-studio-radio.c` 覆盖丢包、重复、回绕和无效头。

共享实现：键盘接入原有 2.4G Studio RPC transport；接收器连接独立 CDC。物理解锁仍由键盘完成。实测临时改键、读回和恢复通过，没有对现有未保存配置执行 save/discard。

## 两把键盘的 Hub

当前接收器以相邻 `../wireless-hub/` 的 NCS 应用为准；这里的旧 ZMK 接收器快照只保留作旧协议 / 灯控补丁参考，不要覆盖到新 Hub。

第二把 EVO 额外使用本目录 `pairing.proto` 与 `pairing_subsystem.c`：替换 Studio 消息仓库 `proto/zmk/pairing.proto` 和 ZMK `app/src/studio/pairing_subsystem.c`，重新生成 protobuf 并编译。扩展增加 field 4 `set_channel`，用于生成独立配对后把两把键盘设置到同一合法频道；不改变无线管理帧。它仍受 Studio 物理解锁保护。旧核心 patch 不包含该频道扩展，必须单独应用这些快照。

第一把保留原无线 Studio 固件与配对；第二把已构建并刷入此扩展。不要在其他 ZMK 基线上盲目应用补丁；先核对版本与差异。实机验证与限制见 `../wireless-hub/VALIDATION.md`。

## 拿起锁定与状态回传

使用相邻 `../motion-guard/` 的 preview.5 叠加包。先应用本目录的核心、配置及频道扩展，再应用拿起锁定补丁；不要把旧 `2g4.c` 快照覆盖到叠加后的源码。最终接收器为 Hub 0.6.7，键盘状态通过加密心跳中的 A1 扩展回传，旧心跳兼容但状态为 UNKNOWN。


## 无线发送缓冲保护（preview.11 配套）

当前核心补丁包含整帧转义长度预检、有界 RPC 等待和非阻塞遥测发送。无线线程通过 zmk_rpc_radio_tx_read 在传输锁下核对管理端点归属，防止 USB 接管后取走 USB 回复。请同时更新 zmk.patch 与 studio-radio.c；不能只复制其中一个文件。相邻 motion-guard preview.11 继续叠加 USB 管理优先、独立通知队列、USB BL 和灯光渐变。新组合生成的 RPC 运行时代码已与三款实际编译／测试代码逐字对比一致；旧版本源码／二进制仅作为回退。

preview.12 继续修复 USB UART 回调：每次最多64字节，尊重0／部分／错误写入，等待 CDC 完成后继续；停止时禁用RX和TX，迟到回调不再读取无线缓冲。对应真实回调回归位于 ../motion-guard/test_uart_runtime.py。
