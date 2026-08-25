# RBLink V3.0 硬件规范（EDA 交接基线）

> 状态：已按2026-08-25导出的网表和原理图PDF同步。连接关系以该网表为准；
> 本文件中的“待修正/待确认”项目在投板前必须闭环。后续 AL/EasyEDA 设计以本文件为硬件基线；
> 固件引脚实现见 `RBLINK_V3.0_PINMAP.md`；若与早期讨论冲突，以本文件和定版网表为准。

## 1. 产品与固件基线

- 产品名称及 USB Product String：`RBLink V3.0`。
- 主控：STM32F407VGT6，LQFP100，3.3 V 供电。
- CMSIS-DAP协议核心：Arm CMSIS-DAP v2.1.1；正式固件工程位于
  `RBLink_DAP_V3.0/`，不再依赖旧DAPLink仓库。原参考版本为DAPLink `v0258`，提交
  `cbc2daa8815f02c580fbfc5daa441bfc31db2051`。
- 调试下载：CMSIS-DAP v2，USB Full-Speed Bulk/WinUSB。
- UART：USB CDC ACM。
- SPI、I2C、CAN：复用 CMSIS-DAP Vendor Command，不增加独立 HID 接口。
- 私有命令：`0x85` SPI、`0x86` I2C、`0x87` CAN、`0x8E` 目标电源。
- V2.1、V2.2 保持不变；V3.0 独立开发。

## 2. 已冻结的接口要求

### 2.0 RBLink 自定义 20 针复用接口（AL 首要输入）

- V3.0 对外调试连接器采用 2×10、20 针物理形式，但电气定义是 **RBLink 自定义接口**，
  不是标准 ARM 20 针 JTAG。AL 必须先创建专用符号 `LTLINK_DEBUG_20P_V3`，不得继续
  使用内部偶数脚名称为 GND 的标准 JTAG 符号后仅在外部覆盖网络名。
- 专用符号的引脚编号、引脚名称和网络必须按下表冻结；Pin 1 方位必须在符号、封装、
  PCB丝印及外壳上同时标识。

| Pin | 专用符号引脚名 | 原理图网络 | 方向（以RBLink为基准） | 说明 |
|---:|---|---|---|---|
| 1 | VTREF_IN | `V_REF` | 输入 | 目标侧逻辑电平参考，1.8–5.0 V |
| 2 | TARGET_POWER_OUT | `V_POWER` | 电源输出 | 软件可调0–5 V/最大300 mA |
| 3 | JTAG_nTRST | `PORT_JNTRST` | 输出 | JTAG测试复位，低有效 |
| 4 | SPI_NSS | `PORT_SPI_NSS` | 输出 | 目标SPI片选，低有效 |
| 5 | JTAG_TDI | `PORT_JTDI` | 输出 | JTAG数据输入（目标视角） |
| 6 | SPI_SCK | `PORT_SPI_SCK` | 输出 | 目标SPI时钟 |
| 7 | JTAG_TMS_SWDIO | `PORT_JTMS_SWDIO` | 双向 | JTAG TMS / SWDIO复用 |
| 8 | SPI_MOSI | `PORT_SPI_MOSI` | 输出 | RBLink到目标 |
| 9 | JTAG_TCK_SWCLK | `PORT_JTCK_SWCLK` | 输出 | JTAG TCK / SWCLK复用 |
| 10 | SPI_MISO | `PORT_SPI_MISO` | 输入 | 目标到RBLink |
| 11 | SIGNAL_GND | `GND` | 地 | 网表中已改为地，不再保留RTCK |
| 12 | I2C_SCL | `PORT_IIC2_SCL` | 双向开漏 | 目标I2C时钟 |
| 13 | JTAG_TDO_SWO | `PORT_JTDO_SWO` | 输入 | JTAG TDO / SWO复用 |
| 14 | I2C_SDA | `PORT_IIC2_SDA` | 双向开漏 | 目标I2C数据 |
| 15 | TARGET_nRESET | `PORT_NRST` | 开漏输出 | 目标复位，低有效 |
| 16 | PWM1 | `PORT_PWM1` | 输出 | TIM8_CH1经电平转换输出 |
| 17 | UART_TX | `PORT_UTXD` | 输出 | RBLink TX到目标RX |
| 18 | PWM2 | `PORT_PWM2` | 输出 | TIM8_CH2经电平转换输出 |
| 19 | UART_RX | `PORT_URXD` | 输入 | 目标TX到RBLink RX |
| 20 | SIGNAL_GND | `GND` | 地 | JTAG/UART/SPI/I2C及目标电源公共回流 |

