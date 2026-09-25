# STM32F103 交互式信号发生器

基于 STM32F103C8T6 的多协议信号发生器：电位器调参、单按键菜单、SSD1306 OLED 图形化显示，
支持标准 PWM / OneShot125 / OneShot42 / MultiShot / DShot150 / DShot300 / DShot600 七种协议在线切换。

---

## 1. 硬件与引脚分配

MCU：STM32F103C8T6，LQFP48，HSE 8MHz × PLL9 = **72MHz**。

| 功能 | 引脚 | 外设 | 配置 |
|---|---|---|---|
| 信号输出 | **PA6** | TIM3_CH1 | AF 推挽，DMA 请求 TIM3_UP |
| 电位器 | **PA0** | ADC1_IN0 | 模拟输入，连续转换 |
| 菜单按键 | **PB0** | GPIO | 输入 + 内部上拉，**低有效** |
| 状态指示灯 | **PC13** | GPIO | 推挽输出，低有效（板载 LED） |
| OLED SCL | **PB10** | I2C2_SCL | 复用开漏 |
| OLED SDA | **PB11** | I2C2_SDA | 复用开漏 |
| 调试 | PA13/PA14 | SWD | 保留 |

外设分工：

- **TIM3** — 信号生成。需频繁改写 PSC/ARR/CCR，由 `signal_output.c` 独占。
- **TIM4** — 1ms 控制节拍，仅更新中断，不占引脚。
- **DMA1_Channel3** — 硬件固定映射到 TIM3_UP（RM0008 表 78），不可自选。
- **I2C2** — OLED，阻塞式发送，不占 DMA。

### 引脚冲突说明

- **PA0 与 TIM2_CH1 重叠**：若改用 TIM2 输出 PWM 会与电位器争用 PA0。本项目选 TIM3 正是为避开。
- **PA13/PA14**：默认 JTAG，CubeMX 已设为 `SWJ_NOJTAG`（仅 SWD），否则释放不出 PA15/PB3/PB4。
- **PC13**：内部上拉很弱（约 40kΩ），只能驱动 LED 或高阻负载，不可用于推电流。
- **PB10/PB11 与 TIM2_CH3/CH4 重叠**：若后续要加 TIM2 输出通道，需改用 I2C1（PB6/PB7）。
- **DMA1 通道紧张**：Channel1=ADC1、Channel3=TIM3_UP、Channel4=I2C2_TX、Channel2/5=SPI/USART。
  加第二路 DShot 输出会受限于剩余的 DMA 通道。

---

## 2. CubeMX 配置要点

### 时钟（RCC）

- HSE：Crystal/Ceramic Resonator
- PLL：HSE × 9 = 72MHz
- AHB = 72MHz，APB1 = 36MHz（**预分频 ≠ 1，故 APB1 定时器时钟 = 72MHz**），APB2 = 72MHz
- ADC 预分频：`RCC_ADCPCLK2_DIV6` → ADCCLK = 12MHz（上限 14MHz，留足裕量）
- Flash 延迟：2 wait state

### ADC1

- IN0（PA0）、独立模式、单通道、`ContinuousConvMode = ENABLE`
- 对齐：右对齐，12 位；采样时间 **55.5 cycles**
- 触发：软件启动
- 初始化后必须调用 `HAL_ADCEx_Calibration_Start()`
- 换算时间：(55.5 + 12.5) / 12MHz ≈ 5.7µs，1kHz 采样绰绰有余

### TIM3（信号输出）

- PSC/ARR 不固定，由运行期按协议改写
- 通道 1：PWM 模式 1，高电平有效，`AutoReloadPreload = ENABLE`
- **不配置 GPIO 复用**，由 `signal_output.c` 初始化（见下方"为什么不用 CubeMX 生成 TIM3"）

### TIM4（控制节拍）

- PSC = 7200-1，ARR = 10-1 → 72MHz / 7200 / 10 = **1kHz，无误差**
- 使能更新中断，抢占优先级 2

### GPIO

- PA0：Analog
- PA6：AF Push-Pull，High speed（由 `signal_output.c` 设置）
- PB0：Input + Pull-up
- PC13：Output PP，初始置高（灭）

### I2C2

