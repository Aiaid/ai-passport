<p align="right">
  <strong>简体中文</strong> · <a href="wifi-csi-sensing.md">English</a>
</p>

# ECHO —— 被动 Wi-Fi 感知固件

ECHO 是跑在 `feature/wifi-csi-sensing` 分支上的被动 Wi-Fi 感知应用。宿主硬件仍是
FoloToy AI Passport(ESP32-C3);ECHO 是这份固件的应用/产品名。命名取自"回声定
位"——ECHO 读的是人体对 Wi-Fi 信号的反射,而不是声音。

它包含两个感知模式:

- **ECHO**——信道状态信息(Channel State Information,CSI)感知:在设备与所连
  接入点(AP)之间的链路上做自标定的占用/运动"绊线"检测,并支持可选的 FTM 测距。
- **RADAR**——被动设备发现:嗅探并粗略分类周边 Wi-Fi 设备,不连接任何网络。

两个模式都只接收。设备不会干扰、攻击或解密他人流量;RADAR 也不会关联任何网络。

## 硬件

ESP32-C3,单天线(1T1R),2.4 GHz,HT20,约 64 子载波;240×320 ST7789 RGB565
屏幕,物理圆角;无 PSRAM;8 MB flash(3 MB 应用分区,ECHO 约占用其中 1.54 MB);
ES8311 音频编解码;三个按键;USB-Serial-JTAG;BLE。

## 菜单:ECHO / RADAR / 设置

主菜单有三张卡片:**ECHO**、**RADAR**、**设置**。

### ECHO(CSI 感知)

- 作为 Wi-Fi STA 连接用户自己的路由器,按固定频率 ping 网关;每个回包产生一条
  CSI 记录(约 10 Hz)。
- 由每子载波 CSI 幅度计算基于 EWMA 的运动分数。
- **自标定占用绊线**:进入该模式时,设备先采集约 7 秒的空场噪声样本,用稳健统
  计(中值 + 中值绝对偏差,MAD)自动算出阈值;可随时重新标定(OK 双击,或 BLE
  `calib` 命令)。占用判定基于运动分数的**滑动窗口中值**(抗尖峰:空场里偶发的
  运动尖峰不会主导窗口中值,也不会打断"正在变空"的计时),屏幕显示
  OCCUPIED/EMPTY 及当前状态已持续的秒数。其语义是"是否有东西穿过了本设备与其
  AP 之间的链路"——这是一条绊线,而非整个房间的在场判定——因此副行状态显示为
  "链路 设备-AP"。
- 把每条 CSI 记录以 CSV 行从 USB-Serial-JTAG 控制台串流出来,供主机分析。
- UI:半圆运动仪表(绿→黄→橙→红色阶)、子载波幅度热力条、状态行、FTM 距离、
  占用指示。
- FTM(精细计时测量)测距:ESP32-C3 是 FTM 发起端,需要 AP 支持 FTM responder
  能力;否则优雅降级为"N/A"。

### RADAR(设备发现)

- 混杂/嗅探模式(从不关联),在 1–13 信道间轮询;32 条 LRU 设备表。
- 分类:AP(来自 beacon 帧)、手机/电脑/物联网(按 OUI 表匹配)、未知(随机
  MAC → UNKNOWN)。屏幕显示彩色的各类型计数(手机绿、PC 蓝、物联琥珀、AP 紫、
  未知灰)以及累计总数,并列出设备清单,含类型徽标、厂商名或 MAC、RSSI 与近似
  距离提示(`~rnd`)。

### 设置(9 项,持久化到 NVS)

语言(中文/英文)、占用阈值、平滑系数(`alpha`)、量程(scale)、告警模式
(关/单次/连续)、音量、亮度、Ping 间隔、重新标定。用三个按键导航;改动实时生
效,并在断电后保留。

## 声光告警

EMPTY→OCCUPIED 的跳变(有东西穿过了绊线)会触发蜂鸣——音量可调,告警模式为
关/单次/连续(连续模式在占用持续期间每 1.5 秒响一次)——同时发送 BLE 告警通
知。屏闪告警曾经尝试过,但根据用户反馈已经去掉。

## BLE:常驻 GATT 外设(协议 v2)

