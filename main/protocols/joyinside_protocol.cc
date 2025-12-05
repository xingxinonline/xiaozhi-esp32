#include "joyinside_protocol.h"
#include "board.h"
#include "system_info.h"
#include "application.h"
#include "settings.h"
#include "assets/lang_config.h"

#include <sdkconfig.h>
#include <cstring>
#include <algorithm>
#include <cJSON.h>
#include <esp_log.h>
#include <esp_random.h>
#include <mbedtls/base64.h>

#define TAG "JoyInside"

// JoyInside WebSocket 服务地址
static const char* JOYINSIDE_WS_URL = "wss://joyinside.jd.com/soulmate/voiceChat/v1";

// NVS 命名空间和键名（用于存储 Bot ID）
static const char* NVS_NAMESPACE = "joyinside";
static const char* NVS_KEY_BOT_ID = "bot_id";

// 心跳间隔 (毫秒)
#ifdef CONFIG_JOYINSIDE_HEARTBEAT_INTERVAL_MS
static const uint32_t HEARTBEAT_INTERVAL_MS = CONFIG_JOYINSIDE_HEARTBEAT_INTERVAL_MS;
#else
static const uint32_t HEARTBEAT_INTERVAL_MS = 15000;  // 15秒
#endif

// 空闲超时时间 (秒) - 超时后关闭音频通道但保持 WebSocket 连接
#ifdef CONFIG_JOYINSIDE_IDLE_TIMEOUT_SEC
static const uint32_t IDLE_TIMEOUT_SEC = CONFIG_JOYINSIDE_IDLE_TIMEOUT_SEC;
#else
static const uint32_t IDLE_TIMEOUT_SEC = 300;  // 默认 5 分钟
#endif

// 音频参数
static const int SAMPLE_RATE = 16000;
static const int FRAME_DURATION_MS = 60;
static const int FRAME_SIZE_SAMPLES = SAMPLE_RATE * FRAME_DURATION_MS / 1000;  // 960
static const int FRAME_SIZE_BYTES = FRAME_SIZE_SAMPLES * 2;  // 1920 bytes for 16-bit

JoyInsideProtocol::JoyInsideProtocol() 
    : heartbeat_timer_(nullptr),
      idle_timeout_timer_(nullptr),
      audio_channel_active_(false),
      reconnecting_(false),
      was_disconnected_(false),
      connection_established_(false),
      dialog_state_(JoyInsideDialogState::kIdle),
      audio_frame_index_(0),
      prebuffering_(true),
      use_binary_mode_(true),  // 默认使用二进制模式（参考 C++ SDK，效率更高）
      tts_started_(false),
      manual_mode_(false),
      bot_id_ready_(false),
      sntp_synced_(false) {
    
    event_group_handle_ = xEventGroupCreate();
    
    // 从 Kconfig 读取配置
#ifdef CONFIG_JOYINSIDE_BOT_ID
    // 如果 Kconfig 中配置了 Bot ID，直接使用
    std::string kconfig_bot_id = CONFIG_JOYINSIDE_BOT_ID;
    if (!kconfig_bot_id.empty()) {
        bot_id_ = kconfig_bot_id;
        bot_id_ready_ = true;
        ESP_LOGI(TAG, "Using Bot ID from Kconfig: %s", bot_id_.c_str());
    }
#endif
    
    // 如果 Kconfig 中没有配置 Bot ID，尝试从 NVS 读取
    if (bot_id_.empty()) {
        Settings settings(NVS_NAMESPACE, false);
        bot_id_ = settings.GetString(NVS_KEY_BOT_ID, "");
        if (!bot_id_.empty()) {
            bot_id_ready_ = true;
            ESP_LOGI(TAG, "Loaded Bot ID from NVS: %s", bot_id_.c_str());
        } else {
            ESP_LOGI(TAG, "No Bot ID found, will register on first connection");
        }
    }
    
#ifdef CONFIG_JOYINSIDE_USE_BINARY_MODE
    use_binary_mode_ = CONFIG_JOYINSIDE_USE_BINARY_MODE;
#else
    use_binary_mode_ = true;  // 默认启用二进制模式
#endif
    
    // 使用设备 UUID 作为用户标识
    user_id_ = Board::GetInstance().GetUuid();
    
    // 配置认证方式
#ifdef CONFIG_JOYINSIDE_AUTH_AK_SK
    // AK/SK 签名认证
    #ifdef CONFIG_JOYINSIDE_ACCESS_KEY
    std::string access_key = CONFIG_JOYINSIDE_ACCESS_KEY;
    #else
    std::string access_key;
    #endif
    
    #ifdef CONFIG_JOYINSIDE_ACCESS_KEY_SECRET
    std::string access_key_secret = CONFIG_JOYINSIDE_ACCESS_KEY_SECRET;
    #else
    std::string access_key_secret;
    #endif
    
    #ifdef CONFIG_JOYINSIDE_VENDOR_ID
    int vendor_id = CONFIG_JOYINSIDE_VENDOR_ID;
    #else
    int vendor_id = 100090;
    #endif
    
    // 如果 bot_id_ 为空，先使用空字符串配置 auth_，后续注册时会更新
    auth_.Configure(access_key, access_key_secret, vendor_id, bot_id_);
    
    // 配置 App ID（用于设备注册）
    #ifdef CONFIG_JOYINSIDE_APP_ID
    auth_.SetAppId(CONFIG_JOYINSIDE_APP_ID);
    #endif
    
    ESP_LOGI(TAG, "Using AK/SK authentication");
#else
    // 静态 Token 认证
    #ifdef CONFIG_JOYINSIDE_ACCESS_TOKEN
    access_token_ = CONFIG_JOYINSIDE_ACCESS_TOKEN;
    auth_.SetStaticToken(access_token_);
    #endif
    ESP_LOGI(TAG, "Using static token authentication");
#endif
    
    server_sample_rate_ = SAMPLE_RATE;
    server_frame_duration_ = FRAME_DURATION_MS;
}

JoyInsideProtocol::~JoyInsideProtocol() {
    StopIdleTimeout();
    StopHeartbeat();
    CloseAudioChannel();
    if (event_group_handle_) {
        vEventGroupDelete(event_group_handle_);
    }
}

