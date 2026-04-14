# 对话模式控制 — 架构设计与推进计划

## 1. 引言

本文档基于已有的 [交互模式架构方案](../docs/interaction-mode-architecture.md) ，将其从方案文档推进到可执行的实施计划，覆盖三个能力：

1. **本地按键切换对话模式**（主模式 realtime ↔ auto、输入策略 toggle ↔ ptt）
2. **远程配置切换对话模式**（MCP 工具查询/设置主模式、输入策略、AEC）
3. **双色 LED 状态灯**（landoubao-dev 板型的 DualColorLed 实现）

---

## 2. 现状与可行性分析

### 2.1 当前代码基础

| 能力                   | 现状                                                                                                  | 差距                                                   |
| ---------------------- | ----------------------------------------------------------------------------------------------------- | ------------------------------------------------------ |
| **主模式概念**         | 不存在独立主模式字段；`GetDefaultListeningMode()` 硬绑 AEC 状态决定 auto/realtime                     | 需引入 `chat.main_mode` 独立字段                       |
| **输入策略概念**       | 不存在；按键回调直接调 `ToggleChatState()`                                                            | 需引入 `chat.button_strategy` 字段 + 板级按键 dispatch |
| **ListeningMode 枚举** | 已有 `kListeningModeAutoStop / kListeningModeManualStop / kListeningModeRealtime`                     | 无需扩展                                               |
| **Pending 机制**       | `SetPendingListeningMode()` + `GetAndClearPendingListeningMode()` 已支持一次性覆盖                    | 可复用                                                 |
| **按钮系统**           | `Button` 类已支持 `OnClick / OnDoubleClick / OnMultipleClick / OnPressDown / OnPressUp / OnLongPress` | 能力完整                                               |
| **MCP 框架**           | `McpServer` 单例 + `McpTool` 注册式；已有 `AddCommonTools() / AddUserOnlyTools()`                     | 需新增 mode/strategy/aec 查询设置工具                  |
| **NVS 持久化**         | `Settings` 类已封装 Get/Set String/Int/Bool                                                           | 可直接使用                                             |
| **LED 体系**           | `Led` 基类 → `SingleLed / GpioLed / CircularStrip / NoLed`；缺少 `DualColorLed`                       | 需新增 `DualColorLed` 类                               |
| **状态灯映射**         | `OnStateChanged()` 基于 11 种 `DeviceState` 映射颜色/动画                                             | DualColorLed 需实现独立的映射表                        |
| **远程 start_remote**  | 已支持 `listening_mode` 参数覆盖本次会话；不改本地长期模式                                            | 不受本次改动影响                                       |

### 2.2 核心可行性结论

1. **本地按键切换**：Button 类能力完整，只需在板级 `InitializeButtons()` 中按新语义分配回调 + Application 提供主模式/策略切换接口。**可行，侵入性低。**
2. **远程配置切换**：MCP 框架已成熟，新增 tool 只需在 `AddCommonTools()` 中注册，无需动协议层。**可行，侵入性低。**
3. **DualColorLed**：LED 抽象层清晰（`Led::OnStateChanged()` 虚方法），新增子类零影响现有板型。**可行，无侵入。**

---

## 3. 需求与约束

### 3.1 功能需求

| ID   | 需求                                           | 优先级 |
| ---- | ---------------------------------------------- | ------ |
| F-01 | 用户可通过按键在 realtime / auto 主模式间切换  | P0     |
| F-02 | 切换后设备给出语音/灯光反馈                    | P0     |
| F-03 | 主模式持久化到 NVS，重启恢复                   | P0     |
| F-04 | MCP 工具可查询/设置主模式                      | P1     |
| F-05 | MCP 工具可查询/设置输入策略                    | P1     |
| F-06 | MCP 工具可查询/设置 AEC 模式                   | P1     |
| F-07 | DualColorLed 支持红蓝双色 GPIO LED             | P0     |
| F-08 | DualColorLed 按设备状态映射颜色与动画          | P0     |
| F-09 | 输入策略 ptt 可通过 PressDown/PressUp 临时使用 | P2     |

### 3.2 约束

1. 协议层不动：继续使用 `auto / manual / realtime` 三种 `listen.mode`。
2. 不影响现有板型：所有改动通过新增代码或可选路径引入。
3. Custom Wake Word 限制：仅在 Idle 阶段启用，Listening/Speaking 不持续运行。
4. 单按键板型（landoubao-dev、zhiban-ai-reader）优先设计。

