#include "joyinside_auth.h"

#include <cstring>
#include <algorithm>
#include <map>
#include <cJSON.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <esp_sntp.h>
#include <mbedtls/md.h>

#define TAG "JoyInsideAuth"

// API URLs
static const char* URL_AUTH_GET_TOKEN = "https://joyinside.jd.com/auth/getToken";
static const char* URL_AUTH_REFRESH_TOKEN = "https://joyinside.jd.com/auth/refreshToken";
static const char* URL_DEVICE_REGISTER = "https://joyinside.jd.com/device/register";

// Token 提前刷新时间 (5分钟)
static const int64_t TOKEN_REFRESH_MARGIN_MS = 5 * 60 * 1000;

// SNTP 时间同步
static bool sntp_initialized = false;

void JoyInsideInitSntp() {
    if (sntp_initialized) {
        return;
    }
    
    ESP_LOGI(TAG, "Initializing SNTP...");
    
    // 注意：不设置时区！SNTP 同步的是 UTC 时间
    // API 签名需要的是 UTC 时间戳，不需要时区转换
    // 如果需要显示本地时间，在显示时再转换
    
    // 设置 SNTP 同步间隔为 1 小时
    esp_sntp_set_sync_interval(3600 * 1000);  // 毫秒
    
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "pool.ntp.org");
    esp_sntp_setservername(2, "time.windows.com");
    esp_sntp_init();
    sntp_initialized = true;
    
    ESP_LOGI(TAG, "SNTP initialized, waiting for sync...");
}

bool JoyInsideIsTimeSynced() {
    // 检查 SNTP 同步状态
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET) {
        return false;
    }
    
    // 额外验证：确保时间已经被更新到合理范围（2024年之后）
    time_t now;
    time(&now);
    // 2024-01-01 00:00:00 UTC 的时间戳是 1704067200
    if (now < 1704067200) {
        ESP_LOGW(TAG, "SNTP status says synced but time is still invalid: %ld", (long)now);
        return false;
    }
    
    return true;
}