- Pin 2 的当前原理图网络为`V_POWER`，功能名称为TARGET_POWER_OUT；
  当目标板已有外部VTREF时，目标电源输出必须关闭并保持高阻，禁止反向灌电。
- Pin 16/18 在标准 ARM 20 针JTAG中通常是GND，本设计改为PWM1/PWM2；Pin 4、6、8、
  10、12、14同样不再是标准接口的GND。禁止使用标准JTAG直通排线，并必须在连接器
  丝印、外壳和用户手册标注 `RBLink CUSTOM 20P` 及误插警告。
- Pin 20 是非隔离接口及目标电源唯一外部GND。连接器焊盘、走线和线缆导体必须按
  300 mA连续电流留有余量；Pin 20 到系统地采用短而宽的回路，高速JTAG/SPI输出端
  预留22–33 ohm源端串联电阻，降低单地回流造成的地弹和串扰。
- CAN不再复用20针接口，改用独立3针连接器CN1：Pin 1 CANH、Pin 2 `VSIO_GND`、
  Pin 3 CANL。CAN连接器、TVS及终端网络均保持在隔离侧。

### 2.1 SWD/JTAG、UART、SPI

- 目标侧逻辑电平：1.8–5.0 V，由目标板 `VTREF` 给电平转换器目标侧供电。
- 电平转换按当前网表使用3颗`TXU0304PWR`（SPI、JTAG、UART/PWM）以及1颗
  `SN74LVC1T45DBVR`（SWDIO可控方向）。
- MCU 侧供电：3.3 V；目标侧供电：`VTREF`。
- SWCLK、TDI/TMS、nTRST、UART_TX、SPI_SCK、SPI_MOSI、SPI_CS：MCU 到目标板，
  DIR固定输出或按对应开漏要求实现。
- TDO、UART_RX、SPI_MISO：目标板到 MCU，DIR 固定输入。
- SWDIO在MCU侧使用独立输出脚`PD13/T_JTMS_SWDIO-1`和独立输入脚
  `PD12/T_JTMS_SWDIO-2`，方向由`PE10/DIR_SPI_JTMS_SWDIO`控制；目标侧仍为同一个
  `PORT_JTMS_SWDIO`。固件通过分别读写两个GPIO
  减少反复修改GPIO模式造成的延时，但SWD turnaround周期仍必须保留，并确保输出通道释放后
  才采样输入通道，禁止总线争用。
- `SN74LVC1T45DBVR`没有OE引脚，原理图不得虚构OE网络。SWDIO通道的DIR
  必须用硬件上下拉定义为“目标端到MCU端”的安全输入方向，固件确认VTREF/VCCB有效后
  才允许切换为输出。若必须实现整组硬件高阻，应另加电源负载开关，或改用带OE的转换器。
- 每颗转换器的 VCCA、VCCB 均必须就近放置 100 nF 去耦，回流路径短且直接。
- nRESET 使用目标侧开漏器件，不允许将 MCU 3.3 V 主动推入目标板。

### 2.2 I2C

- I2C 两线电平转换使用 `PI4ULS5V202UEX`，同时转换 SDA、SCL。
- MCU 侧为 3.3 V，目标侧为 `VTREF`，覆盖 1.8–5.0 V。
- `PI4ULS5V202UEX` 两侧各集成 10 kΩ 上拉。MCU侧不再外加上拉；目标接口侧的
  SCL、SDA 分别固定外加 4.7 kΩ 到 `VTREF`，不使用MOS、跳线或软件控制。