- 400kHz（Fast Mode），7 位地址，占空比 1/2
- SSD1306 从机地址 0x78

### NVIC

| 中断 | 优先级 | 用途 |
|---|---|---|
| SysTick | 15 | HAL 时基 |
| TIM4 | 2 | 1ms 控制节拍 |
| DMA1_Channel3 | 3 | DShot 帧传输（低于控制节拍） |

### 为什么不用 CubeMX 生成 TIM3

TIM3 需要在运行期反复改写 PSC/ARR，且要求"先算后写"的事务性切换。CubeMX 生成的
`MX_TIM3_Init()` 是固定参数的一次性初始化，无法表达这种模式。故 TIM3 与 DMA 的初始化
放在 `signal_output.c` 内自管，并与 DMA1_Channel3 的 NVIC 使能、`__HAL_LINKDMA` 绑定在一起。

---

## 3. 信号协议

### 波形与硬件路径

| 波形 | 协议 | 硬件路径 |
|---|---|---|
| `SIG_WAVE_PWM` | 标准 PWM | ARR 定周期 + CCR 定占空比，**不用 DMA** |
| `SIG_WAVE_ONESHOT` | OneShot125/42、MultiShot | 同上（本质是"周期固定的 PWM"，参数以脉宽表达） |
| `SIG_WAVE_DSHOT` | DShot150/300/600 | ARR 定**位周期** + DMA 逐位写 CCR |

### 各协议参数

| 协议 | 参数 | 范围 | 固定周期 |
|---|---|---|---|
| PWM | 频率 / 占空比 | 50–20000 Hz / 0.0–100.0%（0.1% 步进） | 由频率决定 |
| OneShot125 | 脉宽 | 1–250 µs | 250 µs（4kHz） |
| OneShot42 | 脉宽 | 1–84 µs | 84 µs（12kHz） |
| MultiShot | 脉宽 | 1–25 µs | 30 µs（33kHz） |
| DShot150/300/600 | 油门 | 0（停机）或 48–2047 | 由节流周期决定 |

**限幅逻辑**：全部由 `SignalParam.min/max` 描述并由 `SignalProtocol_ClampParam()` 强制。
越界输入被夹取而非报错，保证写入寄存器的值必然合法。额外两道保护：

- 脉宽上限取 `ARR-1`，保证每个周期都有可见低电平间隔（否则接收端无法识别脉冲边界）
- 占空比比较值上限取 `ARR`（100% 对应 CCR=ARR）

### DShot 的定时细节

- 位周期 = `1e9 / 比特率`，72MHz 下的计数：150kbps→480、300kbps→240、600kbps→120
- **`tick_per_unit` 必须能被 8 整除**，否则 3/8 与 6/8 位周期无法整数表达。
  `ARR = tick_per_unit - 1`（硬件周期是 ARR+1 个计数）
- 逻辑 `0` → 高电平 3/8 位周期；逻辑 `1` → 6/8；比较值超出部分为低电平
- **帧率由节流周期决定**：帧结构为 16 数据位 + 停止位，之后**填充足量零值位槽**，
  使整帧恰好占满 `DSHOT_LOOPTIME_US`（125µs → 8kHz）。
  72MHz 下位槽数（整数除法截断，因此低速率协议的帧周期略短于 125µs）：

  | 协议 | 位周期 | 位槽数 | 实际帧周期 | 实际帧率 |
  |---|---|---|---|---|
  | DShot600 | 1667ns | 75 | 125.0µs | 8000 Hz |
  | DShot300 | 3333ns | 37 | 123.3µs | 8108 Hz |
  | DShot150 | 6667ns | 18 | 120.0µs | 8333 Hz |

  界面与日志显示的是**实际生效值**（`SignalOutputInfo.actual_hz`），不是期望值。
  电调对节流周期的容差较宽，上述偏差在可接受范围；若要严格 8kHz，可改用更大的
  预分频让位周期计数变大，以提高整数分辨率。

  这一步是关键：DShot 并非"一帧紧接一帧"发送。若按最小帧无间隔重发，帧率会达数十 kHz，
  电调会判为非法信号并静默丢弃。

