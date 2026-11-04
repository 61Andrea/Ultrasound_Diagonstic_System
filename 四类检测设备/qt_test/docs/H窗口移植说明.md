# 信号监测功能（H 窗口）移植说明

本次改造把 `自测QT/YB/QT_zice/qt_test` 工程的 Qt 功能整体并入 `qt_test`，
降级为 **H 窗口（信号监测功能）**，作为主界面底部居中的新按钮入口，
并**与 A~G 六个检测窗口共用同一条 TCP 连接**。

---

## 一、改造前的两套协议（并列在同一连接上）

两套协议都以 `AA 55` 为帧头，靠 `(第3字节, 第4字节)` 组合唯一分流，取值区间互不重叠：

| 第3字节 | 第4字节 | 帧型 | 归属 | 帧长 |
|---|---|---|---|---|
| `0x10` / `0x11` | 任意 | 诊断结果短包 | qt_test | 4 |
| `0x01` | `0x01`~`0x06` | 六种业务波形包 | qt_test | 58 / 74 / 1050 / 2074 / 4122 |
| `0x00` 或 `0x01` | `0x40` | 超声变长包 | 信号监测 | 5 + 载荷 + 2 |
| `0x01` | `0x30` | 八通道电压快照 | 信号监测 | 58 |
| `0x01` | `0x32` | 振动波形 | 信号监测 | 30 + 点数*4 + 2 |
| `0x01` | `0x33` | 八通道电流快照 | 信号监测 | 58 |

### 信号监测三类帧的字段布局（已用 docs/ 下抓包样例逐字节核对）

**振动 `0x32`**（样例：`docs/vibration_packet_ch0.txt`，通道 0、4096 点）
```
偏移  0  AA 55
偏移  2  01              版本
偏移  3  32              帧类型
偏移  4  00              通道号 0~3
偏移  5  00              采样格式（0 = 24bit 有符号 AD 码）
偏移  6  u32             序号 sequence
偏移 10  u32             头部上报采样率（样例 = 0x1000 = 4096 Hz）
偏移 14  u32             点数（样例 = 0x1000 = 4096）
偏移 18  u32             载荷字节数（样例 = 0x4000 = 16384）
偏移 24  载荷            每点占 4 字节，取低 24 位为有符号 ADC 码
末尾 2   FF FF
```

**电压 `0x30` / 电流 `0x33`**（样例：`docs/voltage_snapshot_packet.txt`）
```
偏移  0  AA 55
偏移  2  01              版本
偏移  3  30 / 33         帧类型
偏移  4  FF              标记
偏移  5  00              保留
偏移  6  u32             序号
偏移 10  u16             通道数 = 8
偏移 12  u32             载荷字节数 = 32
偏移 24  8 * float32 LE  电压值(V) / 电流值(A)
末尾 2   FF FF
```

**超声变长包**（样例：`docs/ultrasonic_packet_ch0.txt`）
```
偏移  0  AA 55
偏移  2  <通道 0~7>
偏移  3  40              帧类型
偏移  4  u16 LE          载荷字节数（样例 = 0x0018 = 24）
偏移  6  载荷            qint32 LE 序列（样例 6 个点：一个完整正弦周波）
末尾 2   FF FF
```

---

## 二、本次改动清单

### 新增文件（全部位于 `qt_test/qt_test/`）

| 文件 | 说明 |
|---|---|
| `h_window.h` / `h_window.cpp` | H 窗口。由 `自测QT::MainWindow` 迁移，**不持有 QTcpSocket** |
| `h_window.ui` | H 窗口界面。`QMainWindow` → `QDialog`，去掉 menuBar/toolBar/statusBar，改用窗口内 `label_status` 状态栏 |
| `waveformwidget.h` / `waveformwidget.cpp` | 自绘波形控件，原样搬入（零第三方依赖） |
| `docs/*.txt` | 四份协议抓包样例，仅作对照，不参与编译 |

### 修改文件

| 文件 | 改动 |
|---|---|
| `dealfile.h` | 新增 4 个信号：`vibrationDataReceived`、`voltageSnapshotReceived`、`currentSnapshotReceived`、`ultrasonicDataReceived` |
| `dealfile.cpp` | 解包状态机新增 4 个分支：超声变长包（分流 A2）、`0x30`、`0x33`、`0x32`。原有 `0x01`~`0x06` 六个分支与滑动自同步逻辑**未改动** |
| `mainwindow.h` | 新增槽 `on_H_button_clicked()`；新增成员 `m_deviceConnected` |
| `mainwindow.cpp` | 新增 `on_H_button_clicked()` 实现；`updateConnectionStatus()` 中同步 `m_deviceConnected` |
| `mainwindow.ui` | 底部居中新增 `H_button`（285×78 @ 563,855，空心白框 + 14pt 华文仿宋） |
| `qt0320.pro` | SOURCES/HEADERS/FORMS 加入 H 窗口与波形控件；DISTFILES 加入 docs 样例 |

---

## 三、连接共用的具体做法