bool JoyInsideProtocol::Start() {
    // 预先建立 WebSocket 连接，这样唤醒后可以立即开始对话
    ESP_LOGI(TAG, "Pre-connecting to JoyInside server...");
    
    // 持续等待 SNTP 时间同步和建立连接，直到成功
    // JoyInside 需要正确的时间戳进行签名认证，没有同步成功无法连接
    while (true) {
        // 等待 SNTP 同步
        if (!WaitForSntpSync(15)) {
            ESP_LOGW(TAG, "SNTP sync failed, retrying in 5 seconds...");
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        
        // 尝试建立连接
        constexpr int kMaxRetries = 3;
        constexpr int kRetryDelayMs = 2000;
        bool connected = false;
        
        for (int attempt = 1; attempt <= kMaxRetries; ++attempt) {
            if (InitializeConnection()) {
                connected = true;
                break;
            }
            
            if (attempt < kMaxRetries) {
                ESP_LOGW(TAG, "Pre-connection attempt %d/%d failed, retrying in %d ms...", 
                         attempt, kMaxRetries, kRetryDelayMs);
                vTaskDelay(pdMS_TO_TICKS(kRetryDelayMs));
            }
        }
        
        if (connected) {
            // 如果之前断开过并且现在预连接成功，通知应用层播放成功音
            if (was_disconnected_ && on_connected_) {
                on_connected_();
                was_disconnected_ = false;
            }
            
            ESP_LOGI(TAG, "Pre-connection successful");
            return true;
        }
        
        ESP_LOGW(TAG, "Pre-connection failed, retrying in 5 seconds...");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    
    // 不会到达这里
    return true;
}

void JoyInsideProtocol::SendStartListening(ListeningMode mode) {
    // JoyInside 使用云端 VAD，不需要客户端发送 listen 状态
    // 只更新内部状态
    ESP_LOGD(TAG, "SendStartListening called (mode=%d), JoyInside uses cloud VAD", mode);
    SetDialogState(JoyInsideDialogState::kListening);
}

void JoyInsideProtocol::SendStopListening() {
    // JoyInside 使用云端 VAD，客户端不需要发送停止信号
    ESP_LOGD(TAG, "SendStopListening called, JoyInside uses cloud VAD");
}

void JoyInsideProtocol::SendAbortSpeaking(AbortReason reason) {
    // JoyInside 使用打断事件来处理，不需要客户端主动发送
    ESP_LOGD(TAG, "SendAbortSpeaking called (reason=%d)", reason);
}

void JoyInsideProtocol::SendWakeWordDetected(const std::string& wake_word) {
    // JoyInside 不需要发送唤醒词，直接开始录音即可
    ESP_LOGD(TAG, "SendWakeWordDetected: %s", wake_word.c_str());
}

std::string JoyInsideProtocol::GenerateUUID() {
    char uuid[37];
    uint32_t r1 = esp_random();
    uint32_t r2 = esp_random();
    uint32_t r3 = esp_random();
    uint32_t r4 = esp_random();
    
    snprintf(uuid, sizeof(uuid), 
             "%08lx-%04lx-4%03lx-%04lx-%012llx",
             (unsigned long)r1,
             (unsigned long)((r2 >> 16) & 0xFFFF),
             (unsigned long)(r2 & 0x0FFF),
             (unsigned long)(((r3 >> 16) & 0x3FFF) | 0x8000),
             ((unsigned long long)(r3 & 0xFFFF) << 32) | r4);
    
    return std::string(uuid);
}

std::string JoyInsideProtocol::GenerateMessageId() {
    return GenerateUUID();
}

bool JoyInsideProtocol::InitializeConnection() {
    // 清理旧连接（如果存在）
    if (websocket_) {
        // 设置重连标志，避免 Close() 触发断开回调播放警告音
        bool was_reconnecting = reconnecting_;
        reconnecting_ = true;
        websocket_->Close();
        websocket_.reset();
        reconnecting_ = was_reconnecting;  // 恢复原来的状态
        // 等待 SSL 资源完全释放，避免 mbedtls_ssl_fetch_input 错误
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
    // 如果 Bot ID 未就绪，先进行设备注册
    if (!bot_id_ready_) {
        ESP_LOGI(TAG, "Bot ID not ready, attempting device registration...");
        if (!RegisterDeviceAndSaveBotId()) {
            ESP_LOGE(TAG, "Device registration failed");
            return false;
        }
    }
    
    // 获取 Access Token
    access_token_ = auth_.GetAccessToken();
    if (access_token_.empty()) {
        ESP_LOGE(TAG, "Failed to get access token");
        return false;
    }
    
    // 生成会话 ID（复用旧的以支持可能的上下文恢复）
    if (session_id_.empty()) {
        session_id_ = bot_id_ + "_" + GenerateUUID();
        ESP_LOGI(TAG, "New session: %s", session_id_.c_str());
    } else {
        ESP_LOGI(TAG, "Reusing session: %s", session_id_.c_str());
    }
    std::string request_id = GenerateUUID();
    
    // 构建 WebSocket URL
    char url[512];
    snprintf(url, sizeof(url), 
             "%s?botId=%s&sessionId=%s&requestId=%s",
             JOYINSIDE_WS_URL,
             bot_id_.c_str(),
             session_id_.c_str(),
             request_id.c_str());
    
    ESP_LOGI(TAG, "Connecting to JoyInside: botId=%s", bot_id_.c_str());
    
    // 创建 WebSocket
    auto network = Board::GetInstance().GetNetwork();
    websocket_ = network->CreateWebSocket(1);
    if (!websocket_) {
        ESP_LOGE(TAG, "Failed to create WebSocket");
        return false;
    }
    
    // 设置认证头
    std::string auth_header = "Bearer " + access_token_;
    websocket_->SetHeader("Authorization", auth_header.c_str());
    
    // 设置数据回调
    websocket_->OnData([this](const char* data, size_t len, bool binary) {
        OnWebSocketData(data, len, binary);
    });
    
    // 设置断开回调
    websocket_->OnDisconnected([this]() {
        ESP_LOGI(TAG, "JoyInside WebSocket disconnected");
        StopHeartbeat();
        StopIdleTimeout();
        audio_channel_active_ = false;  // 标记音频通道关闭
        SetDialogState(JoyInsideDialogState::kIdle);
        ResetForNewRound();             // 重置会话状态，确保下次对话正常
        
        // 只有在非重连中且首次连接已成功时，才触发断开回调和设置标志
        // 这样可以避免：1) 重连时的预期断开触发警告音 2) 首次连接前的断开触发警告音
        if (!reconnecting_ && connection_established_) {
            was_disconnected_ = true;   // 标记曾经断开过（用于后续重连成功时播放成功音）
            // 通知应用层网络断开
            if (on_disconnected_) {
                on_disconnected_();
            }
            if (on_audio_channel_closed_) {
                on_audio_channel_closed_();
            }
        }
    });
    
    // 连接服务器
    if (!websocket_->Connect(url)) {
        ESP_LOGE(TAG, "Failed to connect to JoyInside server");
        return false;
    }
    
    ESP_LOGI(TAG, "WebSocket connected, waiting for CFG_BOT_EVENT...");
    
    // 等待服务器配置事件 (最多等待 10 秒)
    EventBits_t bits = xEventGroupWaitBits(
        event_group_handle_, 
        JOYINSIDE_EVENT_SERVER_CONFIG,
        pdTRUE, pdFALSE, 
        pdMS_TO_TICKS(10000));
    
    if (!(bits & JOYINSIDE_EVENT_SERVER_CONFIG)) {
        ESP_LOGE(TAG, "Timeout waiting for CFG_BOT_EVENT");
        return false;
    }
    
    // 发送音频配置
    if (!SendAudioConfig()) {
        ESP_LOGE(TAG, "Failed to send audio config");
        return false;
    }
    
    // 等待配置确认 (最多等待 5 秒)
    bits = xEventGroupWaitBits(
        event_group_handle_,
        JOYINSIDE_EVENT_CONFIG_UPDATED,
        pdTRUE, pdFALSE,
        pdMS_TO_TICKS(5000));
    
    if (!(bits & JOYINSIDE_EVENT_CONFIG_UPDATED)) {
        ESP_LOGW(TAG, "Timeout waiting for SERVER_VOICE_CHAT_UPDATED, continuing anyway");
    }
    
    // 启动心跳
    StartHeartbeat();
    
    // 重置音频帧计数
    audio_frame_index_ = 0;
    
    // 标记首次连接已成功（只有成功后的断开才会触发警告音）
    connection_established_ = true;
    
    ESP_LOGI(TAG, "JoyInside protocol initialized successfully");
    return true;
}

bool JoyInsideProtocol::RegisterDeviceAndSaveBotId() {
    /*
     * 设备注册流程：
     * 1. 使用设备 MAC 地址作为 deviceId
     * 2. 根据配置选择设备类型 (APP_ROBOT/PHYSICAL_ROBOT)
     * 3. 调用设备注册 API 获取 Bot ID
     * 4. 将 Bot ID 存储到 NVS 中
     * 5. 更新 auth_ 模块的 bot_id
     */
    
    // 获取设备 UUID 作为 deviceId
    std::string device_id = Board::GetInstance().GetUuid();
    
    // 获取设备类型
#ifdef CONFIG_JOYINSIDE_DEVICE_TYPE_PRODUCTION
    std::string device_type = "PHYSICAL_ROBOT";
#else
    std::string device_type = "APP_ROBOT";  // 默认测试设备
#endif
    
    // 生成设备名称（与配网名称相同格式：LanDouBao-XXXX）
    // 使用 MAC 地址最后 4 位作为设备标识（大写）
    std::string mac = SystemInfo::GetMacAddress();
    mac.erase(std::remove(mac.begin(), mac.end(), ':'), mac.end());
    std::string suffix = mac.substr(mac.length() - 4);
    std::transform(suffix.begin(), suffix.end(), suffix.begin(), ::toupper);
    std::string device_name = "LanDouBao-" + suffix;
    
    ESP_LOGI(TAG, "Registering device: deviceId=%s, type=%s, name=%s",
             device_id.c_str(), device_type.c_str(), device_name.c_str());
    
    // 调用注册 API
    std::string new_bot_id = auth_.RegisterDevice(device_id, device_type, device_name);
    if (new_bot_id.empty()) {
        ESP_LOGE(TAG, "Device registration failed, cannot obtain Bot ID");
        return false;
    }
    
    // 保存 Bot ID 到 NVS
    {
        Settings settings(NVS_NAMESPACE, true);
        settings.SetString(NVS_KEY_BOT_ID, new_bot_id);
        ESP_LOGI(TAG, "Bot ID saved to NVS: %s", new_bot_id.c_str());
    }
    
    // 更新内部状态
    bot_id_ = new_bot_id;
    bot_id_ready_ = true;
    
    // 更新 auth_ 模块中的 bot_id（用于生成会话 ID 等）
    // 注意：需要重新配置 auth_ 以使用新的 bot_id
#ifdef CONFIG_JOYINSIDE_AUTH_AK_SK
    #ifdef CONFIG_JOYINSIDE_ACCESS_KEY
    std::string access_key = CONFIG_JOYINSIDE_ACCESS_KEY;
    #else
    std::string access_key;
    #endif
    
    #ifdef CONFIG_JOYINSIDE_ACCESS_KEY_SECRET
    std::string access_key_secret = CONFIG_JOYINSIDE_ACCESS_KEY_SECRET;
    #else
    std::string access_key_secret;
    #endif
    
    #ifdef CONFIG_JOYINSIDE_VENDOR_ID
    int vendor_id = CONFIG_JOYINSIDE_VENDOR_ID;
    #else
    int vendor_id = 100090;
    #endif
    
    auth_.Configure(access_key, access_key_secret, vendor_id, bot_id_);
#endif
    
    ESP_LOGI(TAG, "Device registration successful, Bot ID: %s", bot_id_.c_str());
    return true;
}

bool JoyInsideProtocol::OpenAudioChannel() {
    error_occurred_ = false;
    reconnecting_ = true;  // 标记正在重连，避免断开回调触发状态重置
    
    // 停止空闲超时定时器
    StopIdleTimeout();
    
    // 如果之前 SNTP 同步失败过，先尝试同步
    if (!sntp_synced_ && !WaitForSntpSync(10)) {
        reconnecting_ = false;
        SetError("时间同步失败");
        return false;
    }
    
    // 检查是否有可用的预连接（WebSocket 已连接且心跳正常）
    bool has_valid_connection = websocket_ && websocket_->IsConnected() && heartbeat_timer_;
    
    if (has_valid_connection) {
        // 发送心跳验证连接是否真正存活
        ESP_LOGI(TAG, "Verifying pre-established connection...");
        if (SendPing()) {
            ESP_LOGI(TAG, "Pre-established connection is valid");
        } else {
            ESP_LOGW(TAG, "Pre-established connection is dead, reconnecting...");
            has_valid_connection = false;
        }
    }
    
    if (!has_valid_connection) {
        // 需要建立新连接（带重试机制）
        const int MAX_RETRIES = 3;
        const int RETRY_DELAY_MS = 1000;
        bool connected = false;
        
        for (int retry = 0; retry < MAX_RETRIES && !connected; retry++) {
            if (retry > 0) {
                ESP_LOGI(TAG, "Retrying connection (%d/%d)...", retry + 1, MAX_RETRIES);
                vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
            }
            connected = InitializeConnection();
        }
        
        if (!connected) {
            reconnecting_ = false;
            SetError("连接 JoyInside 服务器失败");
            return false;
        }
        
        // 如果之前断开过并且现在重连成功，通知应用层
        if (was_disconnected_ && on_connected_) {
            on_connected_();
            was_disconnected_ = false;  // 重置标志
        }
    }
    
    reconnecting_ = false;  // 重连完成
    
    audio_channel_active_ = true;
    SetDialogState(JoyInsideDialogState::kIdle);
    
    if (on_audio_channel_opened_) {
        on_audio_channel_opened_();
    }
    
    return true;
}

void JoyInsideProtocol::CloseAudioChannel() {
    // JoyInside 保持长连接，不关闭 WebSocket
    // 只重置对话状态，连接保持可用
    audio_channel_active_ = false;
    StopIdleTimeout();
    SetDialogState(JoyInsideDialogState::kIdle);
    
    if (on_audio_channel_closed_) {
        on_audio_channel_closed_();
    }
    
    ESP_LOGI(TAG, "Audio channel closed, connection kept alive");
}

bool JoyInsideProtocol::IsAudioChannelOpened() const {
    return audio_channel_active_ && websocket_ && websocket_->IsConnected() && !error_occurred_;
}

void JoyInsideProtocol::StartHeartbeat() {
    if (heartbeat_timer_) {
        return;
    }
    
    esp_timer_create_args_t timer_args = {
        .callback = HeartbeatCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "joyinside_hb",
        .skip_unhandled_events = true
    };
    
    if (esp_timer_create(&timer_args, &heartbeat_timer_) == ESP_OK) {
        esp_timer_start_periodic(heartbeat_timer_, HEARTBEAT_INTERVAL_MS * 1000);
        ESP_LOGI(TAG, "Heartbeat timer started");
    }
}

void JoyInsideProtocol::StopHeartbeat() {
    if (heartbeat_timer_) {
        esp_timer_stop(heartbeat_timer_);
        esp_timer_delete(heartbeat_timer_);
        heartbeat_timer_ = nullptr;
        ESP_LOGI(TAG, "Heartbeat timer stopped");
    }
}

void JoyInsideProtocol::HeartbeatCallback(void* arg) {
    auto* protocol = static_cast<JoyInsideProtocol*>(arg);
    protocol->SendPing();
}

void JoyInsideProtocol::StartIdleTimeout() {
    // 如果已经有定时器在运行，先停止
    StopIdleTimeout();
    
    if (IDLE_TIMEOUT_SEC == 0) {
        return;  // 超时时间为0表示禁用
    }
    
    esp_timer_create_args_t timer_args = {
        .callback = IdleTimeoutCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "joyinside_idle",
        .skip_unhandled_events = true
    };
    
    if (esp_timer_create(&timer_args, &idle_timeout_timer_) == ESP_OK) {
        esp_timer_start_once(idle_timeout_timer_, (uint64_t)IDLE_TIMEOUT_SEC * 1000000);
        ESP_LOGI(TAG, "Idle timeout started (%d seconds)", IDLE_TIMEOUT_SEC);
    }
}

void JoyInsideProtocol::StopIdleTimeout() {
    if (idle_timeout_timer_) {
        esp_timer_stop(idle_timeout_timer_);
        esp_timer_delete(idle_timeout_timer_);
        idle_timeout_timer_ = nullptr;
    }
}

void JoyInsideProtocol::IdleTimeoutCallback(void* arg) {
    auto* protocol = static_cast<JoyInsideProtocol*>(arg);
    protocol->OnIdleTimeout();
}

void JoyInsideProtocol::OnIdleTimeout() {
    ESP_LOGI(TAG, "Idle timeout reached, closing audio channel (connection kept alive)");
    
    // 标记音频通道为非活跃，但保持 WebSocket 连接
    audio_channel_active_ = false;
    idle_timeout_timer_ = nullptr;  // 定时器已经触发，清空指针
    
    // 播放成功提示音，表示进入可唤醒状态
    Application::GetInstance().PlaySound(Lang::Sounds::OGG_SUCCESS);
    
    // 通知应用层音频通道已关闭
    if (on_audio_channel_closed_) {
        on_audio_channel_closed_();
    }
}

bool JoyInsideProtocol::SendPing() {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "mid", GenerateMessageId().c_str());
    cJSON_AddStringToObject(root, "contentType", "PING");
    cJSON_AddStringToObject(root, "uid", user_id_.c_str());
    
    char* json_str = cJSON_PrintUnformatted(root);
    bool result = SendText(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    
    return result;
}

bool JoyInsideProtocol::SendAudioFinish() {
    // 发送 CLIENT_AUDIO_FINISH 事件（手动模式下结束音频上传）
    if (!IsAudioChannelOpened()) {
        return false;
    }
    
    ESP_LOGI(TAG, "Sending CLIENT_AUDIO_FINISH");
    
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "mid", GenerateMessageId().c_str());
    cJSON_AddStringToObject(root, "contentType", "EVENT");
    cJSON_AddStringToObject(root, "uid", user_id_.c_str());
    
    cJSON* content = cJSON_CreateObject();
    cJSON_AddStringToObject(content, "eventType", "CLIENT_AUDIO_FINISH");
    cJSON_AddItemToObject(root, "content", content);
    
    char* json_str = cJSON_PrintUnformatted(root);
    bool result = SendText(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    
    return result;
}

bool JoyInsideProtocol::SendInterrupt() {
    // 发送 CLIENT_INTERRUPT 事件（打断 TTS 播放）
    if (!IsAudioChannelOpened()) {
        return false;
    }
    
    ESP_LOGI(TAG, "Sending CLIENT_INTERRUPT");
    
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "mid", GenerateMessageId().c_str());
    cJSON_AddStringToObject(root, "contentType", "EVENT");
    cJSON_AddStringToObject(root, "uid", user_id_.c_str());
    
    cJSON* content = cJSON_CreateObject();
    cJSON_AddStringToObject(content, "eventType", "CLIENT_INTERRUPT");
    cJSON_AddItemToObject(root, "content", content);
    
    char* json_str = cJSON_PrintUnformatted(root);
    bool result = SendText(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    
    return result;
}

bool JoyInsideProtocol::SendAudioConfig() {
    /*
     * 发送 CLIENT_VOICE_CHAT_UPDATE 消息配置音频参数
     * 
     * 参考 C++ SDK: 使用二进制模式直接发送音频数据，效率更高
     * binary=true: 上行音频使用 WebSocket 二进制帧发送
     * binary=false: 上行音频使用 Base64 编码的 JSON 发送
     * 
     * 上行: OPUS 16kHz, 60ms 帧
     * 下行: OPUS 16kHz, 60ms 帧
     */
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "mid", GenerateMessageId().c_str());
    cJSON_AddStringToObject(root, "contentType", "EVENT");
    cJSON_AddStringToObject(root, "uid", user_id_.c_str());
    
    cJSON* content = cJSON_CreateObject();
    cJSON_AddStringToObject(content, "eventType", "CLIENT_VOICE_CHAT_UPDATE");
    
    cJSON* eventData = cJSON_CreateObject();
    cJSON* audio = cJSON_CreateObject();
    
    // 使用二进制模式发送音频（参考 C++ SDK）
    // binary=true 可以直接发送二进制数据，无需 Base64 编码
    cJSON_AddBoolToObject(audio, "binary", use_binary_mode_);
    
    // 上行音频配置
    cJSON* input = cJSON_CreateObject();
    cJSON_AddStringToObject(input, "codec", "opus");
    cJSON_AddStringToObject(input, "sampleRate", "16000");
    cJSON_AddStringToObject(input, "frameSize", "1920");  // 960 samples * 2 bytes
    cJSON_AddItemToObject(audio, "input", input);
    
    // 下行音频配置
    cJSON* output = cJSON_CreateObject();
    cJSON_AddStringToObject(output, "codec", "opus");
    cJSON_AddStringToObject(output, "sampleRate", "16000");
    cJSON_AddStringToObject(output, "frameSizeMs", "60");
    cJSON_AddItemToObject(audio, "output", output);
    
    // 音色配置
    cJSON* timbre = cJSON_CreateObject();
    // 语速：CONFIG_JOYINSIDE_VOICE_SPEED (80-120) -> 0.8-1.2
    float voice_speed = CONFIG_JOYINSIDE_VOICE_SPEED / 100.0f;
    // 音量：CONFIG_JOYINSIDE_VOICE_VOLUME (5-100) -> 0.5-10.0
    float voice_volume = CONFIG_JOYINSIDE_VOICE_VOLUME / 10.0f;
    cJSON_AddNumberToObject(timbre, "voiceSpeed", voice_speed);
    cJSON_AddNumberToObject(timbre, "voiceVolume", voice_volume);
    cJSON_AddItemToObject(audio, "timbre", timbre);
    
    cJSON_AddItemToObject(eventData, "audio", audio);
    cJSON_AddItemToObject(content, "eventData", eventData);
    cJSON_AddItemToObject(root, "content", content);
    
    char* json_str = cJSON_PrintUnformatted(root);
    ESP_LOGI(TAG, "Sending audio config: binary=%s, codec=opus", 
             use_binary_mode_ ? "true" : "false");
    ESP_LOGD(TAG, "Config JSON: %s", json_str);
    
    bool result = SendText(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    
    return result;
}

bool JoyInsideProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    if (!IsAudioChannelOpened()) {
        return false;
    }
    
    // 递增帧序号
    uint32_t index = ++audio_frame_index_;
    
    if (use_binary_mode_) {
        // 二进制模式: 直接发送音频数据 (参考 C++ SDK)
        return SendBinaryAudioFrame(packet->payload.data(), packet->payload.size());
    } else {
        // JSON 模式: Base64 编码发送
        return SendJsonAudioFrame(packet->payload.data(), packet->payload.size(), index);
    }
}

bool JoyInsideProtocol::SendBinaryAudioFrame(const uint8_t* data, size_t len) {
    // 直接发送二进制音频数据（参考 C++ SDK 的 ws_send_binary_data）
    if (!websocket_ || !websocket_->IsConnected()) {
        return false;
    }
    
    // WebSocket 二进制帧发送
    // 使用 Send(const void* data, size_t len, bool binary, bool fin) 接口
    if (!websocket_->Send(data, len, true, true)) {
        ESP_LOGE(TAG, "Failed to send binary audio frame");
        return false;
    }
    
    return true;
}

bool JoyInsideProtocol::SendJsonAudioFrame(const uint8_t* data, size_t len, uint32_t index) {
    // Base64 编码音频数据
    size_t base64_len = 0;
    mbedtls_base64_encode(nullptr, 0, &base64_len, data, len);
    
    std::vector<char> base64_buf(base64_len + 1);
    mbedtls_base64_encode((unsigned char*)base64_buf.data(), base64_len, &base64_len, data, len);
    base64_buf[base64_len] = '\0';
    
    // 构建 AUDIO 消息
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "mid", GenerateMessageId().c_str());
    cJSON_AddStringToObject(root, "contentType", "AUDIO");
    cJSON_AddStringToObject(root, "uid", user_id_.c_str());
    
    cJSON* content = cJSON_CreateObject();
    cJSON_AddStringToObject(content, "audioBase64", base64_buf.data());
    cJSON_AddNumberToObject(content, "index", index);
    cJSON_AddItemToObject(root, "content", content);
    
    char* json_str = cJSON_PrintUnformatted(root);
    bool result = SendText(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    
    return result;
}

bool JoyInsideProtocol::SendText(const std::string& text) {
    if (!websocket_ || !websocket_->IsConnected()) {
        return false;
    }
    
    if (!websocket_->Send(text)) {
        ESP_LOGE(TAG, "Failed to send text message");
        return false;
    }
    
    return true;
}

void JoyInsideProtocol::OnWebSocketData(const char* data, size_t len, bool binary) {
    last_incoming_time_ = std::chrono::steady_clock::now();
    
    if (binary) {
        // 二进制模式下接收 TTS 音频数据 (参考 C++ SDK)
        if (use_binary_mode_) {
            HandleBinaryTTS(reinterpret_cast<const uint8_t*>(data), len);
        } else {
            ESP_LOGW(TAG, "Unexpected binary message in JSON mode");
        }
        return;
    }
    
    ParseJsonMessage(data, len);
}

void JoyInsideProtocol::ParseJsonMessage(const char* data, size_t len) {
    cJSON* root = cJSON_Parse(data);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse JSON message");
        return;
    }
    
    cJSON* contentType = cJSON_GetObjectItem(root, "contentType");
    if (!cJSON_IsString(contentType)) {
        ESP_LOGW(TAG, "Missing contentType in message");
        cJSON_Delete(root);
        return;
    }
    
    const char* type = contentType->valuestring;
    cJSON* content = cJSON_GetObjectItem(root, "content");
    
    if (strcmp(type, "PONG") == 0) {
        ESP_LOGD(TAG, "Received PONG");
    } else if (strcmp(type, "EVENT") == 0) {
        HandleEvent(content);
    } else if (strcmp(type, "ASR") == 0) {
        HandleASR(content);
    } else if (strcmp(type, "AGENT") == 0) {
        HandleAgent(content);
    } else if (strcmp(type, "TTS") == 0) {
        HandleTTS(content);
    } else if (strcmp(type, "ACTIVITY") == 0) {
        HandleActivity(content);
    } else {
        ESP_LOGD(TAG, "Unknown message type: %s", type);
    }
    
    cJSON_Delete(root);
}

void JoyInsideProtocol::HandleEvent(const cJSON* content) {
    if (!content) return;
    
    cJSON* eventType = cJSON_GetObjectItem(content, "eventType");
    if (!cJSON_IsString(eventType)) return;
    
    const char* type = eventType->valuestring;
    cJSON* eventData = cJSON_GetObjectItem(content, "eventData");
    cJSON* roundId = cJSON_GetObjectItem(content, "roundId");
    
    // 关键事件用 INFO，打断后的残留事件用 DEBUG
    bool is_post_interrupt_event = (dialog_state_ == JoyInsideDialogState::kIdle) &&
        (strcmp(type, "TTS_COMPLETE") == 0 || strcmp(type, "COMPLETE") == 0 || strcmp(type, "INTERRUPT") == 0);
    
    if (is_post_interrupt_event) {
        ESP_LOGD(TAG, "Received event: %s (ignored in IDLE)", type);
    } else {
        ESP_LOGI(TAG, "Received event: %s", type);
    }
    
    if (strcmp(type, "CFG_BOT_EVENT") == 0) {
        // 服务端配置事件
        xEventGroupSetBits(event_group_handle_, JOYINSIDE_EVENT_SERVER_CONFIG);
        
    } else if (strcmp(type, "SERVER_VOICE_CHAT_UPDATED") == 0) {
        // 音频配置确认
        xEventGroupSetBits(event_group_handle_, JOYINSIDE_EVENT_CONFIG_UPDATED);
        
    } else if (strcmp(type, "CALL_AGENT_START_EVENT") == 0) {
        // 智能体开始处理
        if (cJSON_IsString(roundId)) {
            current_round_id_ = roundId->valuestring;
        }
        SetDialogState(JoyInsideDialogState::kProcessing);
        
    } else if (strcmp(type, "TTS_SENTENCE_START") == 0) {
        // 字幕开始 - 通知应用层 TTS 开始
        if (eventData) {
            cJSON* text = cJSON_GetObjectItem(eventData, "text");
            if (cJSON_IsString(text)) {
                ESP_LOGI(TAG, "TTS Subtitle: %s", text->valuestring);
                
                // 首次 TTS 开始时，通知应用层进入播放状态
                if (!tts_started_) {
                    tts_started_ = true;
                    // 发送 tts start 消息给应用层，触发 kDeviceStateSpeaking
                    if (on_incoming_json_) {
                        cJSON* start_msg = cJSON_CreateObject();
                        cJSON_AddStringToObject(start_msg, "type", "tts");
                        cJSON_AddStringToObject(start_msg, "state", "start");
                        on_incoming_json_(start_msg);
                        cJSON_Delete(start_msg);
                        ESP_LOGI(TAG, "Notified app layer: TTS start");
                    }
                }
                
                // 发送字幕给应用层显示
                if (on_incoming_json_) {
                    cJSON* subtitle_msg = cJSON_CreateObject();
                    cJSON_AddStringToObject(subtitle_msg, "type", "tts");
                    cJSON_AddStringToObject(subtitle_msg, "state", "sentence_start");
                    cJSON_AddStringToObject(subtitle_msg, "text", text->valuestring);
                    on_incoming_json_(subtitle_msg);
                    cJSON_Delete(subtitle_msg);
                }
            }
        }
        
    } else if (strcmp(type, "TTS_COMPLETE") == 0) {
        // TTS 播放完成
        // 只在 SPEAKING 状态下处理，避免打断后重复处理
        if (dialog_state_ == JoyInsideDialogState::kSpeaking) {
            xEventGroupSetBits(event_group_handle_, JOYINSIDE_EVENT_TTS_COMPLETE);
            ESP_LOGI(TAG, "TTS playback complete");
            
            // 通知应用层 TTS 结束
            if (on_incoming_json_) {
                cJSON* stop_msg = cJSON_CreateObject();
                cJSON_AddStringToObject(stop_msg, "type", "tts");
                cJSON_AddStringToObject(stop_msg, "state", "stop");
                on_incoming_json_(stop_msg);
                cJSON_Delete(stop_msg);
                ESP_LOGI(TAG, "Notified app layer: TTS stop");
            }
            
            // 正常完成，进入 IDLE
            SetDialogState(JoyInsideDialogState::kIdle);
        } else {
            ESP_LOGD(TAG, "Ignoring TTS_COMPLETE in %s state", 
                     dialog_state_ == JoyInsideDialogState::kIdle ? "IDLE" : "non-SPEAKING");
        }
        
    } else if (strcmp(type, "COMPLETE") == 0) {
        // 对话轮次完成 - 只在非 IDLE 状态下处理
        if (dialog_state_ != JoyInsideDialogState::kIdle) {
            SetDialogState(JoyInsideDialogState::kIdle);
        }
        ResetForNewRound();
        
    } else if (strcmp(type, "INTERRUPT") == 0 || strcmp(type, "CALL_AGENT_INTERRUPTED") == 0) {
        // ⚠️ 打断事件 - INTERRUPT 和 CALL_AGENT_INTERRUPTED 都表示打断
        // 只有在非 IDLE 状态下才处理打断（避免重复打断）
        if (dialog_state_ == JoyInsideDialogState::kIdle) {
            ESP_LOGD(TAG, "Ignoring INTERRUPT in IDLE state");
        } else {
            ESP_LOGW(TAG, "INTERRUPTED! Stopping playback...");
            
            // 记录被打断的轮次 ID
            if (cJSON_IsString(roundId)) {
                interrupted_round_id_ = roundId->valuestring;
            }
            
            // 执行打断处理
            HandleInterrupt();
            xEventGroupSetBits(event_group_handle_, JOYINSIDE_EVENT_INTERRUPTED);
        }
        
    } else if (strcmp(type, "EMPTY_CONTENT") == 0) {
        // 未识别到有效内容
        // JoyInside 协议层回到 IDLE，但 Application 层保持 listening 状态继续监听
        ESP_LOGI(TAG, "No valid speech detected, protocol idle but app keeps listening");
        // 直接设置协议状态，不通过 SetDialogState 避免重启 idle timeout
        JoyInsideDialogState old_state = dialog_state_.exchange(JoyInsideDialogState::kIdle);
        if (old_state != JoyInsideDialogState::kIdle) {
            const char* state_names[] = {"IDLE", "LISTENING", "PROCESSING", "SPEAKING", "INTERRUPTED"};
            ESP_LOGI(TAG, "Dialog state: %s -> IDLE (no timeout reset)", state_names[(int)old_state]);
        }
        // 注意：不通知 Application 层，让它保持 listening 状态继续监听用户说话
        
    } else if (strcmp(type, "USER_AUDIT_FAIL") == 0) {
        // 输入内容审核失败
        ESP_LOGW(TAG, "User content audit failed");
        
    } else if (strcmp(type, "REPEAT_CLIENT_SESSION") == 0) {
        // 设备互踢
        ESP_LOGE(TAG, "Session kicked by another connection");
        error_occurred_ = true;
        if (on_network_error_) {
            on_network_error_("设备被其他连接踢下线");
        }
    }
}

void JoyInsideProtocol::HandleASR(const cJSON* content) {
    if (!content) return;
    
    cJSON* text = cJSON_GetObjectItem(content, "text");
    cJSON* textType = cJSON_GetObjectItem(content, "textType");
    
    if (cJSON_IsString(text)) {
        bool isFinal = textType && cJSON_IsString(textType) && 
                       strcmp(textType->valuestring, "IS_FINAL") == 0;
        
        const char* asr_text = text->valuestring;
        ESP_LOGI(TAG, "ASR %s: %s", isFinal ? "(Final)" : "(Partial)", asr_text);
        
        // 只有非空的 ASR 结果且当前不在 LISTENING/SPEAKING/INTERRUPTED 状态才切换
        // SPEAKING 状态下收到 ASR 表示用户正在打断，等待 CALL_AGENT_INTERRUPTED 统一处理
        // 避免 SPEAKING -> LISTENING -> INTERRUPTED 的冗余状态切换
        if (strlen(asr_text) > 0 && 
            dialog_state_ != JoyInsideDialogState::kListening &&
            dialog_state_ != JoyInsideDialogState::kSpeaking &&
            dialog_state_ != JoyInsideDialogState::kInterrupted) {
            SetDialogState(JoyInsideDialogState::kListening);
        }
    }
}

void JoyInsideProtocol::HandleAgent(const cJSON* content) {
    if (!content) return;
    
    cJSON* agentContent = cJSON_GetObjectItem(content, "content");
    cJSON* finishReason = cJSON_GetObjectItem(content, "finishReason");
    
    if (cJSON_IsString(agentContent) && strlen(agentContent->valuestring) > 0) {
        ESP_LOGD(TAG, "Agent: %s", agentContent->valuestring);
    }
    
    if (cJSON_IsString(finishReason) && strlen(finishReason->valuestring) > 0) {
        ESP_LOGI(TAG, "Agent finished: %s", finishReason->valuestring);
    }
}

void JoyInsideProtocol::HandleTTS(const cJSON* content) {
    if (!content) {
        ESP_LOGW(TAG, "HandleTTS: content is null");
        return;
    }
    
    ESP_LOGD(TAG, "HandleTTS: Received TTS JSON message");
    
    // 检查是否需要丢弃（被打断的轮次）
    cJSON* roundId = cJSON_GetObjectItem(content, "roundId");
    if (cJSON_IsString(roundId) && !interrupted_round_id_.empty() &&
        interrupted_round_id_ == roundId->valuestring) {
        ESP_LOGD(TAG, "Discarding TTS frame from interrupted round");
        return;
    }
    
    cJSON* audioBase64 = cJSON_GetObjectItem(content, "audioBase64");
    cJSON* audioAue = cJSON_GetObjectItem(content, "audioAue");
    cJSON* audioDuration = cJSON_GetObjectItem(content, "audioDuration");
    cJSON* finish = cJSON_GetObjectItem(content, "finish");
    
    if (!cJSON_IsString(audioBase64) || strlen(audioBase64->valuestring) == 0) {
        return;
    }
    
    // 首次收到 TTS 音频数据时，通知应用层进入播放状态
    if (!tts_started_) {
        tts_started_ = true;
        if (on_incoming_json_) {
            cJSON* start_msg = cJSON_CreateObject();
            cJSON_AddStringToObject(start_msg, "type", "tts");
            cJSON_AddStringToObject(start_msg, "state", "start");
            on_incoming_json_(start_msg);
            cJSON_Delete(start_msg);
            ESP_LOGI(TAG, "Notified app layer: TTS start (JSON mode)");
        }
    }
    
    // Base64 解码
    size_t base64_len = strlen(audioBase64->valuestring);
    size_t decoded_len = 0;
    
    // 计算解码后的大小
    mbedtls_base64_decode(nullptr, 0, &decoded_len, 
                          (const unsigned char*)audioBase64->valuestring, base64_len);
    
    std::vector<uint8_t> decoded_data(decoded_len);
    mbedtls_base64_decode(decoded_data.data(), decoded_len, &decoded_len,
                          (const unsigned char*)audioBase64->valuestring, base64_len);
    decoded_data.resize(decoded_len);
    
    // 更新对话状态
    if (dialog_state_ != JoyInsideDialogState::kSpeaking) {
        SetDialogState(JoyInsideDialogState::kSpeaking);
    }
    
    // 确定音频格式
    std::string format = "opus";
    if (cJSON_IsString(audioAue)) {
        format = audioAue->valuestring;
    }
    
    // 将音频数据传递给上层
    if (on_incoming_audio_) {
        auto packet = std::make_unique<AudioStreamPacket>();
        packet->sample_rate = SAMPLE_RATE;
        packet->frame_duration = cJSON_IsNumber(audioDuration) ? audioDuration->valueint : FRAME_DURATION_MS;
        packet->timestamp = 0;
        packet->payload = std::move(decoded_data);
        
        on_incoming_audio_(std::move(packet));
    }
    
    // 检查是否是最后一帧
    if (cJSON_IsBool(finish) && cJSON_IsTrue(finish)) {
        ESP_LOGD(TAG, "TTS frame marked as finish");
    }
}

void JoyInsideProtocol::HandleBinaryTTS(const uint8_t* data, size_t len) {
    /*
     * 二进制模式 TTS 接收 (参考 C++ SDK)
     * 
     * 在 binary=true 模式下，服务端直接发送音频二进制数据，
     * 无需 Base64 解码，效率更高
     */
    if (len == 0) {
        ESP_LOGW(TAG, "HandleBinaryTTS: empty data");
        return;
    }
    
    // 仅在调试模式下打印每帧信息
    ESP_LOGD(TAG, "Binary TTS: %d bytes", (int)len);
    
    // 首次收到二进制 TTS 数据时，通知应用层进入播放状态
    if (!tts_started_) {
        tts_started_ = true;
        if (on_incoming_json_) {
            cJSON* start_msg = cJSON_CreateObject();
            cJSON_AddStringToObject(start_msg, "type", "tts");
            cJSON_AddStringToObject(start_msg, "state", "start");
            on_incoming_json_(start_msg);
            cJSON_Delete(start_msg);
            ESP_LOGI(TAG, "Notified app layer: TTS start (binary mode)");
        }
    }
    
    // 更新对话状态
    if (dialog_state_ != JoyInsideDialogState::kSpeaking) {
        SetDialogState(JoyInsideDialogState::kSpeaking);
    }
    
    // 将音频数据传递给上层
    if (on_incoming_audio_) {
        auto packet = std::make_unique<AudioStreamPacket>();
        packet->sample_rate = SAMPLE_RATE;
        packet->frame_duration = FRAME_DURATION_MS;
        packet->timestamp = 0;
        packet->payload.assign(data, data + len);
        
        on_incoming_audio_(std::move(packet));
    }
    
    ESP_LOGD(TAG, "Received binary TTS: %zu bytes", len);
}

void JoyInsideProtocol::HandleActivity(const cJSON* content) {
    // 处理服务端主动推送的消息
    if (!content) return;
    
    ESP_LOGI(TAG, "Received activity message");
    // TODO: 根据需要处理主动推送
}

void JoyInsideProtocol::HandleInterrupt() {
    SetDialogState(JoyInsideDialogState::kInterrupted);
    
    // 清空预缓冲队列
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        tts_buffer_.clear();
        prebuffering_ = true;
    }
    
    // 调用打断回调通知 AudioService 停止播放
    if (on_interrupt_) {
        ESP_LOGI(TAG, "Calling interrupt callback");
        on_interrupt_();
    }
    
    ESP_LOGI(TAG, "Interrupt handled, ready for new input");
    
    // 过渡回空闲状态
    SetDialogState(JoyInsideDialogState::kIdle);
}

