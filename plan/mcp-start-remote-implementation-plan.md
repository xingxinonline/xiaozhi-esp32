# MCP Tool `self.conversation.start_remote` 实施计划

## Introduction

在 xiaozhi-esp32 固件中实现 `self.conversation.start_remote` MCP 工具，使 adapter 可以通过 mqtt-gateway 远程启动设备对话。这是定时阅读提醒链路中**唯一缺失的一环**。

当前链路验证状态（2026-04-14 更新）：

```
landoubao → [adapter ⚠️ busy拒绝待改] → [gateway ✅ trigger透传已部署] → [固件 ✅ MCP+hello trigger+abort注入] → [server ✅ trigger消费+abort注入]
```

`reading_remind` 全链路已验证通过。
`reading_question` 的固件侧 Phase 6（`listening_mode` + `book_id`）已实现，待联调验证多轮对话。
**固件 Phase 7 已实现**：设备忙碌时不拒绝，改发 `abort(reason=remote_trigger, trigger={...})`。
**server Phase 6 已实现**：abort handler 提取 `remote_trigger` reason 并复用 `on_trigger_context_updated()` 链路。
**adapter 阻塞**：`service.py` 仍在 `is_alive=true` 时提前返回 `device_busy`，MCP 命令未到达固件。需去掉提前拒绝，让 MCP 下发到设备，由固件决定行为。

## Requirements and Constraints

### 目标

1. 设备注册 `self.conversation.start_remote` MCP tool。
2. 收到调用后，设备从 idle 状态发起一次对话（等价于用户按下对话按钮）。
3. MCP 响应应同步返回"已请求启动"，不阻塞等待对话完成。
4. 为后续扩展预留 trigger 上下文传递能力（但首版不要求 server 侧消费）。

### 约束

1. 仅在设备处于 `kDeviceStateIdle` 时直接启动新会话；在 `kDeviceStateSpeaking` / `kDeviceStateListening` 等忙碌状态时改为通过 abort 注入 trigger（Phase 7）。
2. 不修改 gateway 代码。
3. 不修改 server 代码（首版）。
4. 遵循现有 MCP tool 注册模式（参考 `PressToTalkMcpTool`）。
5. 新增文件放在 `boards/common/`，编译加入公共 SOURCES。

### 不做的事

1. 不实现对话过程监控或结果回写（那是 server + adapter 的职责）。
2. 不传音频——对话启动后走正常的音频通道。
3. 不改 hello 握手协议。

## 现有机制分析

### MCP 消息到达路径

```
gateway HTTP /api/commands/:clientId
  → gateway sendMcpRequest() → MQTT → 设备
  → Protocol::OnIncomingJson()
  → Application 路由 type="mcp" → McpServer::ParseMessage()
  → DoToolCall() → Schedule 到主任务 → 工具回调执行
  → 响应通过 MQTT 返回 gateway → HTTP 响应给 adapter
```

关键事实：**MCP 命令通过 MQTT 直发到设备，不经过 WebSocket bridge**。设备在 idle 状态下（无活跃 WebSocket 会话）也能接收 MCP 命令。

### 对话启动路径

正常按钮触发的对话流：

```
按钮按下 → Application::StartListening()       // 线程安全，发送事件
         → HandleStartListeningEvent()          // 主循环处理
         → 检查 state == kDeviceStateIdle
         → SetDeviceState(kDeviceStateConnecting)
         → Schedule → ContinueOpenAudioChannel(kListeningModeManualStop)
         → Protocol::OpenAudioChannel()         // 发送 hello，等 server hello
         → SetListeningMode()                   // 切到 kDeviceStateListening
         → 开始流式传输麦克风音频
```

`StartListening()` 是事件驱动的，调用后立即返回，实际工作在主循环中完成。

### MCP tool 回调执行时机

`McpServer::DoToolCall()` 通过 `Application::Schedule()` 把回调投递到主任务执行。这意味着：
- 回调在主循环线程中运行
- 可以安全访问 Application 状态
- **但**回调需要同步返回 ReturnValue，而 `StartListening()` 是异步的