static bool CheckTimeValid() {
    // 检查时间是否有效（2024年之后）
    struct timeval tv;
    gettimeofday(&tv, NULL);
    
    // 2024-01-01 00:00:00 UTC 的时间戳是 1704067200
    if (tv.tv_sec < 1704067200) {
        ESP_LOGW(TAG, "System time is invalid (timestamp=%ld, before 2024), auth may fail", 
                 (long)tv.tv_sec);
        return false;
    }
    
    // 打印当前 UTC 时间用于调试
    struct tm timeinfo;
    gmtime_r(&tv.tv_sec, &timeinfo);
    ESP_LOGI(TAG, "System time valid, UTC: %04d-%02d-%02d %02d:%02d:%02d",
             timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    
    return true;
}

JoyInsideAuth::JoyInsideAuth()
    : vendor_id_(100090),
      token_expire_time_(0),
      use_static_token_(false) {
}

void JoyInsideAuth::Configure(const std::string& access_key,
                               const std::string& access_key_secret,
                               int vendor_id,
                               const std::string& bot_id) {
    access_key_ = access_key;
    access_key_secret_ = access_key_secret;
    vendor_id_ = vendor_id;
    bot_id_ = bot_id;
    use_static_token_ = false;
    
    ESP_LOGI(TAG, "Configured AK/SK auth, vendorId=%d, botId=%s", 
             vendor_id_, bot_id_.c_str());
}

void JoyInsideAuth::SetStaticToken(const std::string& token) {
    access_token_ = token;
    use_static_token_ = true;
    token_expire_time_ = 0;  // 静态 Token 不过期（假设）
    
    ESP_LOGI(TAG, "Using static token");
}

bool JoyInsideAuth::IsTokenValid() const {
    if (access_token_.empty()) {
        return false;
    }
    
    if (use_static_token_) {
        return true;  // 静态 Token 假设始终有效
    }
    
    // 检查是否过期（提前 5 分钟刷新）
    int64_t now = GetCurrentTimeMs();
    return now < (token_expire_time_ - TOKEN_REFRESH_MARGIN_MS);
}

std::string JoyInsideAuth::GetAccessToken() {
    // 如果 Token 有效，直接返回
    if (IsTokenValid()) {
        return access_token_;
    }
    
    // 如果是静态 Token 模式，直接返回（即使可能无效）
    if (use_static_token_) {
        return access_token_;
    }
    
    // 尝试刷新
    if (!refresh_token_.empty()) {
        std::string new_token = RefreshToken();
        if (!new_token.empty()) {
            return new_token;
        }
    }
    
    // 重新获取
    if (FetchToken()) {
        return access_token_;
    }
    
    ESP_LOGE(TAG, "Failed to get access token");
    return "";
}

std::string JoyInsideAuth::RefreshToken() {
    if (refresh_token_.empty()) {
        ESP_LOGW(TAG, "No refresh token available");
        return "";
    }
    
    // 构建刷新请求
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "accessKeyId", access_key_.c_str());
    cJSON_AddStringToObject(root, "refreshToken", refresh_token_.c_str());
    cJSON_AddStringToObject(root, "botId", bot_id_.c_str());
    
    char* json_str = cJSON_PrintUnformatted(root);
    std::string request_body = json_str;
    cJSON_free(json_str);
    cJSON_Delete(root);
    
    std::string response;
    int status = HttpPost(URL_AUTH_REFRESH_TOKEN, request_body, response);
    
    if (status != 200) {
        ESP_LOGE(TAG, "Refresh token failed, status=%d", status);
        refresh_token_.clear();  // 清除无效的 refresh token
        return "";
    }
    
    // 解析响应
    cJSON* resp_root = cJSON_Parse(response.c_str());
    if (!resp_root) {
        ESP_LOGE(TAG, "Failed to parse refresh response");
        return "";
    }
    
    cJSON* access_token = cJSON_GetObjectItem(resp_root, "accessToken");
    cJSON* new_refresh_token = cJSON_GetObjectItem(resp_root, "refreshToken");
    cJSON* expire_in = cJSON_GetObjectItem(resp_root, "expireIn");
    
    if (cJSON_IsString(access_token)) {
        access_token_ = access_token->valuestring;
        
        if (cJSON_IsString(new_refresh_token)) {
            refresh_token_ = new_refresh_token->valuestring;
        }
        
        // 更新过期时间
        if (cJSON_IsNumber(expire_in)) {
            token_expire_time_ = GetCurrentTimeMs() + (int64_t)(expire_in->valuedouble * 1000);
        }
        
        ESP_LOGI(TAG, "Token refreshed successfully");
    }
    
    cJSON_Delete(resp_root);
    return access_token_;
}

