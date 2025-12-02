# 端到端延时调优指南

本文档介绍影响语音交互端到端延时的关键配置参数，帮助你根据实际场景进行优化。

## 📊 延时组成

完整的端到端延时由以下部分组成：

```
┌──────────────────────────────────────────────────────────────────────────┐
│                          端到端延时组成                                   │
├──────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│  用户说话 ──→ [唤醒检测] ──→ [音频编码] ──→ [网络传输] ──→ [云端处理]     │
│                  ↓              ↓              ↓              ↓          │
│               ~50ms         0~530ms        ~50ms         ~300ms         │
│                                                                          │
│           ──→ [TTS生成] ──→ [网络传输] ──→ [预缓冲] ──→ [音频播放]       │
│                   ↓              ↓             ↓              ↓          │
│              ~200ms          ~50ms        60~600ms        实时           │
│                                                                          │
└──────────────────────────────────────────────────────────────────────────┘
```

## 🎛️ 可配置参数

### 1. 唤醒词数据发送 (`CONFIG_SEND_WAKE_WORD_DATA`)

| 配置项                       | 路径                                      |
| ---------------------------- | ----------------------------------------- |
| `CONFIG_SEND_WAKE_WORD_DATA` | `Xiaozhi Assistant → Send Wake Word Data` |

| 选项         | 延时影响   | 说明                               |
| ------------ | ---------- | ---------------------------------- |
| **关闭 (n)** | **⚡ 最快** | 不发送唤醒词音频，直接发送实时音频 |
| 开启 (y)     | +200~530ms | 需要编码并发送唤醒词缓冲区         |

**推荐**：对于 JoyInside 协议，建议**关闭**此选项，可获得最低延时且避免丢字问题。

**唤醒提示音**：关闭唤醒词发送时，设备会播放 "叮" 提示音反馈用户唤醒成功。提示音在进入 LISTENING 状态后播放（在 `ResetDecoder()` 之后），确保不会被清空。AEC 会自动过滤提示音，不影响语音上传。

---

### 2. 唤醒词缓冲时长 (`CONFIG_WAKE_WORD_BUFFER_MS`)

| 配置项                       | 路径                                            |
| ---------------------------- | ----------------------------------------------- |
| `CONFIG_WAKE_WORD_BUFFER_MS` | `Xiaozhi Assistant → Wake Word Buffer Duration` |

> ⚠️ 仅在 `CONFIG_SEND_WAKE_WORD_DATA=y` 时生效

| 选项        | 数据包 | 编码时间 | 使用场景                       |
| ----------- | ------ | -------- | ------------------------------ |
| 500 ms      | ~16 包 | ~130 ms  | 极低延时，可能丢失唤醒词后内容 |
| 750 ms      | ~25 包 | ~200 ms  | 低延时，简短唤醒词             |
| **1000 ms** | ~33 包 | ~260 ms  | **默认**，平衡延时和完整性     |
| 1500 ms     | ~50 包 | ~400 ms  | 连续对话，捕获唤醒词后 0.5s    |
| 2000 ms     | ~66 包 | ~530 ms  | 最完整，高延时                 |

**权衡**：
- 缓冲越短 → 编码越快 → 但可能丢失唤醒词后的内容
- 缓冲越长 → 内容越完整 → 但延时越高

---

### 3. TTS 预缓冲帧数 (`CONFIG_JOYINSIDE_PREBUFFER_FRAMES`)

| 配置项                              | 路径                                                         |
| ----------------------------------- | ------------------------------------------------------------ |
| `CONFIG_JOYINSIDE_PREBUFFER_FRAMES` | `Xiaozhi Assistant → JoyInside Protocol → Pre-buffer Frames` |

| 帧数     | 延时   | 抗抖动能力 | 说明                       |
| -------- | ------ | ---------- | -------------------------- |
| 1 帧     | 60 ms  | 低         | 最低延时，网络差时可能卡顿 |
| 2 帧     | 120 ms | 中         | 较低延时                   |
| **3 帧** | 180 ms | **推荐**   | **默认**，平衡延时和流畅度 |
| 5 帧     | 300 ms | 高         | 网络不稳定时使用           |
| 10 帧    | 600 ms | 最高       | 极差网络环境               |