### 时序约束

gateway 的 `/api/commands/:clientId` 对 `sendMcpRequest` 设了 **5 秒超时**。MCP tool 回调必须在 5 秒内返回响应。`StartListening()` 是非阻塞的（post event），满足此约束。

## Implementation Phases

### Phase 1: 最小可用 tool（首版目标） ✅ 已实现

创建 `StartRemoteMcpTool` 类，注册 `self.conversation.start_remote` 工具。

**tool 参数定义：**

| 参数          | 类型   | 必选 | 说明                              |
| ------------- | ------ | ---- | --------------------------------- |
| `trigger_id`  | string | 是   | 调度标识，用于追踪                |
| `source`      | string | 否   | 来源标识，默认 `"scheduled_task"` |
| `phase`       | string | 否   | 任务阶段，如 `"reading_remind"`   |
| `book_title`  | string | 否   | 书名（内联，供 server 直读）      |
| `book_author` | string | 否   | 作者                              |
| `start_page`  | int    | 否   | 今日阅读起始页                    |
| `end_page`    | int    | 否   | 今日阅读结束页                    |
| `plan_name`   | string | 否   | 阅读计划名称                      |

**回调逻辑：**

```
1. 检查 Application::GetDeviceState()
   - kDeviceStateIdle → 正常远程唤醒流程（新建 WS）
   - kDeviceStateSpeaking / kDeviceStateListening → 对话中打断注入（Phase 7）
   - 其它状态 → 返回错误 {"status": "rejected", "reason": "device_busy", "state": "..."}
2. 记录 trigger 上下文到成员变量（供后续扩展使用）
3. 如果 Idle：调用 Application::GetInstance().StartListening()
4. 如果忙碌：调用 protocol_->SendAbortSpeaking(kAbortReasonRemoteTrigger, trigger_json)
5. 返回 {"status": "accepted"/"trigger_injected", "trigger_id": "..."}
```

**涉及文件：**

| 文件                                          | 操作                                                    |
| --------------------------------------------- | ------------------------------------------------------- |
| `main/boards/common/start_remote_mcp_tool.h`  | **新建**                                                |
| `main/boards/common/start_remote_mcp_tool.cc` | **新建**                                                |
| `main/CMakeLists.txt`                         | **修改** — 公共 SOURCES 加入 `.cc`                      |
| `main/application.cc`                         | **修改** — `Initialize()` 中调用 tool 的 `Initialize()` |

### Phase 2: 验证全链路 ✅ 已通过

> 2026-04-08 验证通过。设备返回 `{"status":"accepted"}`，进入对话状态。

1. 编译烧录固件。
2. 在 zhiban-adapter 目录运行：
   ```bash
   uv run python -m zhiban_adapter.smoke --device-id "44:1b:f6:81:df:dc" --dispatch
   ```
3. 预期：设备开始对话（LED / 屏幕状态变化），adapter 收到 `{"status": "accepted"}`。

### Phase 3: hello 消息携带 trigger 上下文 ✅ 已实现

> Phase 1/2 已验证通过（2026-04-08）。本阶段将 trigger 上下文注入 hello 握手，使 server 能区分定时任务对话与普通对话。
>
> **2026-04-09 更新**：固件侧 `AddTriggerToHello()` 已实现于 `mqtt_protocol.cc` 和 `websocket_protocol.cc`。
> `SetPendingTriggerContext()` / `GetAndClearPendingTriggerContext()` 已实现。
>
> **重要发现**：gateway 不是直接透传设备 hello，而是重构一个新 hello（v2+websocket）发给 server。
> 设备发的 trigger 字段在 gateway 层被丢弃。已在 gateway 侧做 3 处最小改动透传 trigger（待部署）。

**背景**：Phase 2 验证结果表明，远程唤醒对话成功拉起，但 server 收到的 hello 消息中不含 trigger 信息，无法区分这是定时任务还是用户手动唤醒。