- **校验位不是多项式 CRC**，而是帧各半字节异或取低 4 位：
  `checksum = (frame ^ frame>>4 ^ frame>>8) & 0xF`

  注意：这与 `x^4+x+1` 的多项式 CRC 结果不同（实测两者对同一帧无一例相同）。
  用错算法会让所有帧校验失败，表现为电机完全不响应。这一条由单元测试与独立参考实现交叉验证。

### DShot1200 为何不支持

位周期 = `1e9/1200000` = 833.3ns → 60.0 个计数（72MHz 下），看似可行，但
逻辑 `0` 需 `3/8 × 60 = 22.5` 个计数，**非整数**，16 位定时器无法精确表达。
该情况被 `SignalProtocol_Probe()` 以 `tick_per_unit % 8 != 0` 明确拒绝，
而不是提供一个跑不准的选项。若将来提高定时器时钟或换更高主频器件，
只需在协议表追加一条描述符即可。

### 扩展新协议

在 `signal_protocol.c` 的 `s_protocols[]` 追加一条描述符（含 `wave`、定时参数、参数槽定义）。
核心逻辑、输出层与界面均按描述符驱动，**无需改动**：

- 若新协议属于已有三种波形族之一 → 只加数据，零代码改动
- 若是新波形族 → 在 `Probe`/`BuildPlan` 各加一个分支

---

## 4. 按键消抖与菜单（为什么用轮询而非外部中断）

### 决策：定时器轮询，不用 EXTI

按键状态在 **TIM4 的 1ms 中断**里采样，经 `Button_Update()` 消抖。

选轮询的理由：

1. 机械按键抖动持续 5–20ms，与 1ms 采样配合做"连续稳定 N 次"判据，比中断天然抗抖动 ——
   EXTI 会在抖动期间反复进中断，仍需在中断里做同样的计时消抖，多一道无用开销。
2. 长按判定需要"持续时间"，这本质是定时器功能。用 EXTI 也必须另配一个定时器计时，
   等于同时占用 EXTI 与定时器，比纯轮询更复杂。
3. 消抖后的事件（短按/长按）需要在主循环里与菜单、旋钮、显示协同处理。
   轮询产出的**事件队列**天然适合这种协作；中断只适合"立刻响应"的语义。

若将来按键需要唤醒 STOP 模式，则必须改用 EXTI —— 那时轮询会失效。

### 消抖与事件

| 参数 | 值 | 含义 |
|---|---|---|
| `BTN_DEBOUNCE_MS` | 20 | 电平连续稳定时长才确认 |
| `BTN_LONGPRESS_MS` | 800 | 长按判定阈值 |
| `BTN_REPEAT_MS` | 150 | 长按后重复事件间隔 |

事件：`BTN_EVENT_SHORT` / `BTN_EVENT_LONG` / `BTN_EVENT_REPEAT`。

### 交互模型

| | BROWSE（浏览） | EDIT（编辑） |
|---|---|---|
| **短按** | 焦点在协议项：切到下一个**可用**协议（循环）<br>焦点在参数项：焦点移到下一项 | 提交并**停留**在编辑态（便于连续微调） |
| **长按** | 焦点在协议项：焦点下移到第一个参数项<br>焦点在参数项：进入编辑态 | 提交并**返回**浏览态 |

设计取舍：

- **只有一个按键**，所以协议选择与参数编辑不能各占一个控件。协议项无"数值"，
  因此长按对协议项不进入编辑（否则会出现旋钮无效的空编辑态），
  协议切换由浏览态对协议项的**短按**完成。
- 旋钮采用**相对位移**语义：进入编辑态时以当时的旋钮值为锚点。
  若用绝对位置，切换协议后旋钮的物理位置会把参数"拽走"。
- 切换协议时**跳过 `SignalOutput_Probe()` 失败的协议**，用户无法落到不可用的协议上。

### 协议切换调用链

```
TIM4 ISR (1ms) → App_Tick() 置标志
   ↓
App_Loop() → Button_Update(pressed) → Button_Poll() → Menu_HandleEvent(ev)
   ↓  短按协议项 / 长按提交
Menu_SelectProtocol(idx) 或 Menu_Commit()
   ↓
SignalOutput_Select(proto, params)
   ├─ 1. SignalProtocol_BuildPlan()   ← 纯计算, 不碰寄存器
   ├─ 2. 失败 → 立即返回, 硬件与当前协议完全不变
   └─ 3. 成功 → 停 DMA → 写 PSC/ARR/CCR → 重挂 DMA → 使能输出
   ↓
SignalLog_Push(INFO, "SW DShot600 OK")  /  SignalLog_Printf(ERROR, ...)
   ↓
UI_Refresh() 读 SignalOutput_GetInfo() + SignalLog_Latest() → OLED
```