- 目标侧内部10 kΩ与外部4.7 kΩ并联后的有效上拉约为3.2 kΩ；连接已有上拉的目标板时，
  必须核算并联后的低电平灌电流，不得再接低阻值强上拉。
- SDA/SCL 按开漏总线布线，不串接 SN74LVC1T45，也不使用软件 DIR。

### 2.2.1 目标接口固定偏置（已冻结）

- 不使用TMUX1511、模拟开关或GPIO控制上拉/下拉，全部采用固定电阻。
- JTAG/SWD目标侧固定偏置：`nTRST`、`TDI`、`TMS/SWDIO`、`nRESET`分别使用
  100 kΩ上拉到`VTREF`；`TCK/SWCLK`使用100 kΩ下拉到GND；`TDO/SWO`不加上下拉。
- SPI目标侧：`SPI_NSS`固定使用10 kΩ上拉到`VTREF`；SCK、MOSI、MISO不加固定上下拉。
- I2C目标侧：SCL、SDA继续分别固定使用4.7 kΩ上拉到`VTREF`。
- 所有固定偏置电阻均放在目标接口/电平转换器目标侧，不得接到MCU固定3.3 V侧。

### 2.3 隔离 CAN

- CAN 必须隔离，首版协议为 Classical CAN 2.0B，最高 1 Mbps。
- 隔离 CAN 收发器冻结为 `CA-IS2062VW`，制造商 Chipanalog（川土微），宽体
  SOIC-16，立创商城编号 `C5271191`，单台用量 1 颗。不得替换为不带独立逻辑
  电源脚的 `CA-IS2062W`。
- MCU 使用 CAN1：STM32F407 `PB9/CAN1_TX` 接器件 TXD，`PB8/CAN1_RX` 接器件 RXD，GPIO复用为AF9。
- 逻辑侧 `VCC` 接 USB 5 V，为内部隔离 DC/DC 供电；`VCCL` 接 MCU 3.3 V，
  使 TXD/RXD 与 STM32F407 逻辑电平直接兼容；`GND1` 接系统 GND。
- 总线侧 `VISO` 和 `GND2` 按数据手册连接，当前`GND2`网络名为`VSIO_GND`；
  `VSIO_GND`与USB/MCU GND不得直接连接，并通过CN1 Pin 2引出。
- CANH、CANL与隔离地改由独立3针连接器CN1引出：Pin 1 CANH、Pin 2 `VSIO_GND`、
  Pin 3 CANL；不得再把CAN网络接回非隔离20针接口。
- CA-IS2062VW的CANH/CANL具有器件级保护，但不等同于IEC 61000-4-2系统级TVS；
  本版已经决定不增加独立CAN TVS，产品对外ESD指标必须按整机实测结果声明。
- CAN终端采用可选分裂终端：CANH经60 ohm到中点、CANL经60 ohm到中点。两颗电阻必须
  同时接入或断开，禁止只接
  其中一颗。若空间优先，也可改为单颗120 ohm直接跨CANH/CANL；两种方案只能装一种。
  RBLink位于总线端点时装终端，作为中间节点时不装。
- CAN TVS、终端电阻及共模扼流圈全部位于隔离侧；TVS回流和分裂终端中点电容均接
  `VSIO_GND`。器件两侧去耦、VISO连接方式和隔离区布局必须照官方数据手册复核。
- CA-IS2062VW及其板内隔离区按数据手册保持隔离布局。CN1已经提供隔离地，但整机隔离
  等级仍需结合PCB爬电距离、电气间隙、连接器和外壳共同评估，不得只引用芯片额定值。

### 2.4 USB 接口保护

- USB D+、D- 和 VBUS 的 ESD 保护器件冻结为 `USBLC6-2SC6`。
- USBLC6-2SC6的VBUS参考脚连接到USB连接器侧、保险丝F1之前的原始`VBUS`；D+/D-保护器件
  紧靠连接器放置。该VBUS参考连接只用于ESD钳位，不作为板上负载供电路径。