**设计方案**：在设备发送给 gateway/server 的 hello JSON 中增加可选的 `trigger` 字段。

hello 消息扩展（向后兼容，无此字段时 server 行为不变）：

```json
{
  "type": "hello",
  "version": 2,
  "transport": "websocket",
  "audio_params": { ... },
  "features": { "mcp": true },
  "trigger": {
    "trigger_id": "trigger_1001_20260408",
    "source": "scheduled_task",
    "phase": "reading_remind",
    "book_title": "小王子",
    "book_author": "安托万·德·圣-埃克苏佩里",
    "start_page": 12,
    "end_page": 18,
    "plan_name": "测试阅读计划"
  }
}
```

**实现步骤：**

1. `StartRemoteMcpTool` 在 MCP 回调中把所有参数（`trigger_id`、`source`、`phase`、`book_title`、`start_page` 等）保存到成员变量（Phase 1 已预留）。设备完整透传到 hello，server 直读。
2. 新增 `Application::SetPendingTriggerContext()` / `GetAndClearPendingTriggerContext()` 方法对，线程安全地在主循环中暂存 trigger 上下文。
3. 在 `MqttProtocol::GetHelloMessage()` 和 `WebsocketProtocol::GetHelloMessage()` 中，如果存在 pending trigger context，就在 hello JSON 中追加 `trigger` 对象。
4. hello 发送完毕后自动 clear pending context，避免下次普通唤醒误带旧 trigger。

**涉及文件（在 Phase 1 基础上增量）：**

| 文件                                          | 操作                                               |
| --------------------------------------------- | -------------------------------------------------- |
| `main/boards/common/start_remote_mcp_tool.cc` | **修改** — 回调中调用 `SetPendingTriggerContext()` |
| `main/application.h`                          | **修改** — 添加 trigger context 暂存方法和成员     |
| `main/application.cc`                         | **修改** — 实现 Set/GetAndClear 方法               |
| `main/protocols/mqtt_protocol.cc`             | **修改** — `GetHelloMessage()` 注入 trigger JSON   |
| `main/protocols/websocket_protocol.cc`        | **修改** — 同上                                    |

**向后兼容性**：
- ~~gateway 不解析 hello 内容，只透传给 server，**无需修改**。~~（2026-04-09：gateway 重构了 hello 消息，需要额外透传 trigger。已在 app.js 做 3 处改动，待部署。）
- server 未实现 trigger 消费时忽略未知字段，**不会报错**。
- 普通唤醒（按钮/唤醒词）不经过 `StartRemoteMcpTool`，pending context 始终为空，hello **不含 trigger 字段**。

**验证方式**：
1. 用 adapter smoke `--dispatch` 触发远程唤醒。
2. 查看 server 日志中 hello 消息是否包含 `trigger` 字段。
3. 手动按按钮唤醒，确认 hello 中不含 `trigger` 字段（回归验证）。

### Phase 4: 后续扩展（当前不实现）

- 设备端在对话结束后用 trigger_id 调 adapter 做结果回写（如果 server 不做此事）。
- ~~根据 phase 不同（reading_remind vs reading_question）调整设备端行为（如 LED 颜色/提示音）。~~ → 设备行为差异由 `listening_mode` 控制（Phase 6），非 phase 业务语义。

### Phase 7: 对话中提醒打断注入 ✅ 已实现

> **对应主项目任务：** 整体推进方案 § 4C.5
> **架构设计参见：** 整体架构设计 § 5.6 对话中提醒打断注入
>
> 2026-04-14 验证状态：固件侧已实现三路分支（Idle→accepted / Speaking+Listening→trigger_injected / 其它→rejected）。
> server 侧 abort handler 已实现 `remote_trigger` reason 处理（含测试用例）。
> **阻塞点**：adapter `service.py` 在 `is_alive=true` 时提前返回 `device_busy`，MCP 命令未到达固件。需 adapter 去掉提前拒绝。