---

## 5. 失败回退与报错

### 事务性切换

协议切换是"先算后写"：

1. `SignalProtocol_BuildPlan()` 在**纯计算**阶段完成全部校验（分辨率、周期范围、帧长、参数夹取）
2. 任一校验失败 → 直接返回错误码，**一个寄存器都不动**，当前信号继续输出
3. 校验全通过才开始停 DMA、写寄存器
4. 若写入阶段仍失败 → 主动回到**安全空闲低电平**，而不是停在半个周期的中间态

即"失败 = 不切换"，不会出现半配置状态。

### 错误码

| 码 | 含义 | 典型触发 |
|---|---|---|
| `SIG_ERR_NULL_HANDLE` | 空指针 | 编程错误 |
| `SIG_ERR_UNKNOWN_ID` | 协议未注册 | 非法 ID |
| `SIG_ERR_TICK_TOO_FAST` | 定时器分辨率不足 | DShot1200（tick 不能被 8 整除） |
| `SIG_ERR_PERIOD_TOO_LONG` | 周期超出计数范围 | 极低频率 |
| `SIG_ERR_PERIOD_TOO_SHORT` | 周期小于最小计数 | 节流周期装不下一个 DShot 帧 |
| `SIG_ERR_FRAME_TOO_LONG` | 帧超出 DMA 缓冲 | DMA 容量不足 |
| `SIG_ERR_UNSUPPORTED` | 硬件/配置不支持 | 波形类型未知 |
| `SIG_ERR_RANGE` | 参数越界 | 频率为 0 |
| `SIG_ERR_NO_PARAM` | 参数槽未定义 | 访问越界槽 |

### 反馈渠道

- **界面**：OLED 第四行显示 `E:<错误码文本>` 与最新日志（级别字母 I/W/E 前缀）
- **日志**：`signal_log` 环形缓冲（8 条 × 21 字符），成功写 `SW <协议名> OK`，
  失败写原因。后续接串口只需在写入处增加一个输出点，无需改动调用方。

---

## 6. OLED 界面

SSD1306，128×64，I2C。驱动为波特律动图形库（`oled.c` / `font.c`）。

```
 PROTO: DShot600              ← 第 1 行 y=0   协议名, 焦点标记 >
 500                    100%  ← 第 2 行 y=12  当前参数 + 单位 + 百分比
 OUT 600kbps 500              ← 第 3 行 y=32  实际生效输出
 I SW DShot600 OK             ← 第 4 行 y=32↓ 日志(级别 + 内容)

 ████████████░░░░░░░░░░░░     ← 条形图 y=40, 120×10, 随焦点参数变化
 ╱╲___╱╲__                    ← 波形 y=52, 124×11, 最近 62 个点的轨迹
```

**字库限制（重要）**：`font.c` 的 `zh16x16` 只含 4 个汉字字模，`ASCIIFont` 仅覆盖
`0x20`–`0x7B`，且 `OLED_PrintASCIIChar()` 按 `ch - ' '` 索引。
因此**所有屏幕文本必须是 ASCII**，中文字符串会渲染成空白。代码中的中文注释与日志
不经过 OLED，故不受影响。

**显示节流**：`UI_REFRESH_MS = 80ms`。OLED 每帧需发送 8 页 × 128 字节（约 1KB），
阻塞式 I2C 在 400kHz 下约 20ms。若不节流会拖慢 1ms 控制节奏。

---

## 7. 电位器采样与滤波

两级滤波，兼顾抗尖峰与平滑：

1. **中值滤波**（窗口 3）— 抑制单次采样尖峰，这是 ADC 最常见的高频干扰
2. **滑动平均**（窗口 16，用移位实现）— 平滑抖动；维护累加和并减去最旧值，避免每次重算