void JoyInsideProtocol::SetDialogState(JoyInsideDialogState state) {
    JoyInsideDialogState old_state = dialog_state_.exchange(state);
    
    if (old_state != state) {
        const char* state_names[] = {"IDLE", "LISTENING", "PROCESSING", "SPEAKING", "INTERRUPTED"};
        ESP_LOGI(TAG, "Dialog state: %s -> %s", 
                 state_names[(int)old_state], state_names[(int)state]);
        
        // 当进入 IDLE 状态且音频通道活跃时，启动空闲超时定时器
        if (state == JoyInsideDialogState::kIdle && audio_channel_active_) {
            StartIdleTimeout();
        } else if (state != JoyInsideDialogState::kIdle) {
            // 非 IDLE 状态，停止空闲超时定时器
            StopIdleTimeout();
        }
    }
}

void JoyInsideProtocol::ResetForNewRound() {
    current_round_id_.clear();
    interrupted_round_id_.clear();
    tts_started_ = false;  // 重置 TTS 开始标志
    
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    tts_buffer_.clear();
    prebuffering_ = true;
}

bool JoyInsideProtocol::WaitForSntpSync(int max_wait_sec) {
    // 如果已经同步成功过，直接返回
    if (sntp_synced_) {
        return true;
    }
    
    // 检查当前是否已同步
    if (JoyInsideIsTimeSynced()) {
        sntp_synced_ = true;
        ESP_LOGI(TAG, "SNTP already synced");
        return true;
    }
    
    // 等待同步
    ESP_LOGI(TAG, "Waiting for SNTP time sync (max %d seconds)...", max_wait_sec);
    for (int i = 0; i < max_wait_sec; ++i) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (JoyInsideIsTimeSynced()) {
            sntp_synced_ = true;
            ESP_LOGI(TAG, "SNTP synced after %d seconds", i + 1);
            return true;
        }
    }
    
    ESP_LOGW(TAG, "SNTP sync timeout after %d seconds", max_wait_sec);
    return false;
}