**背景：** Phase 1–6 中 `start_remote` 仅在设备 Idle 时发起新会话。当设备正在对话（Speaking / Listening）时，返回 `device_busy` 拒绝。新设计不再拒绝，而是通过已有 WS 连接发送 abort 消息注入 trigger。

**设计原则：**
- 复用现有 abort 协议（`wake_word_detected` 已拉通），新增 `remote_trigger` reason。
- 复用现有 trigger 消费链路（server 的 `on_trigger_context_updated` 已实现）。
- 固件不新开 WS 连接，直接通过当前会话的 WS 发送 abort + trigger。

**固件改动：**

1. **`protocol.h`** — AbortReason 枚举新增：
   ```cpp
   enum AbortReason {
       kAbortReasonNone = 0,
       kAbortReasonWakeWordDetected = 1,
       kAbortReasonRemoteTrigger = 2,  // 新增
   };
   ```

2. **`protocol.cc`** — `SendAbortSpeaking` 支持携带 trigger JSON：
   ```cpp
   void Protocol::SendAbortSpeaking(AbortReason reason, const std::string& trigger_json) {
       cJSON* root = cJSON_CreateObject();
       cJSON_AddStringToObject(root, "session_id", session_id_.c_str());
       cJSON_AddStringToObject(root, "type", "abort");
       cJSON_AddStringToObject(root, "reason",
           reason == kAbortReasonRemoteTrigger ? "remote_trigger" : "wake_word_detected");
       if (!trigger_json.empty()) {
           cJSON* trigger = cJSON_Parse(trigger_json.c_str());
           if (trigger) cJSON_AddItemToObject(root, "trigger", trigger);
       }
       // ... send
   }
   ```

3. **`start_remote_mcp_tool.cc`** — 回调逻辑改为分支：
   ```cpp
   auto state = app.GetDeviceState();
   if (state == kDeviceStateIdle) {
       // 现有流程：新建 WS → hello + trigger
       app.SetPendingTriggerContext(ctx);
       app.StartListening();
       return ReturnValue({"status": "accepted", ...});
   } else if (state == kDeviceStateSpeaking || state == kDeviceStateListening) {
       // 新增：对话中打断注入
       std::string trigger_json = BuildTriggerJson(ctx);
       app.GetProtocol()->SendAbortSpeaking(kAbortReasonRemoteTrigger, trigger_json);
       return ReturnValue({"status": "trigger_injected", ...});
   } else {
       return ReturnValue({"status": "rejected", "reason": "device_busy", ...});
   }
   ```

**abort 消息格式：**
```json
{"session_id": "...", "type": "abort", "reason": "remote_trigger",
 "trigger": {"trigger_id": "...", "phase": "reading_remind", "book_title": "...", ...}}
```

**涉及文件：**

| 文件                                          | 操作 | 说明                                             |
| --------------------------------------------- | ---- | ------------------------------------------------ |
| `main/protocols/protocol.h`                   | 修改 | AbortReason 枚举新增 `kAbortReasonRemoteTrigger` |
| `main/protocols/protocol.cc`                  | 修改 | `SendAbortSpeaking` 支持 trigger JSON 参数       |
| `main/boards/common/start_remote_mcp_tool.cc` | 修改 | 回调逻辑 busy 状态不拒绝，改发 abort + trigger   |

**预估改动量：** ~25 行（protocol.h 1 行 + protocol.cc ~10 行 + start_remote_mcp_tool.cc ~15 行）

**验证方式：**
1. 设备正在对话时，用 `task_simulator` 触发 `start_remote`。
2. 确认设备返回 `{"status": "trigger_injected"}`，不再返回 `device_busy`。
3. 确认 server 日志收到 abort(reason=remote_trigger, trigger={...})。
4. 确认 server 打断当前对话并播报提醒内容。
5. 回归：设备 Idle 时 `start_remote` 仍正常新建会话，行为不变。
6. 回归：本地唤醒词打断仍使用 `wake_word_detected`，行为不变。

