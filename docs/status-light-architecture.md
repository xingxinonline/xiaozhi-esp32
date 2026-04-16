# 状态灯架构设计

## 1. 文档目的

本文档用于固化 xiaozhi-esp32 项目的状态灯架构设计，明确状态灯在系统中的职责、分层边界、语义模型与板级适配方式。

本文档的目标不是定义某一块板子的具体闪烁参数，而是先建立一套稳定、可扩展、可复用的状态灯架构，使后续不同硬件形态可以在统一语义下各自实现。

本文档是架构与方案文档，不包含代码改动。

## 2. 背景与问题

当前项目已经具备设备状态机，并且应用主循环会在状态切换和语音活动变化时触发 LED 刷新。这说明状态灯已经具备基本接入点，但整体实现仍然存在明显的结构问题。

当前主要问题如下：

1. 状态灯驱动直接读取 Application 全局状态，硬件层反向依赖业务层，边界不清晰。
2. 单灯、GPIO 灯、灯环各自维护一套状态到灯效的映射，容易漂移。
3. LED 抽象层只提供无参的 OnStateChanged 接口，无法表达更细粒度的灯语上下文。
4. 状态灯目前只能看到粗粒度 DeviceState，网络异常、恢复中、监听期人声活跃等语义没有独立建模。
5. 不同板型能力差异很大，当前实现把颜色、亮度、闪烁频率和业务状态直接绑死，不利于无屏单色灯板和 RGB 灯板共存。
6. 系统级指示和会话级指示没有解耦，后续一旦叠加低电量、升级、恢复中等语义，容易互相覆盖。

同时，参考分支的实现也给出一个值得借鉴的方向：

1. 状态灯应该由状态变化驱动，而不是周期性轮询。
2. 监听期应区分静默监听和检测到人声后的活跃监听。
3. 网络断开、恢复、升级等系统态应该具有比普通会话态更高的优先级。

但这些优点应该被吸收到统一架构中，而不是继续沿用“每个灯驱动自己解析业务状态”的模式。

## 3. 设计目标

### 3.1 目标

1. 将状态灯从具体硬件控制中抽象出来，建立统一的语义层。
2. 保证状态灯的输入来源清晰，只从系统状态与事件中推导，不反向读取杂散业务逻辑。
3. 支持单色灯、RGB 灯、灯环等多种硬件能力，并允许能力退化。
4. 让会话状态、系统状态和异常状态具备清晰优先级。
5. 第一阶段优先覆盖 80% 的核心状态语义，不提前堆叠复杂叠加机制。
6. 兼容当前 zhiban-ai-reader 这类无屏、双色、低电流 GPIO 状态灯板。

### 3.2 非目标

1. 本阶段不追求所有板型拥有完全一致的视觉效果。
2. 本阶段不为每一种异常都定义独立灯语。
3. 本阶段不引入任意多层 overlay、动画脚本引擎或通用 DSL。
4. 本阶段不要求状态灯直接承载所有用户交互反馈。

## 4. 设计原则

### 4.1 单一事实来源

状态灯的语义必须由统一控制器生成，不能由多个硬件驱动分别推导。

### 4.2 语义与呈现分离

业务层只定义“当前应该表达什么”，硬件层只负责“如何在这块板子上表达出来”。

### 4.3 优先级显式化

升级、错误、恢复中等系统态必须显式高于普通会话态，避免通过调用顺序隐式覆盖。

### 4.4 能力退化优先于功能分叉

当某块板只有单色灯时，应将统一语义退化为亮度、呼吸、闪烁节奏，而不是为该板单独发明另一套状态模型。

### 4.5 事件驱动优先

状态灯应在状态变化或关键上下文变化时刷新，不应依赖主动轮询 Application 全局状态。

## 5. 总体设计结论

推荐将状态灯系统拆分为四层：

1. 状态源层
2. 状态灯语义层
3. 灯光渲染层
4. 板级装配层