bool JoyInsideAuth::FetchToken() {
    if (access_key_.empty() || access_key_secret_.empty()) {
        ESP_LOGE(TAG, "AK/SK not configured");
        return false;
    }
    
    // 检查时间是否有效（SNTP 应该在 WiFi 连接后已经同步）
    CheckTimeValid();
    
    int64_t now_ms = GetCurrentTimeMs();
    std::string timestamp = std::to_string(now_ms);
    
    // 打印当前时间用于调试
    time_t now_sec = now_ms / 1000;
    struct tm timeinfo;
    gmtime_r(&now_sec, &timeinfo);  // 使用 UTC 时间便于调试
    ESP_LOGI(TAG, "Current UTC time: %04d-%02d-%02d %02d:%02d:%02d (timestamp_ms=%s)",
             timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec, timestamp.c_str());
    
    std::string nonce = GenerateUUID();
    std::string sign = GenerateSign("V2", timestamp, nonce);
    
    ESP_LOGI(TAG, "Fetching token with timestamp=%s, nonce=%s", timestamp.c_str(), nonce.c_str());
    
    // 构建请求
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "accessKeyId", access_key_.c_str());
    cJSON_AddStringToObject(root, "accessTimestamp", timestamp.c_str());
    cJSON_AddStringToObject(root, "accessNonce", nonce.c_str());
    cJSON_AddStringToObject(root, "accessVersion", "V2");
    cJSON_AddStringToObject(root, "accessSign", sign.c_str());
    
    // 如果有 botId 则使用 botId（设备 Token），否则使用 vendorId（厂商 Token）
    if (!bot_id_.empty()) {
        cJSON_AddStringToObject(root, "botId", bot_id_.c_str());
    } else {
        cJSON_AddNumberToObject(root, "vendorId", vendor_id_);
    }
    
    char* json_str = cJSON_PrintUnformatted(root);
    std::string request_body = json_str;
    cJSON_free(json_str);
    cJSON_Delete(root);
    
    ESP_LOGI(TAG, "Token request: %s", request_body.c_str());
    
    std::string response;
    int status = HttpPost(URL_AUTH_GET_TOKEN, request_body, response);
    
    if (status != 200) {
        ESP_LOGE(TAG, "Get token failed, status=%d", status);
        if (!response.empty()) {
            ESP_LOGE(TAG, "Error response: %s", response.c_str());
        }
        return false;
    }
    
    ESP_LOGI(TAG, "Token response: %s", response.c_str());
    
    // 解析响应
    cJSON* resp_root = cJSON_Parse(response.c_str());
    if (!resp_root) {
        ESP_LOGE(TAG, "Failed to parse token response");
        return false;
    }
    
    cJSON* access_token = cJSON_GetObjectItem(resp_root, "accessToken");
    cJSON* refresh_token = cJSON_GetObjectItem(resp_root, "refreshToken");
    cJSON* expire_in = cJSON_GetObjectItem(resp_root, "expireIn");
    
    if (!cJSON_IsString(access_token)) {
        // 检查是否有错误信息
        cJSON* error = cJSON_GetObjectItem(resp_root, "error");
        cJSON* message = cJSON_GetObjectItem(resp_root, "message");
        if (cJSON_IsString(error) || cJSON_IsString(message)) {
            ESP_LOGE(TAG, "Token error: %s - %s", 
                     error ? error->valuestring : "",
                     message ? message->valuestring : "");
        }
        cJSON_Delete(resp_root);
        return false;
    }
    
    access_token_ = access_token->valuestring;
    
    if (cJSON_IsString(refresh_token)) {
        refresh_token_ = refresh_token->valuestring;
    }
    
    // 更新过期时间
    if (cJSON_IsNumber(expire_in)) {
        token_expire_time_ = GetCurrentTimeMs() + (int64_t)(expire_in->valuedouble * 1000);
        ESP_LOGI(TAG, "Token expires in %.0f seconds", expire_in->valuedouble);
    }
    
    ESP_LOGI(TAG, "Token obtained successfully");
    cJSON_Delete(resp_root);
    return true;
}

std::string JoyInsideAuth::GenerateSign(const std::string& access_version,
                                         const std::string& timestamp,
                                         const std::string& nonce) {
    /*
     * 签名算法（参考 Python 实现）:
     * 1. 将参数按 key 小写后排序
     * 2. 拼接为 key=value&key=value 格式
     * 3. 使用 HMAC-MD5 计算签名
     * 4. 返回十六进制字符串
     */
    
    // 构建参数 map（key 小写）
    std::map<std::string, std::string> params;
    params["accessversion"] = access_version;
    params["accesstimestamp"] = timestamp;
    params["accessnonce"] = nonce;
    params["accesskeyid"] = access_key_;
    
    // 按 key 排序拼接
    std::string joint_params;
    for (const auto& kv : params) {
        if (!joint_params.empty()) {
            joint_params += "&";
        }
        joint_params += kv.first + "=" + kv.second;
    }
    
    ESP_LOGI(TAG, "Sign string: %s", joint_params.c_str());
    
    // HMAC-MD5
    std::string sign = HmacMd5(access_key_secret_, joint_params);
    ESP_LOGI(TAG, "Generated sign: %s", sign.c_str());
    return sign;
}