### Phase 5: ManualStop 后主动关闭音频通道 ✅（075d6183）

> **2026-04-09 发现**：远程唤醒使用 `kListeningModeManualStop`。TTS 播完后设备
> `HandleStopTtsEvent()` → `SetDeviceState(kDeviceStateIdle)`。但 `HandleStateChangedEvent()`
> 的 Idle 分支不会调用 `protocol_->CloseAudioChannel()`。
>
> **后果**：gateway 的 WebSocket bridge 保持 alive 状态长达 180s（server `_check_timeout()`），
> 期间 adapter 查询 `isAlive: true` → 拒绝后续推送（`device_busy`）。

**修复方案**：

在 `HandleStateChangedEvent()` 的 `kDeviceStateIdle` 分支中，如果是从 `kDeviceStateSpeaking` 转入
且当前 listening mode 为 ManualStop 且存在活跃音频通道，则主动关闭音频通道。

```cpp
// application.cc HandleStateChangedEvent() — kDeviceStateIdle 分支
case kDeviceStateIdle:
    // ... existing logic ...
    // 如果是 ManualStop 模式下 TTS 播完回 Idle，主动关闭音频通道
    if (previous_state == kDeviceStateSpeaking && 
        listening_mode_ == kListeningModeManualStop) {
        protocol_->CloseAudioChannel();
    }
    break;
```

**涉及文件：**

| 文件                  | 操作 | 说明                                              |
| --------------------- | ---- | ------------------------------------------------- |
| `main/application.cc` | 修改 | `HandleStateChangedEvent()` Idle 分支增加关闭逻辑 |

**验证方式**：
1. adapter 推送唤醒设备 → TTS 播完 → 观察 gateway 日志 bridge 是否在几秒内关闭。
2. TTS 播完后立即再次推送，确认 adapter 不再返回 `device_busy`。
3. 回归：普通按钮唤醒对话流程不受影响（按钮走 AutoStop 路径不受此分支影响）。

### Phase 6: listening_mode + book_id 参数支持 ✅ 已实现

> 2026-04-10 更新：固件侧已支持 `listening_mode` one-shot 下发与 `book_id` 透传 hello trigger，编译验证通过；`reading_question` 仍需真实链路联调确认多轮行为。
> **对应主项目任务：** 整体推进方案 § 4J.1
> **架构设计参见：** 整体架构设计 § 5.5 远程对话模式

**背景：** 当前 `start_remote` 硬编码 `kListeningModeManualStop`，TTS 播完即断连。这对 `reading_remind`（单向广播）正确，但 `reading_question`（阅读提问）需要多轮对话——TTS 播完后设备应进入 Listening 等待用户语音回答。同时，`book_id`（来源于唤醒请求中的 `material_id`，即 ISBN）需要透传到 hello trigger，供 server 按需调用 adapter 内容 API 获取页级原文。

**设计方案：** 新增两个 MCP 参数——`listening_mode` 控制对话行为，`book_id` 透传内容标识。固件不理解业务含义（什么是"提醒"/"提问"），只执行行为指令和透传参数。

**tool 参数扩展：**

| 参数             | 类型   | 必选 | 说明                                                  |
| ---------------- | ------ | ---- | ----------------------------------------------------- |
| `listening_mode` | string | 否   | `manual_stop`（默认）、`default`（使用设备默认模式）  |
| `book_id`        | string | 否   | 图书标识（ISBN），透传到 hello trigger 供 server 消费 |
| （其余参数不变） |        |      |                                                       |

**回调逻辑改动：**

```cpp
// 在 MCP 回调中读取 listening_mode 和 book_id
std::string mode_str = properties.GetValue("listening_mode");
ListeningMode mode = kListeningModeManualStop;  // 默认，向后兼容
if (mode_str == "default") {
    mode = app.GetDefaultListeningMode();  // 有 AEC → realtime，无 AEC → auto
}

std::string book_id = properties.GetValue("book_id");
// book_id 存入 trigger context，由 AddTriggerToHello() 写入 hello 消息
ctx.book_id = book_id;

// 传入 StartListening 或保存到成员变量供 ContinueOpenAudioChannel 使用
app.SetPendingListeningMode(mode);
app.StartListening();
```