各层职责如下：

### 5.1 状态源层

职责：向状态灯系统提供统一输入。

本层只负责产生事实，不负责决定最终灯效。建议纳入以下输入：

1. 设备状态
2. 语音活动状态
3. 网络健康状态
4. 系统任务状态

其中：

1. 设备状态来自 DeviceStateMachine。
2. 语音活动状态来自 AudioService 的 VAD 回调。
3. 网络健康状态来自网络连接与断开事件。
4. 系统任务状态包括启动、配网、激活、升级等阶段性流程。

### 5.2 状态灯语义层

职责：根据状态源计算当前应该呈现的灯语场景。

本层新增统一控制器，建议命名为 StatusLightController。它维护一个 StatusLightContext，并对外输出单一的 LightScene。

本层是整个架构的核心。它不直接操作 GPIO，不直接设置像素颜色，也不应依赖具体灯型。

### 5.3 灯光渲染层

职责：将 LightScene 转换为具体硬件行为。

不同硬件形态分别实现各自的 renderer，例如：

1. MonochromeLightRenderer
2. RgbPixelLightRenderer
3. CircularStripLightRenderer

本层只吃统一场景，不自行读取 Application 状态。

### 5.4 板级装配层

职责：在板级代码中声明当前板卡使用哪一种 renderer，以及是否存在多颗灯。

Board 只负责返回合适的状态灯组件实例，不负责拼装业务语义。

## 6. 核心数据模型

### 6.1 StatusLightContext

StatusLightContext 表示状态灯决策所依赖的最小上下文。第一阶段建议包含以下字段：

1. device_state
2. voice_active
3. network_connected
4. recovering_network
5. transient_overlay

说明如下：

1. device_state 表示当前 DeviceState。
2. voice_active 表示监听态下是否检测到人声。
3. network_connected 表示当前网络是否在线。
4. recovering_network 表示当前是否处于异常恢复或重连阶段。
5. transient_overlay 表示未进入 DeviceStateMachine、但需要临时影响灯语的系统附加语义，例如 low_battery_warning、charging 或其它高优先级提醒。

### 6.1.1 字段归属规则

为避免状态灯出现多重事实来源，本文档额外做如下约束：

1. Starting、WifiConfiguring、Activating、Upgrading 等已经进入 DeviceStateMachine 的流程，只能由 device_state 表达，不得再通过 transient_overlay 重复编码。
2. recovering_network 只用于“设备已经进入过可用网络状态，当前断网并正在尝试恢复”的场景，不用于首次开机建网或配网阶段。
3. transient_overlay 只承载不进入状态机、但需要临时压过基础灯语的附加系统语义；第一阶段只建议为 low_battery_warning 和 charging 预留。
4. LightScene 的计算必须保持纯函数特性：相同的 StatusLightContext 输入，必须得到相同的场景输出。

### 6.2 LightScene

LightScene 是语义层向渲染层输出的统一场景。第一阶段建议使用枚举而不是复杂脚本对象，保持简单和稳定。

建议定义以下基础场景：

1. Booting
2. WifiConfiguring
3. Activating
4. IdleReady
5. Connecting
6. ListeningPassive
7. ListeningActive
8. Speaking
9. Upgrading
10. RecoveringOrError

这 10 个场景已经覆盖当前项目 80% 以上的核心状态表达需求。

### 6.3 ScenePriority

场景优先级必须显式定义，避免由调用顺序决定最终表现。

推荐优先级从高到低如下：

1. Upgrading
2. RecoveringOrError
3. WifiConfiguring
4. Activating
5. Connecting
6. Speaking
7. ListeningActive
8. ListeningPassive
9. IdleReady
10. Booting

说明：

1. Upgrading 和 RecoveringOrError 属于强系统态，必须压过会话态。
2. ListeningActive 高于 ListeningPassive，确保用户说话时反馈立即增强。
3. IdleReady 是稳定基态，只在没有更高优先级场景时出现。

