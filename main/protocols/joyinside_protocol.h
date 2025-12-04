#ifndef _JOYINSIDE_PROTOCOL_H_
#define _JOYINSIDE_PROTOCOL_H_

#include "protocol.h"
#include "joyinside_auth.h"

#include <web_socket.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/timers.h>
#include <esp_timer.h>

#include <string>
#include <atomic>
#include <deque>
#include <mutex>
#include <functional>

// JoyInside 协议事件位定义
#define JOYINSIDE_EVENT_SERVER_CONFIG    (1 << 0)  // 收到 CFG_BOT_EVENT
#define JOYINSIDE_EVENT_CONFIG_UPDATED   (1 << 1)  // 收到 SERVER_VOICE_CHAT_UPDATED
#define JOYINSIDE_EVENT_INTERRUPTED      (1 << 2)  // 收到 CALL_AGENT_INTERRUPTED
#define JOYINSIDE_EVENT_TTS_COMPLETE     (1 << 3)  // 收到 TTS_COMPLETE

/**
 * @brief JoyInside 自由对话模式的对话状态
 */
enum class JoyInsideDialogState {
    kIdle,          // 空闲 - 等待语音输入
    kListening,     // 听取中 - ASR 正在识别
    kProcessing,    // 处理中 - 智能体思考
    kSpeaking,      // 播放中 - TTS 音频播放
    kInterrupted    // 已打断 - 处理打断逻辑
};

/**
 * @brief JoyInside 自由对话协议实现
 * 
 * 实现了附身智能的自由对话模式协议，支持：
 * - 云端 VAD 检测
 * - 自动打断（Barge-in）
 * - 持续录音与流式上传
 * - OPUS 编解码
 */
class JoyInsideProtocol : public Protocol {
public:
    JoyInsideProtocol();
    ~JoyInsideProtocol();

    // Protocol 接口实现
    bool Start() override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel() override;
    bool IsAudioChannelOpened() const override;
    
    // JoyInside 不需要客户端控制 listen 状态（云端 VAD）
    void SendStartListening(ListeningMode mode) override;
    void SendStopListening() override;
    void SendAbortSpeaking(AbortReason reason) override;
    void SendWakeWordDetected(const std::string& wake_word) override;

    // JoyInside 特有方法
    JoyInsideDialogState GetDialogState() const { return dialog_state_; }
    void HandleInterrupt();  // 处理打断事件
    
    // 手动模式支持
    void SetManualMode(bool enabled) { manual_mode_ = enabled; }
    bool IsManualMode() const { return manual_mode_; }
    bool SendAudioFinish();  // 发送音频结束事件 (手动模式)
    bool SendInterrupt();    // 发送打断事件
    
    // 打断回调
    using InterruptCallback = std::function<void()>;
    void SetInterruptCallback(InterruptCallback callback) { on_interrupt_ = callback; }

protected:
    bool SendText(const std::string& text) override;

private:
    // 认证模块
    JoyInsideAuth auth_;
    
    // WebSocket 连接
    std::unique_ptr<WebSocket> websocket_;
    EventGroupHandle_t event_group_handle_;
    
    // 心跳定时器
    esp_timer_handle_t heartbeat_timer_;
    
    // 空闲超时定时器（超时后关闭音频通道但保持连接）
    esp_timer_handle_t idle_timeout_timer_;
    bool audio_channel_active_;       // 音频通道是否激活（区别于 WebSocket 连接）
    bool reconnecting_;               // 正在重连中，断开回调不触发 on_audio_channel_closed_
    bool was_disconnected_;           // 标记是否曾经断开过（用于判断是否需要播放重连成功音）
    bool connection_established_;     // 标记首次连接是否成功（只有首次成功后的断开才播放警告音）
    
    // 对话状态
    std::atomic<JoyInsideDialogState> dialog_state_;
    std::string current_round_id_;
    std::string interrupted_round_id_;  // 被打断的轮次ID，用于丢弃后续TTS帧
    
    // 音频帧计数
    std::atomic<uint32_t> audio_frame_index_;
    
    // 预缓冲相关
    static constexpr int PREBUFFER_FRAMES = 2;  // 预缓冲 2 帧 (120ms @ 60ms/帧)
    std::deque<std::vector<uint8_t>> tts_buffer_;
    std::mutex buffer_mutex_;
    bool prebuffering_;
    
    // 配置参数
    std::string bot_id_;
    std::string session_id_;
    std::string user_id_;
    std::string access_token_;
    bool use_binary_mode_;  // 是否使用二进制模式发送音频 (参考 C++ SDK)
    bool tts_started_;      // 标记 TTS 是否已开始（用于通知应用层）
    bool manual_mode_;      // 手动模式（需要发送 CLIENT_AUDIO_FINISH）
    
    // 打断回调
    InterruptCallback on_interrupt_;
    
    // 初始化与配置
    bool InitializeConnection();
    void StartHeartbeat();
    void StopHeartbeat();
    static void HeartbeatCallback(void* arg);
    
    // 空闲超时管理
    void StartIdleTimeout();
    void StopIdleTimeout();
    static void IdleTimeoutCallback(void* arg);
    void OnIdleTimeout();
    
    // 消息发送
    bool SendPing();
    bool SendAudioConfig();
    bool SendBinaryAudioFrame(const uint8_t* data, size_t len);
    bool SendJsonAudioFrame(const uint8_t* data, size_t len, uint32_t index);
    
    // 消息接收与解析
    void OnWebSocketData(const char* data, size_t len, bool binary);
    void ParseJsonMessage(const char* data, size_t len);
    void HandleEvent(const cJSON* content);
    void HandleASR(const cJSON* content);
    void HandleAgent(const cJSON* content);
    void HandleTTS(const cJSON* content);
    void HandleBinaryTTS(const uint8_t* data, size_t len);  // 二进制模式 TTS
    void HandleActivity(const cJSON* content);
    
    // 状态管理
    void SetDialogState(JoyInsideDialogState state);
    void ResetForNewRound();
    
    // 工具方法
    std::string GenerateUUID();
    std::string GenerateMessageId();
};

#endif // _JOYINSIDE_PROTOCOL_H_