- STM32F407 OTG FS使用芯片内部D+上拉，由USB外设的软件连接/断开控制完成枚举与重枚举。
  正常装配不使用外置1.5 kΩ D+上拉、S8050及`LT_USBRENUMN`控制电路；如希望首板保留回退，
  只能以DNP器件位保留，禁止与内部D+上拉同时启用。
- 本设计不使用PA9 VBUS sensing且设备由USB总线供电，固件必须禁用VBUS sensing并按
  DAPLink USB初始化/反初始化流程控制内部上拉；不得沿用STM32F103外置上拉枚举电路的软件逻辑。
- 本项目指定物料为 TECH PUBLIC（台舟），SOT-23-6，立创商城编号 `C2827654`；
  不得仅按同名型号替换成 ST 或其他厂商版本，替代时必须重新核对引脚和参数。
- 器件放在 USB 连接器之后、MCU/串联电阻之前，并尽可能靠近连接器；D+、D- 采用
  对称直通布线，不留长支路或测试焊盘残桩。
- ESD 器件 GND 使用最短、最宽连接并就近下地过孔；ESD 泄放回路不得绕过数字地平面
  或穿过 MCU/晶振区域。
- VBUS 保护脚连接受保护的 USB 5 V 网络，具体引脚必须按 C2827654 对应数据手册检查，
  不得照搬其他厂商 USBLC6-2SC6 的符号引脚。

### 2.5 目标调试接口 ESD 保护

- SWD/JTAG、SWO、nRESET、UART、SPI 和 I2C 的目标侧接口统一使用
  `TPD2E2U06DCKR-TP` 双通道 ESD 保护器件。
- 指定物料：TECH PUBLIC（台舟），SOT-323，立创商城编号 `C5350871`；当前网表增加PWM1/2后，
  单台整机用量为 **8颗（16个受保护信号通道）**。商品页面数字 `6138721` 不是立创料号。
- 通道分配冻结如下：

| 接口 | 受保护信号 | 双路器件数量 |
|---|---|---:|
| DAP/JTAG/SWO | nTRST、TDI、TMS/SWDIO、TCK/SWCLK、TDO/SWO、nRESET | 3 |
| UART | UART_TX、UART_RX | 1 |
| SPI | SPI_SCK、SPI_MOSI、SPI_MISO、SPI_CS | 2 |
| I2C | I2C_SCL、I2C_SDA | 1 |
| PWM | PWM1、PWM2 | 1 |
| **合计** | **16路** | **8** |

- 每颗器件的两个 I/O 通道应优先分配给同一连接器或同一接口，便于ESD回流和布局；
  推荐配对为 TCK/SWCLK与TMS/SWDIO、TDI与nTRST、TDO/SWO与nRESET、UART_TX/UART_RX、
  SPI_SCK/SPI_MOSI、SPI_MISO/SPI_CS、I2C_SCL/I2C_SDA、PWM1/PWM2。
- ESD 器件必须放在目标接口连接器与电平转换器之间并紧靠连接器；I/O 走线不留长支路，
  GND 引脚以最短、最宽路径连接地平面，并在器件旁就近设置接地过孔。
- 该器件标称反向截止电压 5.5 V、结电容 0.7 pF。创建或选用 EasyEDA 器件时，
  必须按 `C5350871` 对应数据手册复核 3 引脚定义和 SOT-323 焊盘，不得仅凭同名器件替换。
- USB仍使用2.4节指定的`USBLC6-2SC6`；CAN本版不增加独立TVS，`TARGET_POWER`输出保护
  留待后续版本。以上均不计入本节的8颗用量。

## 3. 目标可调电源（已冻结）

### 3.1 额定规格

- 输入：USB 5 V。
- 输出：0–5.000 V，软件和接口最大输出电流 300 mA。
- `0 V` 的定义：关闭稳压器并主动放电到安全电压，不要求闭环稳压到绝对 0 V。
- 上电默认关闭；USB 枚举、参数校验和外部 VTREF 检查通过后才允许开启。

### 3.2 已确认器件

