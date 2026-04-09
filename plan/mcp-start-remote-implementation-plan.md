# MCP Tool `self.conversation.start_remote` 实施计划

## Introduction

在 xiaozhi-esp32 固件中实现 `self.conversation.start_remote` MCP 工具，使 adapter 可以通过 mqtt-gateway 远程启动设备对话。这是定时阅读提醒链路中**唯一缺失的一环**。

当前链路验证状态（2026-04-09 更新）：

```
landoubao → [adapter ✅] → [gateway ✅ trigger透传已实现] → [固件 ✅ MCP+hello trigger] → [server 待对接]
```

adapter 已成功发送 MCP 请求到真实设备，设备返回 `accepted` 并拉起对话。hello trigger 注入已实现。

## Requirements and Constraints

### 目标

1. 设备注册 `self.conversation.start_remote` MCP tool。
2. 收到调用后，设备从 idle 状态发起一次对话（等价于用户按下对话按钮）。
3. MCP 响应应同步返回"已请求启动"，不阻塞等待对话完成。
4. 为后续扩展预留 trigger 上下文传递能力（但首版不要求 server 侧消费）。

### 约束

1. 仅在设备处于 `kDeviceStateIdle` 时允许启动，其他状态返回错误。
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
1. 检查 Application::GetDeviceState() == kDeviceStateIdle
   - 不是 idle → 返回错误 {"status": "rejected", "reason": "device_busy", "state": "..."}
2. 记录 trigger 上下文到成员变量（供后续扩展使用）
3. 调用 Application::GetInstance().StartListening()
4. 返回 {"status": "accepted", "trigger_id": "..."}
```

注意：返回 `accepted` 只表示"已请求启动对话"，不保证对话成功建立。

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
- 根据 phase 不同（reading_remind vs reading_question）调整设备端行为（如 LED 颜色/提示音）。

### Phase 5: ManualStop 后主动关闭音频通道（待实现）

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
| `main/application.cc`                         | 修改     | Initialize() 中注册 tool       |

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
