<p align="right">
  <strong>简体中文</strong> · <a href="wifi-csi-sensing.md">English</a>
</p>

# ECHO —— Wi-Fi CSI 感知模式

在 `feature/wifi-csi-sensing` 分支上新增的被动 Wi-Fi 信道状态信息(CSI)感知模式,
作为菜单第七项("WiFi CSI")。这是良性传感应用:设备接收数据包、从信道响应推算运动
分数,不干扰、不攻击、不解密他人流量。

## 它做什么

- 作为 2.4 GHz Wi-Fi STA 连接用户自己的路由器,按固定频率 ping 网关,使每个回包产生
  一条 CSI 记录(只收不发射干扰)。
- 在设备上计算每子载波幅度与基于 EWMA 的运动分数。
- 把每条 CSI 记录以 CSV 行从 USB-Serial-JTAG 控制台串流出来,供主机分析。
- 暴露 BLE GATT 外设,让手机或浏览器在 Wi-Fi 运行的同时查看实时状态并下发控制命令
  (ESP32-C3 用一颗 2.4 GHz 射频在 Wi-Fi 与 BLE 间分时;已启用软件共存)。
- 可选对 AP 做 FTM(精细测距)。

## 源文件

| 文件 | 职责 |
| --- | --- |
| `main/demo_csi.c` / `.h` | 模式本体:LVGL 屏、后台 task、CSI 回调、命令循环 |
| `main/csi_metric.c` / `.h` | 纯 C(零 ESP-IDF):幅度换算 + EWMA 运动指标 |
| `main/csi_proto.c` / `.h` | 纯 C:构造 STATUS JSON、解析 CONTROL JSON 命令 |
| `main/csi_ble.c` / `.h` | NimBLE GATT 外设(STATUS 通知 + CONTROL 写) |
| `main/demo_radio.c` / `.h` | NVS / netif / 事件准备,以及阻塞式 STA 连接 helper |
| `main/Kconfig.projbuild` | CSI 凭证与时序配置项 |

`csi_metric` 与 `csi_proto` 由 host 测试覆盖(`tests/test_csi_metric.c`、
`tests/test_csi_proto.c`),纳入 `tools/validate.sh --static`。

## 配置(menuconfig)

凭证绝不入库,请在本地 `sdkconfig` 填写:

- `CSI_WIFI_SSID`、`CSI_WIFI_PASSWORD` —— 用户自己的网络(默认空;为空时屏显
  "No WiFi: set in menuconfig")。
- `CSI_PING_INTERVAL_MS` —— ping 间隔(默认 100)。
- `CSI_PING_TARGET` —— ping 目标(默认 DHCP 网关)。
- `CSI_FTM_AUTO_INTERVAL_S` —— 自动 FTM 周期秒数(默认 0 = 关)。

## 串口 CSV 契约

USB-Serial-JTAG 控制台上每条 CSI 记录一行:

```
CSI_DATA,<rx_seq>,<timestamp_us>,<rssi>,<rate>,<noise_floor>,<channel>,<motion_0_100>,<csi_len>,[b0 b1 ... b(len-1)]
```

头部逗号分隔;末尾方括号内是 `csi_len` 个空格分隔的 `int8`,按子载波交织的
`(imag, real)` 对(子载波数 = `csi_len / 2`;幅度 = `sqrt(real^2 + imag^2)`)。

## BLE GATT 契约

广播名:`AIPassport-CSI`

- 感知服务:`e2e90001-8f2a-4c7b-9f3d-1a2b3c4d5e6f`
- STATUS(Read + Notify)`e2e90002-...`:约 3 Hz 的 UTF-8 JSON,字段 `st`
  (0 关 / 1 连接中 / 2 运行 / 3 失败)、`ssid`、`rssi`、`mot`(0-100)、
  `rate`(CSI 包/秒)、`ftm`(距离 cm,0=无效)、`fv`(FTM 有效 0/1)。
- CONTROL(Write / Write Without Response)`e2e90003-...`:UTF-8 JSON 命令
  `{"cmd":"start"}`、`{"cmd":"stop"}`、`{"cmd":"ftm"}`、`{"cmd":"ping_ms","v":100}`
  (v 20-2000)。命令驱动采集启停、FTM 与 ping 间隔。

## FTM 说明

ESP32-C3 是 FTM 发起端。要得到距离估计,需要一个声明 FTM responder 能力的 AP;
多数家用路由器不支持。AP 非 responder(或会话失败)时优雅降级:`fv` 保持 0,屏显
"Dist: N/A"。

## 配套客户端

跨平台客户端(Python 主机工具、Web Bluetooth 面板、iOS app)位于 umbrella 项目的
`companion/` 目录,共用上面两份契约。

## 验证状态

- **Build**:PASS —— ESP-IDF 5.5.3,app 约占 3 MB 分区的 1.19 MB;分区保护与合并
  镜像已校验。
- **Host tests**:PASS —— `csi_metric` 与 `csi_proto`。
- **Device tests**:NOT RUN(无真机)。

### 仍需真机验证

1. Wi-Fi + BLE 共存下的 RAM 余量(无 PSRAM);链接已过,但堆占用是运行期指标。
2. 对支持 FTM responder 的 AP 的实测距离。
3. 反复进出稳定性、共存下的 CSI 包速率,以及完整 BLE 链路(连接、订阅、通知、
   CONTROL 命令)与客户端联调。
4. 运动灵敏度标定(`alpha` / `scale` 为经验默认值)。