std::string JoyInsideAuth::HmacMd5(const std::string& key, const std::string& data) {
    unsigned char result[16];  // MD5 输出 16 字节
    
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    
    const mbedtls_md_info_t* md_info = mbedtls_md_info_from_type(MBEDTLS_MD_MD5);
    mbedtls_md_setup(&ctx, md_info, 1);  // 1 = HMAC
    mbedtls_md_hmac_starts(&ctx, (const unsigned char*)key.c_str(), key.length());
    mbedtls_md_hmac_update(&ctx, (const unsigned char*)data.c_str(), data.length());
    mbedtls_md_hmac_finish(&ctx, result);
    mbedtls_md_free(&ctx);
    
    // 转换为十六进制字符串
    char hex[33];
    for (int i = 0; i < 16; i++) {
        snprintf(hex + i * 2, 3, "%02x", result[i]);
    }
    hex[32] = '\0';
    
    return std::string(hex);
}

std::string JoyInsideAuth::GenerateUUID() {
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

int64_t JoyInsideAuth::GetCurrentTimeMs() const {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

// HTTP 响应缓冲区
static char http_response_buffer[4096];
static size_t http_response_len = 0;

static esp_err_t http_event_handler(esp_http_client_event_t* evt) {
    switch (evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGD("JoyInsideAuth", "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGD("JoyInsideAuth", "HTTP_EVENT_ON_CONNECTED");
            break;
        case HTTP_EVENT_ON_DATA:
            // 处理所有响应数据（包括 chunked 和非 chunked）
            if (evt->data_len > 0) {
                size_t copy_len = evt->data_len;
                if (http_response_len + copy_len >= sizeof(http_response_buffer)) {
                    copy_len = sizeof(http_response_buffer) - http_response_len - 1;
                    ESP_LOGW("JoyInsideAuth", "Response buffer full, truncating");
                }
                if (copy_len > 0) {
                    memcpy(http_response_buffer + http_response_len, evt->data, copy_len);
                    http_response_len += copy_len;
                    http_response_buffer[http_response_len] = '\0';
                }
            }
            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGD("JoyInsideAuth", "HTTP_EVENT_ON_FINISH, total len=%zu", http_response_len);
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGD("JoyInsideAuth", "HTTP_EVENT_DISCONNECTED");
            break;
        default:
            break;
    }
    return ESP_OK;
}

int JoyInsideAuth::HttpPost(const std::string& url, const std::string& json_body, std::string& response) {
    // 重置响应缓冲区
    http_response_len = 0;
    http_response_buffer[0] = '\0';
    
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_POST;
    config.event_handler = http_event_handler;
    config.timeout_ms = 10000;
    config.buffer_size = 2048;
    config.crt_bundle_attach = esp_crt_bundle_attach;  // 使用内置证书包进行 HTTPS 验证
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        return -1;
    }
    
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json_body.c_str(), json_body.length());
    
    ESP_LOGI(TAG, "Sending HTTP POST to %s", url.c_str());
    ESP_LOGD(TAG, "Request body: %s", json_body.c_str());
    
    esp_err_t err = esp_http_client_perform(client);
    int status = -1;
    
    if (err == ESP_OK) {
        status = esp_http_client_get_status_code(client);
        response = std::string(http_response_buffer, http_response_len);
        ESP_LOGI(TAG, "HTTP POST status=%d, response=%zu bytes", status, http_response_len);
        if (status != 200) {
            ESP_LOGW(TAG, "Response: %s", response.c_str());
        }
    } else {
        ESP_LOGE(TAG, "HTTP POST failed: %s (0x%x)", esp_err_to_name(err), err);
    }
    
    esp_http_client_cleanup(client);
    return status;
}

std::string JoyInsideAuth::RegisterDevice(const std::string& device_id,
                                           const std::string& device_type,
                                           const std::string& device_name) {
    /*
     * 设备注册 API
     * 参考: https://joyinside.jd.com/device/register
     * 
     * 请求参数:
     * - vendorId: 厂商唯一标识
     * - appId: 应用空间唯一标识
     * - deviceId: 设备唯一标识
     * - type: 设备类型 (PHYSICAL_ROBOT/APP_ROBOT)
     * - name: 设备名称
     * 
     * 响应:
     * - state: SUCCESS/FAILURE
     * - data: Bot ID
     */
    
    ESP_LOGI(TAG, "Registering device: deviceId=%s, type=%s, name=%s",
             device_id.c_str(), device_type.c_str(), device_name.c_str());
    
    // 获取 Access Token（设备注册需要厂商 Token）
    std::string token = GetAccessToken();
    if (token.empty()) {
        ESP_LOGE(TAG, "Failed to get access token for device registration");
        return "";
    }
    
    // 检查必要参数
    if (app_id_.empty()) {
        ESP_LOGE(TAG, "App ID not configured, cannot register device");
        return "";
    }
    
    // 构建注册请求
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "vendorId", vendor_id_);
    cJSON_AddStringToObject(root, "appId", app_id_.c_str());
    cJSON_AddStringToObject(root, "deviceId", device_id.c_str());
    cJSON_AddStringToObject(root, "type", device_type.c_str());
    cJSON_AddStringToObject(root, "name", device_name.c_str());
    
    char* json_str = cJSON_PrintUnformatted(root);
    std::string request_body = json_str;
    cJSON_free(json_str);
    cJSON_Delete(root);
    
    ESP_LOGI(TAG, "Device register request: %s", request_body.c_str());
    
    // 发送带认证头的 HTTP POST 请求
    std::string response;
    int status = HttpPostWithAuth(URL_DEVICE_REGISTER, request_body, token, response);
    
    if (status != 200) {
        ESP_LOGE(TAG, "Device registration failed, status=%d", status);
        if (!response.empty()) {
            ESP_LOGE(TAG, "Error response: %s", response.c_str());
        }
        return "";
    }
    
    ESP_LOGI(TAG, "Device register response: %s", response.c_str());
    
    // 解析响应
    cJSON* resp_root = cJSON_Parse(response.c_str());
    if (!resp_root) {
        ESP_LOGE(TAG, "Failed to parse device register response");
        return "";
    }
    
    // 检查状态
    cJSON* state = cJSON_GetObjectItem(resp_root, "state");
    if (!cJSON_IsString(state) || strcmp(state->valuestring, "SUCCESS") != 0) {
        cJSON* result = cJSON_GetObjectItem(resp_root, "result");
        cJSON* code = cJSON_GetObjectItem(resp_root, "code");
        ESP_LOGE(TAG, "Device registration failed: state=%s, code=%s, result=%s",
                 state ? state->valuestring : "null",
                 code ? code->valuestring : "null",
                 result ? result->valuestring : "null");
        cJSON_Delete(resp_root);
        return "";
    }
    
    // 获取 Bot ID
    cJSON* data = cJSON_GetObjectItem(resp_root, "data");
    if (!cJSON_IsString(data)) {
        ESP_LOGE(TAG, "Device registration response missing 'data' field");
        cJSON_Delete(resp_root);
        return "";
    }
    
    std::string bot_id = data->valuestring;
    ESP_LOGI(TAG, "Device registered successfully, botId=%s", bot_id.c_str());
    
    cJSON_Delete(resp_root);
    return bot_id;
}