**Application 改动：**

1. 新增 `SetPendingListeningMode(ListeningMode mode)` / `GetAndClearPendingListeningMode()`，类似已有的 trigger context 暂存模式。
2. `ContinueOpenAudioChannel()` 中使用 pending listening mode 而非硬编码：
   ```cpp
   // 当前：ContinueOpenAudioChannel(kListeningModeManualStop)
   // 改为：
  ListeningMode mode = GetAndClearPendingListeningMode();  // 默认 ManualStop
   ContinueOpenAudioChannel(mode);
   ```

**状态机效果对比：**

```
manual_stop（现有，不变）：
  SPEAKING → Idle → CloseAudioChannel → 设备返回待机

default（新增，有 AEC 设备 → realtime）：
  SPEAKING → Listening(Realtime) → 用户语音(ASR) → server LLM → TTS → SPEAKING → ...
  超时 → Idle → CloseAudioChannel

default（新增，无 AEC 设备 → auto）：
  SPEAKING → Listening(Auto) → 用户语音(VAD→ASR) → server LLM → TTS → SPEAKING → ...
  超时 → Idle → CloseAudioChannel
```

**涉及文件：**

| 文件                                          | 操作 | 说明                                                                             |
| --------------------------------------------- | ---- | -------------------------------------------------------------------------------- |
| `main/boards/common/start_remote_mcp_tool.cc` | 修改 | PropertyList 加 `book_id`；读取 `listening_mode` + `book_id`，传递给 Application |
| `main/boards/common/start_remote_mcp_tool.h`  | 修改 | `StartRemoteTriggerContext` 结构体加 `book_id` 成员                              |
| `main/application.h`                          | 修改 | 新增 pending listening mode 暂存方法和成员                                       |
| `main/application.cc`                         | 修改 | `ContinueOpenAudioChannel` 使用 pending mode                                     |
| `main/protocols/mqtt_protocol.cc`             | 修改 | `AddTriggerToHello()` 增加 `book_id` 字段输出                                    |
| `main/protocols/websocket_protocol.cc`        | 修改 | `AddTriggerToHello()` 增加 `book_id` 字段输出                                    |

**向后兼容性：**
- `listening_mode` 参数缺省时默认 `manual_stop`，现有行为不变。
- gateway 透传 MCP 参数，无需修改。
- server 不感知 listening_mode（server 的多轮对话本身就是通用能力）。

**验证方式：**
1. `task_simulator --phase reading_question` → adapter 传 `listening_mode=default` → 设备 TTS 播完后进入设备默认监听模式（AEC 设备 → Realtime）。
2. 在设备上语音回答 → server ASR → LLM 追问 → 多轮对话。
3. 回归：`--phase reading_remind` → `listening_mode=manual_stop` → TTS 播完断连，行为不变。
4. 回归：不传 `listening_mode` → 默认 ManualStop，行为不变。

## 代码参考

### 现有 PressToTalkMcpTool 模式

头文件结构（[press_to_talk_mcp_tool.h](main/boards/common/press_to_talk_mcp_tool.h)）：

```cpp
class PressToTalkMcpTool {
public:
    PressToTalkMcpTool();
    void Initialize();      // 注册到 McpServer
private:
    ReturnValue HandleXxx(const PropertyList& properties);  // 回调
};
```

