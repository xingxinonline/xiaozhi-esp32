#ifndef _JOYINSIDE_AUTH_H_
#define _JOYINSIDE_AUTH_H_

#include <string>
#include <functional>

/**
 * @brief 初始化 SNTP 时间同步（应在 WiFi 连接后立即调用）
 * 
 * 这是一个非阻塞调用，会在后台进行时间同步
 */
void JoyInsideInitSntp();

/**
 * @brief 检查时间是否已同步
 * @return true 如果时间已同步
 */
bool JoyInsideIsTimeSynced();

/**
 * @brief JoyInside 认证模块
 * 
 * 实现 AK/SK 签名认证和 Token 管理
 * 参考：https://joyinside.jdcloud.com/docs/#/zh-cn/authToken/getToken
 */
class JoyInsideAuth {
public:
    JoyInsideAuth();
    ~JoyInsideAuth() = default;

    /**
     * @brief 配置 AK/SK 认证参数
     * @param access_key AccessKey ID
     * @param access_key_secret AccessKey Secret
     * @param vendor_id 厂商 ID (默认 100090)
     * @param bot_id Bot ID
     */
    void Configure(const std::string& access_key, 
                   const std::string& access_key_secret,
                   int vendor_id,
                   const std::string& bot_id);

    /**
     * @brief 获取 Access Token
     * 如果本地有有效 Token 则直接返回，否则调用 API 获取
     * @return Access Token，失败返回空字符串
     */
    std::string GetAccessToken();

    /**
     * @brief 刷新 Token
     * @return 刷新后的 Access Token，失败返回空字符串
     */
    std::string RefreshToken();

    /**
     * @brief 检查 Token 是否有效
     */
    bool IsTokenValid() const;

    /**
     * @brief 使用静态 Token（手动模式）
     */
    void SetStaticToken(const std::string& token);

private:
    // AK/SK 配置
    std::string access_key_;
    std::string access_key_secret_;
    int vendor_id_;
    std::string bot_id_;
    
    // Token 管理
    std::string access_token_;
    std::string refresh_token_;
    int64_t token_expire_time_;  // Token 过期时间 (毫秒时间戳)
    
    // 是否使用静态 Token
    bool use_static_token_;
    
    /**
     * @brief 生成签名
     * @param access_version 版本 (V2)
     * @param timestamp 时间戳 (毫秒)
     * @param nonce 随机数
     * @return HMAC-MD5 签名的十六进制字符串
     */
    std::string GenerateSign(const std::string& access_version,
                              const std::string& timestamp,
                              const std::string& nonce);
    
    /**
     * @brief 调用 getToken API
     * @return 是否成功
     */
    bool FetchToken();
    
    /**
     * @brief 生成 UUID
     */
    std::string GenerateUUID();
    
    /**
     * @brief 获取当前时间戳 (毫秒)
     */
    int64_t GetCurrentTimeMs() const;
    
    /**
     * @brief HMAC-MD5 计算
     * @param key 密钥
     * @param data 数据
     * @return 十六进制字符串
     */
    std::string HmacMd5(const std::string& key, const std::string& data);
    
    /**
     * @brief HTTP POST 请求
     * @param url 请求 URL
     * @param json_body JSON 请求体
     * @param response 响应内容
     * @return HTTP 状态码，失败返回 -1
     */
    int HttpPost(const std::string& url, const std::string& json_body, std::string& response);
};

#endif // _JOYINSIDE_AUTH_H_