| 功能 | 器件 | 厂商/封装 | 立创商城编号 | 状态 |
|---|---|---|---|---|
| Boost | `MT3608L` | AEROSEMI，按官方器件封装 | `C2932326` | 已冻结 |
| Boost功率电感 | `FTC201610S4R7MBCA` | cjiang，2016（2.0 mm x 1.6 mm x 1.0 mm） | `C5832346` | 已冻结 |
| 可调 LDO | `TLV75901PDRVR` | TI，WSON-6-EP 2 mm x 2 mm | `C544759` | 已冻结 |

不得把 MT3608、MT3608B 或兼容料默认替代 MT3608L；不得把 TLV758P 默认替代
TLV75901PDRVR。任何替代必须重新检查引脚、封装、反馈基准、限流和热设计。

### 3.3 电源路径

```text
USB_5V
  -> 输入保护/滤波
  -> MT3608L Boost（固定约 5.45 V）
  -> TLV75901PDRVR（STM32 DAC 控制 FB）
  -> 电压/电流检测与硬件过流关断
  -> 防反灌输出开关
  -> TARGET_POWER
```

- Boost 标称目标为 5.45 V；结合 FB 容差、反馈电阻、纹波和负载瞬态验证后，
  正常工作全条件不得超过 TLV759P 的 6.0 V 输入上限。
- MT3608L 反馈基准和外围按其数据手册计算，反馈电阻使用 0.1% 精度并远离 SW 节点。
- Boost 电感固定为 `FTC201610S4R7MBCA`：4.7 uH、正负20%、额定电流1.6 A、
  饱和电流2 A、DCR约190 mohm。该器件只按当前300 mA输出规格使用；若未来提高到
  1 A输出，必须重新选择电感和整个Boost功率级。
- MT3608L峰值限流必须低于电感2 A饱和电流并留出容差和温升裕量；OCP电阻按数据手册
  计算且保留修改位，首板通过启动、短路和负载瞬态测试后冻结最终阻值。
- 肖特基二极管和输入/输出电容必须通过样机纹波、温升和启动测试确认。
- MT3608L 的可调过流脚不得悬空照搬模块设计。该限流不能替代300 mA输出侧硬件限流。
- MT3608L与TLV75901P的EN均为高有效逻辑输入，分别使用独立STM32 GPIO控制；GPIO到EN
  建议串4.7–10 kΩ，每个EN节点用100 kΩ下拉，确保复位期间默认关闭。不使用P-MOS控制EN。
- 如需硬件故障强制关断，可用N-MOS将EN节点拉低，或由开漏故障输出直接拉低；串联电阻
  用于限制GPIO仍输出高电平时的冲突电流。Boost先开启并稳定后再开启LDO；关闭时先关LDO，
  再关闭Boost。两个EN不得直接并联为同一个网络。

### 3.4 TLV759P 控制与保护

```text
STM32 PA4 / DAC1_OUT -> 隔离电阻/补偿网络 -> TLV759P FB
STM32 PD0            -> 串联电阻/默认下拉  -> TLV759P EN
VTREF                -> 精密分压 + RC      -> STM32 PA2 / ADC123_IN2
TARGET_POWER         -> 精密分压 + RC      -> STM32 PA1 / ADC123_IN1
NTC                  -> 精密分压 + RC      -> STM32 PA0 / ADC123_IN0
硬件故障比较器输出                        -> 当前网表未实现
```

- TLV75901PDRVR 额定1 A只是器件余量，LTLink V3.0 对外仍限制300 mA。
- 连续调节范围按0.55–5.000 V设计；0 V通过EN关闭和芯片主动放电实现。
- DAC向FB注入控制量，输出与DAC码值的方向、全量程阻值和补偿电容必须计算并仿真；
  不允许把DAC直接接FB后凭软件试值。