BLE GATT 外设开机即启动,独立于当前模式运行,广播名为 `AIPassport-CSI`。

- 服务:`e2e90001-8f2a-4c7b-9f3d-1a2b3c4d5e6f`
- **STATUS**(Read + Notify)`e2e90002-...`:UTF-8 JSON,具体形态按当前模式
  变化。
  - 公共字段:`st`(0 关闭 / 1 连接中 / 2 运行中 / 3 失败)、`mode`
    (`"echo"`/`"radar"`)、`rate`(包速率)。
  - ECHO 形态另含:`ssid`、`rssi`、`mot`(运动分 0-100)、`occ`(0/1)、
    `occ_s`(占用状态持续秒数)、`ftm`(距离 cm,0 = 无效)、`fv`(FTM 有效位
    0/1)、`alert`(一个每次绊线触发就自增的计数器——客户端靠监测这个值的变化
    来判断"发生了一次新的告警")。
  - RADAR 形态另含:`dev`——`{ph, pc, io, ap, unk, tot}`,各类型计数与总数。
- **CONTROL**(Write)`e2e90003-...`:UTF-8 JSON 命令。支持:`start`、
  `stop`、`ftm`、`ping_ms`(v,毫秒,限幅 20-2000)、`mode`(切换到
  `"echo"`/`"radar"`)、`sens`(`alpha`/`scale`)、`occ_th`(v,占用阈值)、
  `alert`(v = 0/1,开关告警)、`alert_mode`(v = 0 关 / 1 单次 / 2 连续)、
  `vol`(v = 0-100)、`bright`(v = 0-100)、`calib`(基于当前空场重新自标
  定)。

## 串口 CSV 契约

USB-Serial-JTAG 控制台上每条 CSI 记录一行:

```
CSI_DATA,<rx_seq>,<timestamp_us>,<rssi>,<rate>,<noise_floor>,<channel>,<motion_0_100>,<csi_len>,[b0 b1 ... b(len-1)]
```

头部逗号分隔;末尾方括号内是 `csi_len` 个空格分隔的 `int8`,按子载波交织的
`(imag, real)` 对(子载波数 = `csi_len / 2`;幅度 = `sqrt(real^2 + imag^2)`)。

## i18n 与子集中文字体

界面为中英双语,默认中文。设备屏上的中文使用**子集 CJK 字体**:基于 Noto Sans
SC(OFL 许可),裁剪到屏幕实际用到的约 74 个汉字,用 `lv_font_conv` 生成压缩
位图字体(需要开启 `CONFIG_LV_USE_FONT_COMPRESSED`)。所有可翻译的上屏文案都
经过 `ui_echo_font`(中文用 CJK 字体,英文用 `montserrat`),切换语言会原地更
新屏幕而不重建。如果修改了中文文案,必须重新生成字体;`tools/check_cjk_font.py`
会自查中文字符串里用到的每个字是否都已收录在生成的字体里。

## UI 视觉风格

全彩 ECHO HUD:深蓝底色配琥珀品牌色,全程用颜色编码信息(运动色阶、子载波热
力条、RADAR 设备类型配色)。为物理圆角屏保留圆角安全区:内容整体内收 10px,
四角各预留约 18px 的留白。

## 源文件

| 文件 | 职责 |
| --- | --- |
| `main/demo_csi.c` / `.h` | ECHO 模式本体:LVGL 屏(运动仪表、热力条、占用指示)、后台 task、CSI 回调、命令循环 |
| `main/demo_discovery.c` / `.h` | RADAR 模式本体:被动嗅探/信道轮询、设备表、分类、LVGL 屏 |
| `main/demo_settings.c` / `.h` | 设置屏:9 个设置项、实时应用、NVS 持久化 |
| `main/occupancy.c` / `.h` | 纯 C 占用/绊线状态机:滑动窗口中值、去抖/滞回(独立的进入/离开计时器)、基于基线样本的自标定(中值 + MAD) |
| `main/csi_metric.c` / `.h` | 纯 C(零 ESP-IDF):幅度换算 + EWMA 运动指标 |
| `main/csi_proto.c` / `.h` | 纯 C:BLE 线上协议——构造 STATUS JSON、解析 CONTROL JSON 命令(协议 v2) |
| `main/csi_ble.c` / `.h` | 常驻 NimBLE GATT 外设:广播、STATUS 通知定时器、CONTROL 命令分发 |
| `main/echo_state.c` / `.h` | 跨任务共享状态:供 BLE STATUS 用的状态快照、模式切换请求、ECHO 命令队列,全部加锁/加队列保护 |
| `main/ui_i18n.c` / `.h` | 纯 C 中英双语字符串表,覆盖静态上屏文案 |
| `main/ui_echo.c` / `.h` | 共享 HUD 配色、安全区常量与控件,供菜单、ECHO、RADAR 三屏复用 |
| `main/app_settings.c` / `.h` | 设置持久化:默认值、NVS 读写、应用到运行态、从运行态快照 |
| `main/lv_font_echo_cjk_16.c`、`lv_font_echo_cjk_20.c` | 子集 CJK 位图字体(生成产物,压缩格式) |