**权衡**：
- 帧数越少 → TTS 开始播放越快 → 但网络抖动时可能卡顿
- 帧数越多 → 播放越流畅 → 但首字延时越高

---

### 4. 心跳间隔 (`CONFIG_JOYINSIDE_HEARTBEAT_INTERVAL_MS`)

| 配置项                                   | 路径                                                          |
| ---------------------------------------- | ------------------------------------------------------------- |
| `CONFIG_JOYINSIDE_HEARTBEAT_INTERVAL_MS` | `Xiaozhi Assistant → JoyInside Protocol → Heartbeat Interval` |

| 间隔         | 说明                             |
| ------------ | -------------------------------- |
| 5000 ms      | 频繁心跳，保活更可靠，略增加功耗 |
| **15000 ms** | **默认**，平衡保活和功耗         |
| 30000 ms     | 最长间隔，省电但可能断连         |

**影响**：心跳本身不直接影响对话延时，但影响预连接的有效性。心跳超时可能导致连接失效，下次唤醒需要重新建立连接（增加 ~500ms）。

---

### 5. 空闲超时 (`CONFIG_JOYINSIDE_IDLE_TIMEOUT_SEC`)

| 配置项                              | 路径                                                    |
| ----------------------------------- | ------------------------------------------------------- |
| `CONFIG_JOYINSIDE_IDLE_TIMEOUT_SEC` | `Xiaozhi Assistant → JoyInside Protocol → Idle Timeout` |

| 设置       | 说明                       |
| ---------- | -------------------------- |
| 0          | 禁用超时，始终保持音频通道 |
| 60 秒      | 较短超时，省资源           |
| 120 秒     | 适中                       |
| **300 秒** | **默认**，5 分钟           |
| 3600 秒    | 最长，1 小时               |

**影响**：超时后会关闭音频通道但保持 WebSocket 连接。下次唤醒需要重新打开音频通道（增加 ~100ms）。

---

### 6. Opus 编码器复杂度

| 位置         | 文件                                 |
| ------------ | ------------------------------------ |
| 代码中硬编码 | `afe_wake_word.cc`, `audio_codec.cc` |

```cpp
encoder->SetComplexity(0); // 0 = 最快, 10 = 最高质量
```

| 复杂度 | 编码速度 | 音质   |
| ------ | -------- | ------ |
| **0**  | **最快** | 足够好 |
| 5      | 中等     | 更好   |
| 10     | 最慢     | 最佳   |

当前默认使用 `0`（最快），对于语音来说音质已足够。

---

## 📈 延时优化配置方案

### 方案一：极低延时（推荐）

适用于网络良好、追求最快响应的场景。

```kconfig
# 不发送唤醒词（省去编码时间）
CONFIG_SEND_WAKE_WORD_DATA=n

# TTS 预缓冲 2 帧
CONFIG_JOYINSIDE_PREBUFFER_FRAMES=2

# 心跳 15 秒
CONFIG_JOYINSIDE_HEARTBEAT_INTERVAL_MS=15000

# 空闲超时 5 分钟
CONFIG_JOYINSIDE_IDLE_TIMEOUT_SEC=300
```

**预期延时**：
| 阶段        | 时间           |
| ----------- | -------------- |
| 唤醒检测    | ~50 ms         |
| 唤醒词编码  | 0 ms（跳过）   |
| 连接建立    | 0 ms（预连接） |
| 网络 + 云端 | ~350 ms        |
| TTS 预缓冲  | 120 ms         |
| **总计**    | **~520 ms**    |

---

### 方案二：平衡配置（默认）

适用于一般场景，平衡延时和功能。

```kconfig
# 不发送唤醒词
CONFIG_SEND_WAKE_WORD_DATA=n

# TTS 预缓冲 3 帧
CONFIG_JOYINSIDE_PREBUFFER_FRAMES=3

# 心跳 15 秒
CONFIG_JOYINSIDE_HEARTBEAT_INTERVAL_MS=15000

# 空闲超时 5 分钟
CONFIG_JOYINSIDE_IDLE_TIMEOUT_SEC=300
```

