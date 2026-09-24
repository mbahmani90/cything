# FreeRTOS tasks

Every task the firmware creates with `xTaskCreate()`, with its stack size,
priority, where it is started and how long it lives. All stack sizes and
priorities are defined in one place, [src/common/task_config.h](../src/common/task_config.h),
as `<TASK>_TASK_STACK_SIZE` / `<TASK>_TASK_PRIORITY` — change them there, not at
the `xTaskCreate` call. All tasks are created
without core affinity (`xTaskCreate`, not `...PinnedToCore`), so the scheduler
may run them on either core. Stack sizes are **bytes** (ESP-IDF semantics, not
FreeRTOS words). Higher priority number = higher priority; `0` is the idle
priority.

## Active tasks

| Task function | Task name | Stack (bytes) | Prio | Created from | Parameter | Lifetime |
|---|---|---|---|---|---|---|
| [`wifi_init_sta`](../src/wifi/station_mode.c) | `wifi_init_sta_task` | 4096 (`WIFI_STA_TASK_STACK_SIZE`) | 5 | [cything.c:73](../src/cything.c:73) — STA mode boot | `NULL` | one-shot: brings up Wi-Fi station, waits for connect, starts `aws_iot_task`, then `vTaskDelete` |
| [`tcp_server_task`](../src/tcp_server/tcp_server.c) | `tcp_server` | 4096 (`TCP_SERVER_TASK_STACK_SIZE`) | 5 | [cything.c:82](../src/cything.c:82) (AP mode) · [wifi/station_mode.c](../src/wifi/station_mode.c) (STA got-IP) · [wifi/station_mode.c](../src/wifi/station_mode.c) (static-IP variant) | `AF_INET` | forever: listen/accept loop — see [tcp-server.md](tcp-server.md) |
| [`tcp_client_recv_task`](../src/tcp_server/tcp_client_recv.c) | `tcp_client_recv` | 10240 (`TCP_SERVER_RECV_TASK_STACK_SIZE`) | 5 | [tcp_server.c](../src/tcp_server/tcp_server.c) — once per accepted TCP client | `account_struct *` | per-connection: exits when the client disconnects or `recv` errors |
| [`tcp_response_send_task`](../src/tcp_server/tcp_client_list.c) | `tcp_response_send` | 3072 (`TCP_RESPONSE_SEND_TASK_STACK_SIZE`) | 5 | [cything.c:54](../src/cything.c:54) — at boot, before Wi-Fi | `NULL` | forever: blocks on `tcp_response_fifo`, sends each drained batch to every TCP client — see [send-buffers.md](send-buffers.md) |
| [`udp_server_task`](../src/udp_socket/udp_server.c) | `udp_server` | 4096 (`UDP_SERVER_TASK_STACK_SIZE`) | 5 | [cything.c:81](../src/cything.c:81) (AP mode) · [wifi/station_mode.c](../src/wifi/station_mode.c) (STA got-IP) · [wifi/station_mode.c](../src/wifi/station_mode.c) (static-IP variant) | `AF_INET` | forever: multicast/unicast discovery (`GET_INFO`) |
| [`aws_iot_task`](../src/aws/aws_iot_handler.c) | `aws_iot_task` | 4096 (`AWS_IOT_TASK_STACK_SIZE`) | **0** | [wifi/station_mode.c](../src/wifi/station_mode.c) — after STA connect, only if provisioned and `IS_REMOTE_CON_ENABLE` | `NULL` | forever: MQTT connect / subscribe / process loop with reconnect; drains `mqtt_response_fifo` every `mqtt_response_get_period_ms()` — see [send-buffers.md](send-buffers.md) |
| [`ota_task`](../src/ota_lib/http_ota_handler.c:6) | `ota_task` | 8192 (`OTA_TASK_STACK_SIZE`) | 5 | [tcp_command.c](../src/tcp_server/tcp_command.c) — on `updatingcomm<url>` | `sock` (by value, cast to `void *`) | one-shot: HTTPS OTA download, progress on the TCP socket, then `esp_restart` |
| `host_task` ([ble/ble_pairing.c](../src/ble/ble_pairing.c)) | `nimble_host` | 4096 (`CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE`) | 21 (NimBLE's own, `nimble_port_freertos_init`) | [cything.c](../src/cything.c) — AP (pairing) mode only, via `ble_pairing_start()` | `NULL` | forever: `nimble_port_run()` — every GAP/GATT callback runs here — see [ble-pairing.md](ble-pairing.md) |
| [`ble_commit_task`](../src/ble/ble_pairing.c) | `ble_commit` | 4096 (`BLE_COMMIT_TASK_STACK_SIZE`) | 5 | [ble/ble_pairing.c](../src/ble/ble_pairing.c) — on Control = COMMIT | `NULL` | one-shot: `wifi_trial_run()` (APSTA credential trial, ≤ 15 s), Status `04`, 1.5 s read window, `pairing_commit()` (flash write + arm reboot), final Status notify, `vTaskDelete` |

The `main` task that runs `app_main()` (which just calls `cything_begin()`, [cything.c](../src/cything.c)) is created by ESP-IDF itself:
`CONFIG_ESP_MAIN_TASK_STACK_SIZE=3584`, priority 1, pinned to CPU0
([sdkconfig](../sdkconfig)). It returns after spawning the tasks above.

### Configuration

All values live in [src/common/task_config.h](../src/common/task_config.h):

| Task | Stack macro | Priority macro |
|---|---|---|
| `wifi_init_sta` | `WIFI_STA_TASK_STACK_SIZE` | `WIFI_STA_TASK_PRIORITY` |
| `tcp_server_task` | `TCP_SERVER_TASK_STACK_SIZE` | `TCP_SERVER_TASK_PRIORITY` |
| `tcp_client_recv_task` | `TCP_SERVER_RECV_TASK_STACK_SIZE` | `TCP_SERVER_RECV_TASK_PRIORITY` |
| `tcp_response_send_task` | `TCP_RESPONSE_SEND_TASK_STACK_SIZE` | `TCP_RESPONSE_SEND_TASK_PRIORITY` |
| `udp_server_task` | `UDP_SERVER_TASK_STACK_SIZE` | `UDP_SERVER_TASK_PRIORITY` |
| `aws_iot_task` | `AWS_IOT_TASK_STACK_SIZE` | `AWS_IOT_TASK_PRIORITY` |
| `ota_task` | `OTA_TASK_STACK_SIZE` | `OTA_TASK_PRIORITY` |
| `ble_commit_task` | `BLE_COMMIT_TASK_STACK_SIZE` | `BLE_COMMIT_TASK_PRIORITY` |

The earlier scattered macros (`*_STACK_DEPTH` in the old `constants.h` / `TcpServer.h`,
including the never-used `AWS_IOT_STACK_DEPTH 20480`) and the bare literals at
the `xTaskCreate` calls are gone.

## Commented-out / removed tasks

Kept here so nobody re-adds them by accident without checking the history:

| Task | Where | Note |
|---|---|---|
| `check_remote_server_status` (`http_test`, 4096, prio 5) | [wifi/station_mode.c](../src/wifi/station_mode.c) | remote reachability probe |
| `pingMainTask` (`mainTask`, `configMINIMAL_STACK_SIZE`, prio 2) | [wifi/station_mode.c](../src/wifi/station_mode.c) | ping test |
| `ota_task` from MQTT | [aws/aws_iot_handler.c](../src/aws/aws_iot_handler.c) | OTA is triggered over TCP only |

## Observations

- **`aws_iot_task` runs at priority 0 with a 4096-byte stack.** Priority 0 is
  the idle priority, so it competes with the idle task and is starved by every
  other task in the system while they are runnable. 4096 bytes is also small
  for coreMQTT + mbedTLS; the old, never-used `AWS_IOT_STACK_DEPTH 20480`
  looked like the intended value. Both knobs are now `AWS_IOT_TASK_STACK_SIZE`
  / `AWS_IOT_TASK_PRIORITY` in `task_config.h`, unchanged for the moment.
- **All application tasks share priority 5** (except `aws_iot_task`), so
  scheduling between the TCP listener, the per-client receivers, the UDP server
  and the one-shot helpers is purely round-robin by tick (`CONFIG_FREERTOS_HZ=100`).
- **Per-client TCP tasks are unbounded in number** up to the 100-client
  registry cap, each costing 4096 bytes of stack plus a TCB.
- **Delayed reboots don't use tasks.** Pairing `FinishP`, provisioning
  `PFIN` and the end of `ota_task` all call
  [`reboot_after_ms()`](../src/common/reset_handler.c), an `esp_timer` one-shot
  that calls `esp_restart()` from the esp_timer task (the former
  `pairing_esp_restart` / `finish_provisioning_task` tasks are gone).
- **`wifi_init_sta` and `udp_server_task` were reduced from 20480 to 4096**
  (estimated peak ~2 KB each; see the reasoning in the commit that changed
  them). Both log `uxTaskGetStackHighWaterMark` — `wifi_init_sta` once just
  before it exits, `udp_server_task` after every processed datagram — under
  `WIFI_STA_DB` / `UDP_DB`. Check the monitor after flashing: a "free" figure
  below ~1000 bytes means the size should go back up. A genuine overflow
  panics with `A stack overflow in task <name> has been detected`.
- **`tcp_client_recv_task` is 10240** because `rx_buffer[TCP_RECEIVE_DATA_LENGTH]`
  (5120) is a stack local and the deepest handler (provisioning / mbedTLS)
  adds ~3.5 KB. It logs its high-water mark after every processed line under
  `TCP_SERVER_DB`; a `CSRREQ` exercises the worst case. Each connected client
  costs one such stack, so ~5–7 clients is the practical ceiling on a
  no-PSRAM ESP32.

## Software timers

Not tasks, but they run callbacks on a timer task (FreeRTOS timer service or `esp_timer`):

| Timer | Created | Period | Callback |
|---|---|---|---|
| `reboot` (`esp_timer`, [common/reset_handler.c](../src/common/reset_handler.c)) | lazily by `reboot_after_ms()` | one-shot, caller-supplied (1 s after pairing `FinishP` / provisioning `PFIN`, 2 s after OTA `updatedres`) | `esp_restart()`. Runs on the esp_timer task (`CONFIG_ESP_TIMER_TASK_STACK_SIZE`), so keep it to the restart — no logging or flash writes there. |
| `pairing` ([tcp_server/pairing.c](../src/tcp_server/pairing.c)) | `pairing_timer_init()` from [cything.c](../src/cything.c) | one-shot, `PAIRING_TIMEOUT_MS` (6 s, [wifi/wifi_config.h](../src/wifi/wifi_config.h)) | resets `pairing_step` to `START_PAIRING_IND`. Re-armed by `pairing_touch()` on every accepted pairing step, stopped by `pairing_done()` on `FinishP`. Replaced the old `pairing_time_out_handler` task, whose loop `break`-ed after one iteration and never fired. |