## 7. 状态源到场景的映射规则

### 7.1 启动阶段

当设备处于 Starting 时，输出 Booting。

### 7.2 配网阶段

当设备处于 WifiConfiguring 时，输出 WifiConfiguring。

### 7.3 激活阶段

当设备处于 Activating 时，输出 Activating。

### 7.4 空闲阶段

当设备处于 Idle，且网络在线、没有恢复任务、没有系统任务时，输出 IdleReady。

### 7.5 连接阶段

当设备处于 Connecting 时，输出 Connecting。

该场景在板级渲染上必须与 ListeningPassive 可区分，避免用户无法判断设备是在建链还是已经进入可说话状态。

### 7.6 监听阶段

当设备处于 Listening 或 AudioTesting 时：

1. 如果 voice_active 为 false，输出 ListeningPassive。
2. 如果 voice_active 为 true，输出 ListeningActive。

### 7.7 播报阶段

当设备处于 Speaking 时，输出 Speaking。

### 7.8 升级阶段

当设备处于 Upgrading 时，输出 Upgrading。

### 7.9 恢复与错误阶段

当网络断开并进入恢复流程，或系统明确进入错误状态时，输出 RecoveringOrError。

这里需要强调，RecoveringOrError 不要求第一阶段区分“短暂断网告警”和“不可恢复故障”。对当前系统而言，先统一表达为高优先级告警场景更稳妥。

## 8. 组件边界设计

### 8.1 Application 的职责

Application 负责发布状态灯相关事件，但不再直接命令某一类 LED 如何闪烁。

Application 后续只做以下事情：

1. 在设备状态变化时通知 StatusLightController。
2. 在 VAD 状态变化时更新 voice_active。
3. 在网络连接和断开时更新 network_connected 与 recovering_network。
4. 在不进入 DeviceStateMachine 的附加系统语义变化时更新 transient_overlay；启动、配网、激活、升级仍由 device_state 表达。

### 8.2 StatusLightController 的职责

StatusLightController 负责：

1. 聚合上下文。
2. 计算当前场景。
3. 处理优先级。
4. 将场景下发给 renderer。

它不应负责：

1. 播放音效。
2. 修改显示文案。
3. 管理协议连接。
4. 决定设备状态迁移。

### 8.3 LightRenderer 的职责

LightRenderer 负责：

1. 将 LightScene 映射为本硬件可表达的亮度、颜色或动画。
2. 在硬件支持范围内尽量保持统一语义。
3. 在能力不足时做保真度退化，而不是改变场景含义。

它不应负责：

1. 推导当前系统处于什么业务状态。
2. 自行查询 Application 单例。
3. 维护复杂业务规则。

### 8.4 线程模型与唯一写入口

为避免状态灯被多个线程并发修改，本文档明确做如下决策：

1. StatusLightController 由 Application 持有。
2. 只有 Application 主事件循环可以写入 StatusLightContext、计算 LightScene 并调用 renderer。
3. 网络、音频、协议、按钮等回调只负责投递事件或通过 Schedule 切回主循环，不得直接触发灯效切换。
4. renderer 内部可以使用 LEDC、RMT、esp_timer 等机制驱动动画，但场景切换入口必须保持单线程。

### 8.5 接口契约

第一阶段为了降低改动面，Board::GetLed 的获取方式可以保持不变，但 Led 的职责必须从“状态感知驱动”收口为“纯场景渲染器”。

推荐接口如下：

