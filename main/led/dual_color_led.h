#ifndef _DUAL_COLOR_LED_H_
#define _DUAL_COLOR_LED_H_

#include "led.h"
#include <driver/gpio.h>
#include <driver/ledc.h>
#include <esp_timer.h>
#include <mutex>

/**
 * @brief 双色 LED 驱动类
 * 
 * 支持红蓝双色 LED，分别由两个 GPIO 控制。
 * 可以显示：红色、蓝色、紫色（红+蓝同时亮）
 */
class DualColorLed : public Led {
public:
    /**
     * @brief 构造双色 LED
     * @param red_gpio 红色 LED 的 GPIO 引脚
     * @param blue_gpio 蓝色 LED 的 GPIO 引脚
     */
    DualColorLed(gpio_num_t red_gpio, gpio_num_t blue_gpio);
    virtual ~DualColorLed();

    void OnStateChanged() override;

private:
    std::mutex mutex_;
    ledc_channel_config_t red_channel_ = {};
    ledc_channel_config_t blue_channel_ = {};
    bool ledc_initialized_ = false;
    
    uint32_t red_duty_ = 0;
    uint32_t blue_duty_ = 0;
    
    int blink_counter_ = 0;
    int blink_interval_ms_ = 0;
    bool blink_state_ = false;
    esp_timer_handle_t blink_timer_ = nullptr;
    
    bool fade_up_ = true;
    bool use_fade_ = false;

    void InitLedc(gpio_num_t red_gpio, gpio_num_t blue_gpio);
    
    void SetRed(uint8_t brightness);
    void SetBlue(uint8_t brightness);
    void SetPurple(uint8_t red_brightness, uint8_t blue_brightness);
    
    void TurnOn();
    void TurnOff();
    void TurnOffRed();
    void TurnOffBlue();
    
    void StartBlinkTask(int times, int interval_ms);
    void StartContinuousBlink(int interval_ms);
    void OnTimer();
    void OnBlinkTimerUnlocked();
    void OnHoldTimerUnlocked();
    
    void StartFadeTask();
    void StartFadeUnlocked();
    void OnFadeEnd();
    static bool FadeCallback(const ledc_cb_param_t *param, void *user_arg);
};

#endif // _DUAL_COLOR_LED_H_