实现文件的 Initialize（[press_to_talk_mcp_tool.cc](main/boards/common/press_to_talk_mcp_tool.cc#L11)）：

```cpp
void PressToTalkMcpTool::Initialize() {
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddTool("self.set_press_to_talk",
        "描述...",
        PropertyList({Property("mode", kPropertyTypeString)}),
        [this](const PropertyList& properties) -> ReturnValue {
            return HandleSetPressToTalk(properties);
        });
}
```

### Application::Initialize() 中注册 tool

[application.cc](main/application.cc#L96) 已有：

```cpp
auto& mcp_server = McpServer::GetInstance();
mcp_server.AddCommonTools();
mcp_server.AddUserOnlyTools();
```

在此之后添加 `start_remote_tool_.Initialize();`。

### CMakeLists.txt 公共 SOURCES

[CMakeLists.txt](main/CMakeLists.txt#L45) 中已有：

```cmake
"boards/common/press_to_talk_mcp_tool.cc"
```

在同一位置添加 `"boards/common/start_remote_mcp_tool.cc"`。

## Affected Files

| 文件                                          | 变更类型 | 说明                           |
| --------------------------------------------- | -------- | ------------------------------ |
| `main/boards/common/start_remote_mcp_tool.h`  | 新建     | tool 类声明                    |
| `main/boards/common/start_remote_mcp_tool.cc` | 新建     | tool 注册 + 回调实现           |
| `main/CMakeLists.txt`                         | 修改     | 添加 .cc 到公共编译列表        |
| `main/application.h`                          | 修改     | 添加 `StartRemoteMcpTool` 成员 |
| `main/application.cc`                         | 修改     | Initialize() 中注册 tool；Phase 6: pending listening mode |

后续新增（Phase 6）：
- `main/boards/common/start_remote_mcp_tool.cc` — PropertyList 加 `book_id`；读取 `listening_mode` + `book_id`
- `main/boards/common/start_remote_mcp_tool.h` — `StartRemoteTriggerContext` 加 `book_id` 成员
- `main/application.h` — 新增 pending listening mode 方法
- `main/protocols/mqtt_protocol.cc` — `AddTriggerToHello()` 加 `book_id`
- `main/protocols/websocket_protocol.cc` — `AddTriggerToHello()` 加 `book_id`

## Testing and Verification

1. **编译验证**：`idf.py build` 通过。
2. **tool 注册验证**：gateway 刷新设备 tool 列表（重连后），确认包含 `self.conversation.start_remote`。
3. **状态查询冒烟**：用 adapter smoke CLI 查设备状态，确认 `exists=true, is_alive=false`（idle）。
4. **dispatch 冒烟**：`--dispatch` 执行，确认设备返回 `accepted` 且设备进入对话状态。
5. **非 idle 拒绝**：设备正在对话时再次 dispatch，确认返回 `rejected`。

## Risks and Assumptions

### 风险

1. **MCP 回调线程模型**：回调在 `Schedule()` 中执行（主循环线程），而 `StartListening()` 也是投递事件到主循环。需要确认在同一个 Schedule 回调中调用 `StartListening()` 不会死锁。如果有问题，可改为直接 post `MAIN_EVENT_START_LISTENING` 事件。
2. **gateway 5 秒超时**：如果 tool 回调因为等待资源而阻塞超过 5 秒，gateway 会返回 timeout 给 adapter。当前设计中回调是非阻塞的，不应触发此问题。
3. **设备首次重连后 tool 列表缓存**：gateway 在设备连接时缓存 tool 列表。固件更新后需要确保设备重新连接 gateway 以刷新缓存。

### 假设

1. `Application::StartListening()` 在主循环 Schedule 回调中调用是安全的（event post 不阻塞）。
2. 设备 idle 时 MQTT 连接是活跃的，能接收 MCP 命令。
3. 对话启动后设备会自动走完正常的音频传输和 TTS 播放流程。

## Related Documents

- `zhiban-adapter/plan/adapter-implementation-plan.md` — adapter 侧实施计划
- `zhiban-adapter/spec/spec-architecture-remote-wakeup.md` — 整体架构规格
- `main/boards/common/press_to_talk_mcp_tool.h/cc` — MCP tool 参考实现
- `main/mcp_server.h/cc` — MCP 服务器框架
- `main/application.h/cc` — 设备应用主逻辑