---

## 4. 架构设计

### 4.1 主模式与输入策略抽象

在 `Application` 中引入两个新字段，与现有 `aec_mode_` 平级：

```
Application
├── main_mode_: MainMode          // realtime | auto (NVS 持久化)
├── button_strategy_: ButtonStrategy  // toggle | ptt (NVS 持久化)
├── aec_mode_: AecMode            // off | device | server (已有)
├── listening_mode_: ListeningMode // 每轮会话实际协议模式 (已有，运行时)
```

**类型定义**（新增在 `application.h`）：

```cpp
enum MainMode {
    kMainModeRealtime = 0,   // 连续对话
    kMainModeAuto = 1,       // 单轮对话
};

enum ButtonStrategy {
    kButtonStrategyToggle = 0,  // 点击开始/结束
    kButtonStrategyPtt = 1,     // 按住说话
};
```

### 4.2 GetDefaultListeningMode 改造

**Before：**
```cpp
ListeningMode Application::GetDefaultListeningMode() const {
    return aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime;
}
```

**After：**
```cpp
ListeningMode Application::GetDefaultListeningMode() const {
    return main_mode_ == kMainModeRealtime ? kListeningModeRealtime : kListeningModeAutoStop;
}
```

核心变化：**主模式直接决定默认 ListeningMode**，AEC 不再参与决策。AEC 回归为纯音频能力开关。

### 4.3 按键交互分配

#### 单按键板型（landoubao-dev / zhiban-ai-reader）

| 操作     | 行为                                             |
| -------- | ------------------------------------------------ |
| **单击** | 执行当前主模式主动作（ToggleChatState 逻辑不变） |
| **双击** | AEC 模式循环切换（已有逻辑保持）                 |
| **三击** | 主模式切换 realtime ↔ auto，给出语音/灯光反馈    |
| **长按** | 进入配网（已有逻辑保持）                         |
| **按住** | PTT 输入策略（P2 阶段，暂可不实现）              |

#### Application 新增公共接口

```cpp
// 主模式
MainMode GetMainMode() const;
void SetMainMode(MainMode mode);      // 持久化 + 切换
void ToggleMainMode();                 // realtime <-> auto

// 输入策略
ButtonStrategy GetButtonStrategy() const;
void SetButtonStrategy(ButtonStrategy strategy);  // 持久化
```

### 4.4 MCP 工具设计

新增 6 个工具，注册在 `AddCommonTools()` 中：

| 工具名                           | 参数                                 | 返回                              |
| -------------------------------- | ------------------------------------ | --------------------------------- |
| `self.audio.get_main_mode`       | 无                                   | `"realtime"` / `"auto"`           |
| `self.audio.set_main_mode`       | `mode: string ("realtime" / "auto")` | 设置结果                          |
| `self.audio.get_button_strategy` | 无                                   | `"toggle"` / `"ptt"`              |
| `self.audio.set_button_strategy` | `strategy: string ("toggle"/"ptt")`  | 设置结果                          |
| `self.audio.get_aec_mode`        | 无                                   | `"off"` / `"device"` / `"server"` |
| `self.audio.set_aec_mode`        | `mode: string`                       | 设置结果                          |

### 4.5 DualColorLed 设计

**参考来源**：landoubao-dev 旧分支使用 `DualColorLed(LED_RED_GPIO, LED_BLUE_GPIO)` 双色 GPIO LED。

**类设计**：

```cpp
class DualColorLed : public Led {
public:
    DualColorLed(gpio_num_t red_gpio, gpio_num_t blue_gpio);
    void OnStateChanged() override;

private:
    GpioLed red_led_;   // 复用已有 GpioLed 类
    GpioLed blue_led_;
};
```

**状态-颜色映射表**（基于 landoubao-dev 硬件：红蓝两色 LED）：