- EN外接下拉，MCU复位、固件升级、看门狗复位和硬件故障期间保持关闭。
- 电压设定后必须由独立ADC回读，不允许用DAC码值代替实际输出测量。
- 300 mA限流必须包含不依赖固件的快速硬件关断；TLV759P内部限流不能代替该功能。
- 目标板已有外部VTREF时默认禁止内部电源开启；输出端必须防反灌并在关闭状态高阻。
- 校准数据包含电压增益/偏移、电流增益/偏移和CRC；软件硬上限为5000 mV/300 mA。

#### 3.4.1 300 mA保护链路（V3.0本版明确不装）

```text
MT3608L 5.45 V
  -> TPS2553可调限流开关（建议设定330 mA，覆盖器差后保证额定300 mA）
  -> TLV75901PDRVR
  -> 0.10 ohm / 1% / >=0.25 W高侧采样电阻
  -> 背靠背P-MOS输出隔离开关
  -> TARGET_POWER

采样电阻两端 -> INA180A3（增益100） -> 待分配ADC（PA5已被UART_PORTEN占用）
INA180输出 -> 硬件比较器（约3.0 V阈值） -> 故障锁存/开漏下拉 -> TLV759 EN
                                                     -> 待分配故障输入
```

- 将可调限流开关放在固定5.45 V节点与LDO之间，可避免多数集成负载开关在低于1.6–2.5 V时
  无法工作的限制，同时对LDO短路和目标输出短路提供硬件限流。最终限流电阻必须按所选
  TPS2553具体后缀的数据手册与容差重新计算。
- V3.0本版已决定不增加TPS2553、INA180、比较器、故障锁存和背靠背MOS；产品规格不得
  宣称具备300mA硬件恒流、短路关断或输出防反灌，300mA作为软件限制和推荐负载上限。
- 0.10 ohm与INA180A3组合在300 mA时理论输出约3.0 V，适合3.3 V ADC；采样采用Kelvin走线，
  ADC前增加100 ohm + 10 nF RC。若改用其他增益，必须重新核算ADC满量程和比较器阈值。
- 比较器故障不得只依赖固件轮询；故障应能直接拉低TLV759 EN。建议使用锁存方式，需由MCU
  明确清除后才能重新开启，避免短路时周期性自动重启。
- 最终输出使用背靠背P-MOS实现关闭高阻和防目标板反灌；MOS耐压建议>=12 V，按
  `VGS=-2.5 V`条件选择低导通电阻器件。栅极应有默认关闭电阻，控制电路不得在MCU复位时误导通。
- TLV759在低输出电压、300 mA时功耗较大，硬件限流不代替温度降额；PA0 NTC用于热保护。

### 3.5 热降额

固定约5.45 V输入时，LDO在300 mA负载下的理论耗散约为：

| 输出 | LDO理论耗散 |
|---:|---:|
| 5.0 V | 0.14 W |
| 3.3 V | 0.65 W |
| 1.8 V | 1.10 W |
| 1.0 V | 1.34 W |
| 0.55 V | 1.47 W |

- WSON裸露焊盘必须完整连接大面积GND铜皮并布置足够热过孔。
- 低输出电压必须根据实测结温/板温做电流或时间降额；不能宣称全范围连续300 mA，
  除非热测试证明满足最高环境温度和结温裕量。
- 在TLV759P邻近位置预留NTC或板温采样位置。

## 4. MCU实际引脚基线（2026-08-25网表）