int JoyInsideAuth::HttpPostWithAuth(const std::string& url, 
                                     const std::string& json_body,
                                     const std::string& token,
                                     std::string& response) {
    // 重置响应缓冲区
    http_response_len = 0;
    http_response_buffer[0] = '\0';
    
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_POST;
    config.event_handler = http_event_handler;
    config.timeout_ms = 10000;
    config.buffer_size = 2048;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        return -1;
    }
    
    // 设置认证头
    std::string auth_header = "Bearer " + token;
    esp_http_client_set_header(client, "Authorization", auth_header.c_str());
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json_body.c_str(), json_body.length());
    
    ESP_LOGI(TAG, "Sending authenticated HTTP POST to %s", url.c_str());
    
    esp_err_t err = esp_http_client_perform(client);
    int status = -1;
    
    if (err == ESP_OK) {
        status = esp_http_client_get_status_code(client);
        response = std::string(http_response_buffer, http_response_len);
        ESP_LOGI(TAG, "HTTP POST status=%d, response=%zu bytes", status, http_response_len);
        if (status != 200) {
            ESP_LOGW(TAG, "Response: %s", response.c_str());
        }
    } else {
        ESP_LOGE(TAG, "HTTP POST failed: %s (0x%x)", esp_err_to_name(err), err);
    }
    
    esp_http_client_cleanup(client);
    return status;
}