| DeviceState       | 红灯行为        | 蓝灯行为     | 说明     |
| ----------------- | --------------- | ------------ | -------- |
| `Starting`        | 关              | 快闪 (100ms) | 启动中   |
| `WifiConfiguring` | 关              | 慢闪 (500ms) | 配网中   |
| `Idle`            | 关              | 关(或微亮)   | 待机     |
| `Connecting`      | 关              | 常亮         | 连接中   |
| `Listening`       | 常亮/高亮(有声) | 关           | 收音中   |
| `Speaking`        | 关              | 常亮         | 播报中   |
| `Upgrading`       | 快闪 (100ms)    | 快闪 (100ms) | 升级中   |
| `Activating`      | 关              | 慢闪 (500ms) | 激活中   |
| `FatalError`      | 快闪 (100ms)    | 关           | 致命错误 |
| `AudioTesting`    | 同 Listening    | 关           | 音频测试 |

### 4.6 模式反馈机制

主模式切换时的用户反馈：

1. **语音**：播放短提示音（复用现有 `popup.ogg` 或后续新增专用音效）
2. **LED**：切换瞬间闪烁特定颜色（如三击切换后蓝灯闪 2 次表示 realtime，红灯闪 2 次表示 auto）
3. **日志**：ESP_LOGI 输出当前模式

---

## 5. 实施阶段

### 阶段 1：应用层主模式抽象 (P0)

**目标**：Application 引入 MainMode / ButtonStrategy，替换 GetDefaultListeningMode 中的 AEC 硬绑。

**改动文件**：

| 文件                  | 改动说明                                                      |
| --------------------- | ------------------------------------------------------------- |
| `main/application.h`  | 新增 `MainMode`/`ButtonStrategy` 枚举、成员变量、公共接口声明 |
| `main/application.cc` | 实现 Get/Set/Toggle 方法，修改 `GetDefaultListeningMode()`    |
| `main/settings.cc`    | （可选）如需特定 namespace，新增 `chat` namespace             |

**验证**：
- 编译通过，默认行为与改动前一致（MainMode 默认值与板型 AEC 状态对齐）
- NVS 写入/读取主模式正确

### 阶段 2：DualColorLed 实现 (P0)

**目标**：新增 DualColorLed LED 实现，landoubao-dev 板型接入。

**改动文件**：

| 文件                                                     | 改动说明                                                 |
| -------------------------------------------------------- | -------------------------------------------------------- |
| `main/led/dual_color_led.h`                              | **新建** — DualColorLed 类声明                           |
| `main/led/dual_color_led.cc`                             | **新建** — OnStateChanged 状态映射实现                   |
| `main/CMakeLists.txt`                                    | SRCS 列表添加 `led/dual_color_led.cc`                    |
| `main/boards/zhiban-ai-reader/config.h`                  | 新增 `LED_RED_GPIO` / `LED_BLUE_GPIO` （如板型支持双色） |
| `main/boards/zhiban-ai-reader/zhiban_ai_reader_board.cc` | `GetLed()` 改用 `DualColorLed`（如硬件匹配）             |

**验证**：
- 编译通过
- 各 DeviceState 切换时红蓝灯行为符合映射表

### 阶段 3：板级按键接入 (P0)

**目标**：zhiban-ai-reader / landoubao-dev 板型按新语义分配按键回调。

**改动文件**：

| 文件                                                     | 改动说明                                     |
| -------------------------------------------------------- | -------------------------------------------- |
| `main/boards/zhiban-ai-reader/zhiban_ai_reader_board.cc` | 三击回调调用 `Application::ToggleMainMode()` |
| （可选）landoubao-dev 板级文件                           | 同上，重新分配单击=聊天切换、三击=模式切换   |

**验证**：
- 三击切换主模式后，下次语音唤醒/单击进入的 ListeningMode 正确
- 切换后有灯光/语音反馈
- NVS 持久化，重启后模式恢复

### 阶段 4：MCP 远程配置 (P1)

**目标**：通过 MCP 工具远程查询和设置主模式、输入策略、AEC。

**改动文件**：

| 文件                 | 改动说明                             |
| -------------------- | ------------------------------------ |
| `main/mcp_server.cc` | `AddCommonTools()` 中注册 6 个新工具 |

**验证**：
- MCP 客户端调用 `self.audio.get_main_mode` 返回正确值
- MCP 客户端调用 `self.audio.set_main_mode` 后设备行为切换
- 会话进行中调用 set 方法不会崩溃（先结束当前会话再切换）

### 阶段 5：PTT 输入策略 (P2)

**目标**：支持按住说话的输入策略。

**改动文件**：