```cpp
enum class LightScene {
    Booting,
    WifiConfiguring,
    Activating,
    IdleReady,
    Connecting,
    ListeningPassive,
    ListeningActive,
    Speaking,
    Upgrading,
    RecoveringOrError,
};

struct LightCalibrationProfile {
    uint8_t channel_a_max_duty_percent;
    uint8_t channel_b_max_duty_percent;
    uint8_t mixed_max_duty_percent;
    uint16_t breathe_period_ms;
    uint16_t blink_fast_ms;
    uint16_t blink_slow_ms;
    uint16_t connect_pulse_ms;
};

class Led {
public:
    virtual ~Led() = default;
    virtual void ApplyScene(LightScene scene) = 0;
};

class StatusLightController {
public:
    void SetContext(StatusLightContext context);
    LightScene ComputeScene() const;
    void Flush(Led& renderer);
};
```

配套约束如下：

1. Led 实现不得再主动读取 Application 单例。
2. 校准参数由板级 renderer 在构造期持有，不由 Application 在运行时动态推导。
3. 现有 OnStateChanged 只允许作为迁移期兼容壳存在，迁移完成后应删除。

## 9. 板级适配策略

### 9.1 单色灯板

单色灯无法表达颜色语义，因此需要用以下维度承载统一场景：

1. 常亮或熄灭
2. 亮度高低
3. 呼吸效果
4. 闪烁频率
5. 节奏模式

对单色灯来说，语义映射的重点不是区分颜色，而是区分：

1. 系统忙碌
2. 可交互待机
3. 正在监听
4. 正在播报
5. 告警或恢复中

### 9.2 RGB 灯板

RGB 灯除了亮度和节奏，还可叠加颜色区分场景。

但颜色只是一种增强表达，不应成为上层语义定义的必需条件。

### 9.3 灯环板

灯环除了颜色和亮度，还可表达空间方向和流动感，例如：

1. 启动阶段使用滚动动画。
2. 监听阶段使用整环呼吸。
3. 连接阶段使用固定亮度常亮。
4. 告警阶段使用高频整体闪烁。

即便如此，灯环也必须服从统一 LightScene，而不是单独维护另一套业务状态表。

### 9.4 双色共阳 LED 板

部分板型并不是单色灯，也不是空间上可分离的两颗状态灯，而是一个双色 LED 封装，内部包含两路可独立驱动的发光通道。

这类板型的推荐做法是：由同一个 renderer 在统一场景下同时控制两路颜色通道，并向上仍然暴露一个统一的状态灯语义。

其特点如下：

1. 用户看到的是一个发光点，而不是两颗可以分工的独立指示灯。
2. 上层语义更适合映射为颜色加节奏，而不是“左灯表示系统、右灯表示会话”。
3. 两路通道可以组合出第三种混合色，因此适合表达正常、对话中、告警、升级等高区分度状态。

对于这类板型，renderer 应负责：

1. 处理共阳极或 active-low 等电气细节。
2. 维护两路颜色通道的 PWM、闪烁和呼吸。
3. 将统一 LightScene 映射为单个可见光点的综合色彩表现。

### 9.5 板级校准参数

为避免 PWM 常量、亮度上限和节奏参数散落在 renderer 内部，每种灯型都应持有板级校准参数。

第一阶段建议最少包含以下字段：

1. channel_a_max_duty_percent
2. channel_b_max_duty_percent
3. mixed_max_duty_percent
4. breathe_period_ms
5. blink_fast_ms
6. blink_slow_ms
7. connect_pulse_ms

对 zhiban-ai-reader 而言，channel_a 和 channel_b 最终可以直接落为 blue 和 brilliant red 两个颜色通道。

## 10. zhiban-ai-reader 的推荐灯语设计

zhiban-ai-reader 当前更适合建模为一颗双色共阳 LED，而不是两颗分离指示灯。

当前已知器件料号为 12-22/BHR6C-A01/2C。结合用户提供的规格摘录，可以确认该器件属于 multi-color SMD LED，颜色组合为 BH = Blue、R6 = Brilliant Red，适合做设备状态指示灯。

从原理图可以确认以下硬件事实：