| 模块 | 引脚 | 说明 |
|---|---|---|
| USB OTG FS | PA11 DM、PA12 DP | DAP v2 Bulk + CDC |
| USB VBUS检测 | 不使用专用GPIO检测 | PA9改作USART1_TX；USB设备采用VBUS常在/禁用内部VBUS sensing方案，原理图与固件必须一致 |
| RGB LED | PC1数据、PC2状态 | 经1 kΩ驱动LED阳极，GPIO高有效 |
| 目标UART | PA9 TX、PA10 RX | USART1，AF7，可用DMA |
| 板载调试UART | PB10 TX、PB11 RX | USART3，AF7；3.3V维护接口，不经过目标侧电平转换 |
| DAP nTRST/TDI | PD15/PD14 | 普通高速GPIO输出，经TXU0304到目标接口 |
| DAP SWDIO/TMS输出 | PD13 | `T_JTMS_SWDIO-1`，独立输出GPIO |
| DAP SWDIO输入 | PD12 | `T_JTMS_SWDIO-2`，独立输入GPIO |
| DAP SWDIO方向 | PE10 | `DIR_SPI_JTMS_SWDIO`，控制SN74LVC1T45 DIR |
| DAP SWCLK/TCK | PD11 | 普通高速GPIO输出 |
| DAP JTDO输入 | PD10 | `T_JTDO_SWO-1`，JTAG模式输入 |
| SWO输入 | PD9 | `T_JTDO_SWO-2`，SWD模式接收SWO |
| 目标nRESET控制 | PD8 | 驱动2N7002K开漏下拉；MOS栅极100kΩ下拉 |
| 目标SPI | PB12 NSS、PB13 SCK、PB14 MISO、PB15 MOSI | SPI2，AF5 |
| 无线模块SPI | PB3 SCK→ESP GPIO4、PB4 MISO←ESP GPIO6、PB5 MOSI→ESP GPIO5 | SPI1，AF5；STM32为Master、ESP32-C3为Slave，工程目标40MHz |
| 无线模块控制 | PB6 ESP_CS→ESP GPIO7、PB7 ESP_READY←ESP GPIO10、PE0 ESP_EN→CHIP_EN | ESP_CS低有效；ESP_READY接EXTI7；ESP_EN网络外接上拉 |

ESP固件位于 `esp32c3/`。其中ESP_READY的定版语义是“ESP SPI从机DMA事务已经排队，可开始一次固定512字节传输”：事务准备后拉高、CS传输完成后拉低。它不是单纯的数据待取指示；STM32在每次传输前都必须等待该脚为高。
| 外部NOR Flash | PC12 SCK、PC11 MISO、PC10 MOSI、PA15 FLASH_CS | SPI3，AF6；FLASH_CS为GPIO低有效 |
| 目标I2C | PA8 SCL、PC9 SDA | I2C3，AF4 |
| 目标PWM | PC6 PWM1、PC7 PWM2 | TIM8_CH1/CH2，AF3，经U12电平转换输出 |
| CAN1 | PB8 RX、PB9 TX | AF9；接CA-IS2062VW逻辑侧，CAN改由独立CN1引出 |
| VTREF检测 | PA2 ADC123_IN2 | 精密分压与RC后输入 |
| 输出电压ADC | PA1 ADC123_IN1 | TARGET_POWER精密分压与RC后输入 |
| 电源区NTC采样 | PA0 ADC123_IN0 | 监测TLV759及可调电源区域板温 |
| 电源DAC | PA4 DAC1_OUT | TLV759P FB模拟设定 |
| Boost/LDO使能 | PD1/PD0 | `BOOST_POWEREN`/`LDO_POWEREN`；各有100kΩ硬件下拉 |
| JTAG/UART/I2C端口使能 | PA7/PA5/PA6 | 对应`*_PORTEN`，高有效 |
| SPI端口使能 | PE11 | `SPI_PORTEN`，高有效 |
| 目标电压选择状态 | PE7 | `V_TRGT_SWITCH`，TPS2116状态输出采样 |
| 侧按键 | PE2 | `HARDKEY`，100kΩ上拉，按下接地 |

- 连接器Pin 13的`PORT_JTDO_SWO`分成两路输入：`PD10/T_JTDO_SWO-1`用于JTAG TDO，
  `PD9/T_JTDO_SWO-2`用于SWO；两个GPIO始终保持输入模式。
- 连接器Pin 7的`PORT_JTMS_SWDIO`在MCU侧拆成`PD13/T_JTMS_SWDIO-1`输出和
  `PD12/T_JTMS_SWDIO-2`输入，以减少固件切换GPIO模式的延时。SWD turnaround协议周期仍须保留，
  固件必须先释放输出通道再采样输入通道。

以上是原理图网络分配基线；封装、电源脚、启动脚、晶振、USB、调试口及全部AF映射，
在画图前仍需使用STM32CubeMX和STM32F407VGT6数据手册逐项复核。