归一化：`level = (raw - KNOB_RAW_MIN_VALID) × 1000 / (KNOB_RAW_MAX_VALID - KNOB_RAW_MIN_VALID)`，
夹取到 `0..1000`。两端各留 20 LSB 无效区，规避机械行程外的悬空读数。

死区 `KNOB_LEVEL_DEADBAND = 4`：归一化值变化超过阈值才认为有效，抑制显示末位抖动。

---

## 8. 代码结构

```
Core/
  Inc/  app_config.h        所有可调常量(唯一调参入口)
        signal_protocol.h   协议描述符/注册表/编码器   ─┐ 纯逻辑,
        signal_log.h        环形日志                   ─┘ 不依赖 HAL
        signal_output.h     TIM3+DMA 硬件层
        knob.h / button.h   采样与按键(纯逻辑)
        menu.h             菜单状态机
        ui.h               OLED 渲染
        app.h              装配与调度
        adc.h / tim.h / gpio.h / i2c.h / oled.h / font.h
  Src/  对应实现
tests/  宿主机单元测试(不交叉编译)
```

分层依赖是单向的：

```
app → menu → signal_output → signal_protocol
  ↓      ↓                      ↑
 ui    knob/button           (纯计算)
  ↓
 signal_log
```

`signal_protocol.c` / `signal_log.c` / `knob.c` / `button.c` 不引用任何 STM32 头文件，
因此可在 PC 上直接编译测试。

---

## 9. 构建

```powershell
cd D:\STM32PROJECK\AItostm32
cmake --preset Debug
cmake --build build/Debug
```

产物：`build/Debug/AItostm32.elf`

实测占用（`-O0 -g3` Debug）：

```
RAM:   4152 B / 20 KB   (20.3%)
FLASH: 34912 B / 64 KB  (53.3%)
```

---

## 10. 验证

### 宿主机单元测试

```powershell
cd D:\STM32PROJECK\AItostm32
cmake -S tests -B build/tests -G Ninja
cmake --build build/tests
ctest --test-dir build/tests --output-on-failure
```

需要 `-G Ninja`：本机 CMake 默认生成器为 NMake 但找不到 MSVC。

| 测试 | 覆盖内容 |
|---|---|
| `test_signal_protocol` | 注册表完整性、七协议能力探测、PWM/OneShot 的 PSC/ARR/CCR 精确值、DShot 位时序与帧率、校验位与独立参考实现交叉验证、空指针与越界防护、参数步进与限幅 |
| `test_signal_log` | 环形回绕、容量上限、超长截断（保证 0 结尾）、倒序读取、seq 单调性、NULL 防护 |
| `test_button` | 抖动序列只产生一个 SHORT、长按产生 LONG 并在按住时 REPEAT、消抖阈值 |
| `test_knob` | 中值滤波剔除单点尖峰、滑动平均平滑、归一化端点与越界夹取 |

预期结果：`100% tests passed, 0 tests failed out of 4`。

关键断言举例：

- DShot600 在 72MHz 下：`ARR=119`、`ccr_low=45`、`ccr_high=90`、帧周期 `125µs`、帧率 `8000Hz`
- 1kHz PWM：`PSC=1`、`ARR=35999`、50% 时 `CCR=18000`
  （72000 个计数超过 16 位上限，必须分频 —— 这一点曾被我误判为 `PSC=0`，是测试纠正了预期）
- DShot 校验位：8 组边界帧（0/1/48/500/1000/2047 及遥测位开关）与独立参考实现逐一比对

### 需要真实硬件确认的部分

单元测试覆盖纯逻辑与寄存器目标值，以下**必须**上板验证：

1. **DShot 能否被电调解码** —— 用示波器看位宽（0 位 625ns、1 位 1250ns、位周期 1667ns）
   与帧间隔（125µs），并确认电调正常响应油门
2. **PWM/OneShot 实测频率与脉宽** —— 示波器测 PA6
3. **OLED 实际显示** —— 字库边界、像素对齐、上电 20ms 等待是否足够
4. **电位器手感** —— `KNOB_RAW_MIN/MAX_VALID` 是否匹配实际行程
5. **按键真实消抖** —— 宿主机用模型化的抖动序列验证过逻辑，真实触点波形未必与之相同