1. GPIO39 和 GPIO38 各自驱动 LED 封装内的一路颜色通道。
2. 两路通道的阳极接 3.3V，GPIO 侧通过 2.2k 电阻串联后灌电流点亮，因此属于 active-low 结构。
3. 用户最终看到的是同一个发光点的变色与闪烁，而不是两个空间分离的提示灯。
4. 当前资料已经确认它是蓝加亮红双色 multi-color 器件；结合现有板级代码只驱动 STATUS_LED_GPIO(GPIO39) 且设备实测点亮蓝灯，可以确认 GPIO39 对应 Blue，GPIO38 对应 Brilliant Red。

结合这类硬件特征，zhiban-ai-reader 的推荐实现不是“双灯分工”，而是“双色 renderer”。

板级代码可以继续保留两个 GPIO 宏，但在状态灯架构中应把它们视为一个灯的两路颜色通道，而不是两个独立状态灯对象。

推荐灯语如下：

| 场景              | 推荐颜色/节奏 | 说明                         |
| ----------------- | ------------- | ---------------------------- |
| Booting           | 蓝色快闪      | 表示系统上电初始化           |
| WifiConfiguring   | 蓝色慢闪      | 表示等待配网或配网中         |
| Activating        | 紫色慢闪      | 表示设备激活或初始化服务能力 |
| IdleReady         | 蓝色呼吸      | 表示设备在线且待命           |
| Connecting        | 蓝色双脉冲    | 表示会话连接建立中           |
| ListeningPassive  | 蓝色常亮      | 表示已进入监听但用户未讲话   |
| ListeningActive   | 蓝色脉冲增强  | 表示检测到用户讲话           |
| Speaking          | 红色常亮      | 表示设备正在播报             |
| Upgrading         | 紫色快闪      | 表示升级进行中               |
| RecoveringOrError | 红色快闪      | 表示断网恢复或异常告警       |

说明如下：

1. 器件颜色组合现已确认是蓝色通道加亮红通道。
2. GPIO39 已可绑定为蓝色通道，GPIO38 已可绑定为亮红通道。
3. 蓝色和亮红同时点亮时，可在视觉上形成紫色系混合色，用于激活和升级这类系统态。

结合当前实现还可以进一步确认：

1. 现有板级代码通过单通道 GpioLed 只驱动 STATUS_LED_GPIO，也就是只驱动蓝色通道。
2. 这就是为什么当前代码下设备只能亮蓝灯，而不会出现红灯或紫灯。
3. 要落地文档里的完整灯语，后续必须把当前单通道实现替换为真正的双色 renderer。

这里额外做一条产品语义决策：

1. Connecting 不再使用蓝色常亮，而使用蓝色双脉冲。
2. ListeningPassive 保留蓝色常亮。
3. 这样用户可以直接区分“正在建链”和“已经进入可说话状态”。

这个映射优先保证用户能区分：

1. 设备是否可对话。
2. 当前是否正在听。
3. 当前是否正在说。
4. 当前是否处于异常或恢复中。

### 10.1 关于 active-low 与电流约束

由于原理图显示两路 LED 都是通过 GPIO 下拉灌电流点亮，因此实现时需要注意：

1. 逻辑上的“亮”在驱动层对应的是输出低电平或低占空比灌电流。
2. BH 蓝色通道的绝对最大正向电流为 10mA，R6 亮红通道为 25mA，两路电气能力不同，renderer 需要支持按通道独立限幅与亮度校准。
3. 即便板上已有 2.2k 串联电阻，也不应假设两路灯在同一 PWM 占空比下具有相同亮度或相同安全余量。
4. 第一阶段应优先使用常亮、闪烁、呼吸节奏区分语义，而不是依赖非常细的亮度分级。

### 10.2 关于颜色通道分工

既然硬件上已经明确存在两路颜色通道，文档建议不再把 zhiban-ai-reader 按双离散灯理解，而是将其作为双色 LED 的板级推荐实现。