| 文件                                                     | 改动说明                                                                  |
| -------------------------------------------------------- | ------------------------------------------------------------------------- |
| `main/boards/zhiban-ai-reader/zhiban_ai_reader_board.cc` | 根据 `ButtonStrategy` 在 PressDown/PressUp 中分别调用 Start/StopListening |
| `main/application.cc`                                    | 确保 PTT 路径使用 `kListeningModeManualStop`                              |

**验证**：
- 按住时进入 Listening(manual)，松开时停止
- 切换 toggle ↔ ptt 后行为正确

---

## 6. 替代方案对比

| 方案                            | 优点                 | 缺点                                       | 结论     |
| ------------------------------- | -------------------- | ------------------------------------------ | -------- |
| A. 主模式放 Application（推荐） | 全局统一、MCP 易接入 | Application 职责略增                       | **采用** |
| B. 主模式放 Board 子类          | 板级自治             | MCP/远程配置需要回调穿透，耦合更大         | 不采用   |
| C. 不引入主模式，继续绑 AEC     | 零改动               | 违反架构文档设计意图，用户无法独立控制模式 | 不采用   |

---

## 7. 依赖关系

```
阶段 1 (主模式抽象)
   ├──→ 阶段 2 (DualColorLed) ← 无依赖，可并行
   ├──→ 阶段 3 (按键接入) ← 依赖阶段 1
   ├──→ 阶段 4 (MCP 远程) ← 依赖阶段 1
   └──→ 阶段 5 (PTT) ← 依赖阶段 1 + 3
```

阶段 1 和阶段 2 可并行推进。阶段 3、4 依赖阶段 1 完成。阶段 5 依赖阶段 1 和 3。

---

## 8. 受影响文件汇总

| 文件                                                     | 阶段 | 操作         |
| -------------------------------------------------------- | ---- | ------------ |
| `main/application.h`                                     | 1    | 修改         |
| `main/application.cc`                                    | 1, 5 | 修改         |
| `main/led/dual_color_led.h`                              | 2    | **新建**     |
| `main/led/dual_color_led.cc`                             | 2    | **新建**     |
| `main/CMakeLists.txt`                                    | 2    | 修改         |
| `main/boards/zhiban-ai-reader/zhiban_ai_reader_board.cc` | 3, 5 | 修改         |
| `main/boards/zhiban-ai-reader/config.h`                  | 2    | 修改（可选） |
| `main/mcp_server.cc`                                     | 4    | 修改         |

---

## 9. 测试与验证

### 9.1 单元级

- `MainMode` NVS 读写正确性
- `GetDefaultListeningMode()` 在不同 MainMode 下返回值正确
- `DualColorLed::OnStateChanged()` 各状态灯光行为正确

### 9.2 集成级

- 语音唤醒后进入的 ListeningMode 与当前 MainMode 一致
- 三击按键切换主模式 → NVS 持久化 → 重启恢复
- MCP `set_main_mode` → 设备行为切换 → `get_main_mode` 返回新值
- 会话进行中切换主模式不崩溃

### 9.3 回归级

- 现有板型（未改按键逻辑的）行为不变
- start_remote 远程对话的 pending listening mode 仍正常工作
- AEC 切换（双击）不受影响

---

## 10. 风险与假设

| 风险/假设                                      | 影响                       | 缓解措施                                        |
| ---------------------------------------------- | -------------------------- | ----------------------------------------------- |
| MainMode 默认值与现有 AEC 绑定行为不一致       | 升级后用户感知模式变化     | 首次启动时根据 AEC 状态初始化 MainMode          |
| 三击识别不灵敏                                 | 用户误触发或无法触发       | Button 类已支持 OnMultipleClick，可调整防抖参数 |
| DualColorLed 两个 GpioLed 共用 LEDC timer 冲突 | 初始化失败                 | 构造时传入不同 timer_num 和 channel             |
| 会话中切换主模式导致协议状态不一致             | 服务端收到意外的 mode 切换 | 切换前先结束当前会话（如有），再应用新模式      |
| Custom Wake Word 在 realtime 模式下的 CPU 压力 | 看门狗触发                 | 保持现有限制：CWW 只在 Idle 阶段启用            |

---

## 11. 相关文档

- [交互模式架构方案](../docs/interaction-mode-architecture.md)
- [MCP 协议文档](../docs/mcp-protocol.md)
- [MCP start_remote 实施计划](./mcp-start-remote-implementation-plan.md)
- [监听模式备忘](/memories/repo/listening-mode-notes.md)