**发送方向**（H 窗口 → 下位机）
```cpp
connect(hWindow, &H_window::sendCommand,     dealFile, &DealFile::sendData);
connect(hWindow, &H_window::sendFrameSignal, dealFile, &DealFile::sendFrame);
```
`DealFile` 已在 `sendData` / `sendFrame` 内部做 `ConnectedState` 检查并会 `emit errorOccurred`，
因此 H 窗口内原本 20 多处 `if (tcpSocket->state() != ConnectedState)` 与 `tcpSocket->flush()` 全部删除。

**接收方向**（下位机 → H 窗口）
`DealFile` 在子线程解析出载荷后发信号，`MainWindow` 用 lambda 转发给 H 窗口：
`DealFile` 属于子线程、接收者是主线程的 `MainWindow`，`Qt::AutoConnection` 自动走队列连接，
UI 更新安全落在主线程。lambda 内再用 `m_activeChildWindow == hWindow` 二次过滤，
与 A~G 六个窗口的事件投递方式保持一致。

**连接状态** `dealFile::connectionStatusChanged` → `H_window::setDeviceConnected`。
未连接时四个采集按钮与通道下拉框禁用，并且**断线即复位采集状态机**
（`vibrationAcquiring` / `ultrasonicAcquiring` / `*Pending` 全部清零、按钮文案复位），
避免复用连接时残留旧的"采集中"状态。

**互斥** `on_H_button_clicked()` 复用现有 `m_activeChildWindow` 拦截，
H 窗口与 A~G 检测窗口同一时刻只能开一个——这正是"串行复用一条连接"的前提。

---

## 四、按你确认的决策落实的四处

1. **承载方式**：独立窗口（`QDialog`），与 A~G 子窗口同级 —— 已按此实现。
2. **振动采样率**：保持 `64000` 不变。抽出为两个具名常量，日后一处可改：
   - `waveformwidget.cpp` 的 `kVibrationSampleRateHz`
   - `h_window.cpp` 的 `kVibrationSampleRate`
   - 两者用于过零测频与 X 轴换算，**必须保持一致**。
3. **按钮样式**：完全复刻附图 —— 空心/透明底 + 2px 白框 + 14pt（比上方六个按钮小一号），
   hover 为半透明白底，按下为白底蓝字反色。
4. **样例文件**：已搬到 `docs/` 并登记进 `.pro` 的 DISTFILES。

---

## 五、遗留疑问（需你确认，均不影响当前编译）

### Q1. 头部上报采样率 4096 Hz 与硬编码 64000 的矛盾
`docs/vibration_packet_ch0.txt` 偏移 10 明确是 `0x1000 = 4096`，但原工程注释写
"忽略硬件报的假采样率，强行写入真实的 ADC 物理采样率"，并硬编码 64000。
本次按你的决定保持 64000 不动。**若设备实际是 4096 Hz，则时间轴与频率显示会整体偏约 15.6 倍。**

### Q2. `build5007StatusPacket` 的 `firstStatusIndex` 语义
H 窗口三个入口分别传 `(channel, 1)`、`(12, 8)`、`(4, 8)`，即
振动用通道号、电压用 12、电流用 4。这三个偏移量是原工程的硬编码，
**我无法从代码或抓包样例反推其含义**（状态位到物理通道的映射表）。请确认是否正确。

### Q3. 超声通道映射
原码 `physicalChannel = uiChannel`，启动指令为 `304 + 10 * physicalChannel`。
界面上是"超声通道 1~8"（索引 0~7），下位机侧的通道编号是否为 0~7？
样例 `ultrasonic_packet_ch0.txt` 的第 3 字节是 `0x00`，看起来是 0 基，暂按此保留。

### Q4. 超声包第 3 字节是版本号还是通道号
原 `自测QT` 代码把它同时当"版本号"（判定 `version < 8` 才当超声）和"通道号"使用，
语义含糊。现按"通道号"统一处理，并且同时接受第 3 字节为 `0x00` 或 `0x01`
（`dealfile.cpp` 分流 A2），兼容性更宽。若下位机实际上是"版本号恒为 0、通道号在别处"，
请告知，我再调整。

### Q5. 快照包的"隐藏门槛"
原 `自测QT` 仅在 `voltageSnapshotPending` / `currentSnapshotPending` 为真时才填表，
本次保持不变。若希望"只要收到快照就刷新表格"，去掉这两个判断即可。

### Q6. 未编译验证
本机 DSH 沙箱在给会话工作区授予写权限时被 Windows 拒绝
（`SetNamedSecurityInfoW failed (Win32 5): grantWrite(...QT_hebin)`），
**PowerShell 完全无法调用**，因此本次改动**只做了逐行人工审查，未做编译验证**。
请在有 Qt 5.9.8 MinGW 32bit 的环境下执行 `qmake && mingw32-make` 确认，
我预计需要关注的两处编译风险：
- `h_window.cpp` 中 `QVector<qint32>` 与 `WaveformWidget::setWaveform` 重载的匹配；
- `mainwindow.ui` 的 `H_button` 是否会因绝对定位与 `label_6` 重叠（当前坐标已核算，无重叠）。