但这里的颜色通道使用仍然保持简单：

1. 正常待机、监听、连接优先使用蓝色通道。
2. 播报和错误优先使用亮红通道。
3. 升级和激活等系统态允许双通道同时点亮形成紫色系混合色。

这仍然符合 80/20 原则，因为它只是把既有双色硬件做了统一抽象，没有引入额外状态模型。

## 11. 演进路线

### 11.1 兼容迁移策略

为了避免一次性改穿所有板型，迁移顺序应明确如下：

1. 保持 Board::GetLed 获取方式不变，先不扩大板级 API 变更面。
2. 先在 Application 内引入 StatusLightController，并把状态、VAD、网络等输入收口为 StatusLightContext。
3. 再将 Led 接口从 OnStateChanged 迁移为 ApplyScene，旧接口只保留短期兼容壳。
4. 以 zhiban-ai-reader 作为首个双色 renderer 落地板型。
5. 在 zhiban-ai-reader 跑通后，再逐步迁移 single_led、gpio_led、circular_strip 等现有实现。
6. 所有板型迁移完成后，删除 OnStateChanged 和驱动层反查 Application 的旧模式。

### 11.2 第一阶段

目标：完成统一语义收口。

内容：

1. 定义 StatusLightContext。
2. 定义 LightScene。
3. 引入 StatusLightController。
4. 让现有 LED 驱动改为 renderer 风格消费 LightScene。
5. 在 zhiban-ai-reader 上先完成双色 GPIO 灯验证。

### 11.3 第二阶段

目标：细化系统态与会话态。

内容：

1. 区分网络短时断开与持续恢复中。
2. 增加低电量或充电状态的可选场景。
3. 细化双色语义下的异常覆盖和低电量占位规则。

### 11.4 第三阶段

目标：扩展到更多灯型。

内容：

1. 统一 RGB 板和灯环板的 renderer 实现。
2. 复用同一套场景到多板型。
3. 只在 renderer 层保留灯型差异。

## 12. 风险与约束

### 12.1 状态模型不完整的风险

如果网络异常、恢复中、transient_overlay 等状态仍散落在 Application 各处，而没有统一进入 StatusLightContext，那么状态灯仍然会退回到“靠调用时机拼运气”的模式。

### 12.2 优先级失控的风险

如果继续允许不同模块直接触发 LED 动画，而不经过 StatusLightController，后续一定会出现升级灯效被会话态覆盖、告警灯效被 idle 覆盖的问题。

### 12.3 过度设计的风险

如果第一阶段就引入多层 overlay、脚本式动画描述或高度泛化配置，会显著增加实现复杂度，并拖慢当前主线演进。

因此本设计明确采用：

1. 有限场景枚举。
2. 显式优先级。
3. 渲染层能力退化。

## 13. 验收标准

后续实现可以按以下标准验证：

1. 任意板型的 LED 驱动不再主动读取 Application 状态。
2. 状态到灯语的映射只维护一份，不再分散在多个灯驱动中。
3. Listening 阶段能区分静默监听与活跃监听。
4. 网络异常和恢复中具备独立高优先级场景。
5. zhiban-ai-reader 可以在双色 GPIO 灯上清晰表达待机、监听、播报、升级和异常等核心语义。
6. 所有场景计算与 renderer 切换都只发生在 Application 主事件循环中。

## 14. 结论

状态灯架构的核心，不是增加更多颜色和动画，而是先把“状态语义”与“硬件呈现”拆开。

对当前项目而言，最合适的路径不是继续在各个 LED 驱动里补分支，而是建立统一的 StatusLightController，让状态机、VAD、网络和系统任务共同生成一个明确的 LightScene，再由不同板型按自身能力渲染。

这条路径既能兼容 zhiban-ai-reader 这样的双色无屏 GPIO 灯板，也能为 RGB 灯和灯环板提供稳定的长期演进基础。