**预期延时**：~580 ms

---

### 方案三：连续对话模式

适用于需要捕获唤醒词后内容的场景（如 "你好东东今天天气"）。

```kconfig
# 发送唤醒词
CONFIG_SEND_WAKE_WORD_DATA=y

# 缓冲 1.5 秒（捕获唤醒词后内容）
CONFIG_WAKE_WORD_BUFFER_1500MS=y

# TTS 预缓冲 3 帧
CONFIG_JOYINSIDE_PREBUFFER_FRAMES=3
```

**预期延时**：~980 ms（编码增加 ~400 ms）

---

### 方案四：弱网络环境

适用于 WiFi 信号差、网络不稳定的场景。

```kconfig
# 不发送唤醒词
CONFIG_SEND_WAKE_WORD_DATA=n

# TTS 预缓冲 5 帧（抗抖动）
CONFIG_JOYINSIDE_PREBUFFER_FRAMES=5

# 心跳 10 秒（更频繁保活）
CONFIG_JOYINSIDE_HEARTBEAT_INTERVAL_MS=10000

# 空闲超时 2 分钟
CONFIG_JOYINSIDE_IDLE_TIMEOUT_SEC=120
```

**预期延时**：~700 ms，但播放更流畅

---

## 🔧 调试与监控

### 查看延时日志

启用调试日志可以看到各阶段耗时：

```cpp
esp_log_level_set("JoyInside", ESP_LOG_DEBUG);
esp_log_level_set("AfeWakeWord", ESP_LOG_DEBUG);
```

关键日志：
```
I (xxx) AfeWakeWord: Encode wake word opus 33 packets in 264 ms
I (xxx) JoyInside: Dialog state: IDLE -> LISTENING
I (xxx) JoyInside: Received event: TTS_SENTENCE_START
I (xxx) JoyInside: TTS playback started after 2 frames buffered
```

### 延时测量点

| 测量点     | 日志关键词                                |
| ---------- | ----------------------------------------- |
| 唤醒检测   | `Wake word detected`                      |
| 提示音播放 | `Playing wake word popup sound`           |
| 编码完成   | `Encode wake word opus X packets in Y ms` |
| 状态切换   | `Dialog state: X -> Y`                    |
| 收到 ASR   | `ASR (Final): ...`                        |
| TTS 开始   | `TTS_SENTENCE_START`                      |
| 播放开始   | `TTS playback started`                    |

---

## 📋 配置速查表

| 配置项                  | 影响         | 推荐值           |
| ----------------------- | ------------ | ---------------- |
| `SEND_WAKE_WORD_DATA`   | 唤醒响应延时 | **n**（关闭）    |
| `WAKE_WORD_BUFFER_MS`   | 编码时间     | 1000（如需发送） |
| `PREBUFFER_FRAMES`      | TTS 首字延时 | **2-3 帧**       |
| `HEARTBEAT_INTERVAL_MS` | 连接保活     | 15000            |
| `IDLE_TIMEOUT_SEC`      | 资源释放     | 300              |

---

## ❓ FAQ

### Q: 为什么关闭唤醒词发送能减少延时？
**A**: 不发送唤醒词可以跳过：
1. 唤醒词缓冲区的 Opus 编码（200-530ms）
2. 唤醒词数据的网络传输

### Q: 关闭唤醒词发送会影响识别吗？
**A**: 不会。云端 VAD 会自动检测实时音频中的语音，ASR 从检测到的语音开始识别。唤醒词本身通常不需要发送给云端。

### Q: 预缓冲帧数设太少会怎样？
**A**: TTS 播放可能出现卡顿，因为没有足够的缓冲来应对网络抖动。如果网络良好，2 帧就足够了。

### Q: 空闲超时设为 0 会有什么影响？
**A**: 音频通道永不关闭，下次唤醒响应更快（省去重新开启通道的时间），但会持续占用服务端资源。

---

*文档最后更新：2025年12月2日*