### 4.1 端口使能默认状态

- `JTAG_PORTEN`(PA7)、`UART_PORTEN`(PA5)、`SPI_PORTEN`(PE11)、`I2C_PORTEN`(PA6)分别连接到
  对应电平转换器的OE/EN，每根控制线上独立增加100 kΩ到系统GND的下拉电阻。
- TXU0304的OE为高有效：OE=0时所有输出高阻，OE=1时正常传输。PI4ULS5V202的EN必须按
  实际采购料号数据手册复核有效电平；当前设计按高有效处理。
- 下拉电阻放在电平转换器OE/EN引脚附近。GPIO与控制脚之间可预留0–100 ohm串联电阻，
  不使用上拉到3.3 V，不允许四路共用一个下拉电阻。
- 最新TEL网表确认R59/R60/R63/R67均为100kΩ；I2C目标侧R73/R74均为4.7kΩ。
- 固件启动顺序为：GPIO先输出低电平，确认`VTREF/V_TRGT`有效并完成协议配置后，再将对应
  PORTEN置高；关闭接口时先置低，使目标侧进入高阻。

## 5. AL/EasyEDA 交付要求

- 原理图至少分页为：USB与主控、DAP与电平转换、UART/SPI/I2C接口、隔离CAN、
  无线模块、可调电源与保护、连接器与ESD。
- 所有已冻结器件必须使用准确制造商料号、立创编号、符号和封装，不使用模糊通用器件。
- 电源页必须标注关键额定值、反馈公式、默认开关状态、测试点和未装/可调器件。
- 预留测试点：USB_5V、BOOST_5V45、TARGET_POWER、DAC_SET、LDO_FB、CURRENT_SENSE、
  VTREF、POWER_FAULT、VSIO_GND。
- PCB布局时SW节点最小化，Boost高di/dt环路与DAC/FB/ADC区域分开；FB走线不得经过
  电感、SW铜皮下方或靠近USB差分对。
- 在开始PCB前完成ERC、网络名检查、电源默认状态检查和BOM中冻结料号核对。

## 6. 尚未冻结，不得由AL自行决定

- 300 mA输出硬件限流、电流检测放大器、防反灌负载开关和外部温度检测具体型号。
- MT3608L肖特基二极管、OCP电阻以及全部补偿/滤波器件的最终料号。
- DAC注入FB网络的最终阻值和控制极性，必须计算、仿真并经样机闭环验证后冻结。

## 7. 2026-08-25网表投板前检查项

以下项目来自对导出网表和整页PDF的交叉检查，不属于附件中的设计说明：

1. **I2C器件已经确认。** U11继续使用`PI4ULS5V202UEX/C481697`，不换PCA9617；
   R73/R74按最新TEL网表均为4.7kΩ。
2. **自定义20针符号暂不处理。** JP1继续沿用标准JTAG外观，只以Pin号和实际网络定义接口；
   后续检查不得根据符号内部的GND/NC文字判断功能。
3. **阻容值继续补齐。** 最新TEL已提供部分值；仍显示`{Value}`的器件等用户后续给值再冻结。
4. **PORTEN默认状态已确认。** R59/R60/R63/R67均为100kΩ下拉，复位时接口关闭。
5. **CAN方案已确认。** 不增加独立TVS；R61/R68为2×60Ω分裂终端。TEL中的C45为470nF，
   与先前讨论值不同，投板前只需再确认该中点电容是否确实采用470nF。
6. **电源保护范围已确认。** V3.0本版不增加300mA硬件限流、故障锁存和输出防反灌链路。
7. **ESD数量已经变化。** Q2-Q9共8颗双通道器件，新增Q9保护PWM1/PWM2；BOM不能继续沿用7颗。
8. **固件引脚已经按本表迁移。** `RBLink_DAP_V3.0/App`、CMSIS-DAP和USB复合设备已由
   Keil ARMCC 5.06 完整链接，结果为0错误、0警告；下一阶段只剩实板枚举、电气和时序验证。