`csi_metric`、`csi_proto`、`occupancy` 均为纯 C、零 ESP-IDF 依赖,由 host 测
试(`tests/test_csi_metric.c`、`tests/test_csi_proto.c`、
`tests/test_occupancy.c`)覆盖,纳入 `tools/validate.sh --static`。

## 配置(menuconfig)

凭证绝不入库,请在本地 `sdkconfig` 的 `ECHO (WiFi CSI Sensing)` 菜单下填写:

- `CSI_WIFI_SSID`、`CSI_WIFI_PASSWORD`——用户自己的网络(默认空;为空时 ECHO
  模式会显示未配置提示并停留,不会崩溃)。
- `CSI_PING_INTERVAL_MS`——ping 间隔,10-10000 ms(默认 100)。
- `CSI_PING_TARGET`——ping 目标 IPv4(默认空 = 使用 DHCP 拿到的网关)。
- `CSI_FTM_AUTO_INTERVAL_S`——自动 FTM 周期秒数,0-600(默认 0 = 关闭,此时
  仅在收到 BLE `ftm` 命令时测距)。

## 可用级边界(诚实说明)

单天线意味着**没有到达角/方位信息**:在场与运动检测可用,但呼吸检测、手势识
别、精确定位都是研究级课题,本固件不做。要判断 AP 的方向,只能拿着设备走动、
观察 RSSI 强弱变化——这是一种粗略方法。FTM 需要 AP 支持 responder 能力。由于
现代客户端普遍做 MAC 随机化,RADAR 报告的设备大多是未知类型。

## 配套客户端

跨平台客户端(位于 umbrella 项目的 `companion/` 目录)消费上面两份契约:

- **host-csi**:Python(`uv`)工具,从串口读取 CSV 流,实时展示子载波热力图
  与运动曲线,并支持离线分析。
- **web**:Web Bluetooth 控制面板(Chrome/Edge,桌面或安卓;不支持 iOS;需
  `localhost` 或 HTTPS),消费 GATT 协议 v2——占用、设备计数、模式、灵敏
  度、告警、重新标定,以及告警提示。
- **ios**:SwiftUI + CoreBluetooth 的 iPhone app(主屏名 ECHO,需要付费
  Apple 开发者账户),功能同上。

## 验证状态

- **Build**:PASS——ESP-IDF 5.5.3,app 约占 3 MB 分区的 1.54 MB;分区保护
  布局与合并镜像已校验。
- **Host tests**:PASS——所有可 host 测试的逻辑(`csi_metric`、
  `csi_proto`、`occupancy` 及相关纯 C 模块)。
- **Device tests**:已在真机验证——干净启动;CSI 约 10 包/秒且有实时运动
  响应;自标定占用绊线在 EMPTY/OCCUPIED 间正确跳变;声光告警(蜂鸣)在绊线
  触发时正常响起;RADAR 发现(AP 与未知设备计数正确更新);BLE STATUS 在真实
  连接下正常刷新;中文界面,包括切换语言不卡死、排版整齐。

### 仍需真机验证的项目

1. 对真实 FTM responder AP 的测距结果。
2. Wi-Fi + BLE 长时间共存下的稳定性。
3. iOS 与 Web 配套客户端在真机上的端到端联调。
