#ifndef _ZHIBAN_AI_READER_DUAL_LED_H_
#define _ZHIBAN_AI_READER_DUAL_LED_H_

#include <driver/gpio.h>
#include <driver/ledc.h>
#include <esp_timer.h>

#include <mutex>

#include "led/led.h"

class ZhibanAiReaderDualLed : public Led {
public:
    ZhibanAiReaderDualLed(gpio_num_t blue_gpio, gpio_num_t red_gpio);
    ~ZhibanAiReaderDualLed() override;

    void OnStateChanged() override;
    void ApplyScene(LightScene scene) override;

private:
    struct CalibrationProfile {
        uint8_t blue_max_duty_percent = 100;
        uint8_t red_max_duty_percent = 100;
        uint8_t mixed_max_duty_percent = 100;
        uint16_t breathe_period_ms = 1400;
        uint16_t blink_fast_ms = 120;
        uint16_t blink_slow_ms = 500;
        uint16_t connect_pulse_ms = 140;
    } profile_;

    enum class AnimationMode {
        None,
        Blink,
        ConnectPulse,
        Ramp,
    };

    std::mutex mutex_;
    ledc_channel_config_t blue_channel_ = {};
    ledc_channel_config_t red_channel_ = {};
    bool ledc_initialized_ = false;
    esp_timer_handle_t animation_timer_ = nullptr;
    LightScene last_scene_ = LightScene::Booting;
    AnimationMode animation_mode_ = AnimationMode::None;
    bool output_enabled_ = false;
    bool ramp_increasing_ = true;
    int connect_step_ = 0;
    int ramp_step_ = 0;
    int ramp_total_steps_ = 1;
    int connect_short_gap_ms_ = 80;
    int connect_pause_ms_ = 420;
    uint32_t active_blue_duty_ = 0;
    uint32_t active_red_duty_ = 0;
    uint32_t ramp_min_blue_duty_ = 0;
    uint32_t ramp_min_red_duty_ = 0;
    uint32_t ramp_max_blue_duty_ = 0;
    uint32_t ramp_max_red_duty_ = 0;

    void InitLedc(gpio_num_t blue_gpio, gpio_num_t red_gpio);
    void StopAnimationLocked();
    void SetOutputLocked(uint32_t blue_duty, uint32_t red_duty);
    void SetColorLocked(uint32_t blue_duty, uint32_t red_duty);
    void StartBlinkLocked(uint32_t blue_duty, uint32_t red_duty, int interval_ms);
    void StartConnectPulseLocked();
    void AdvanceConnectPulseLocked();
    void StartRampLocked(uint32_t min_blue_duty, uint32_t min_red_duty,
                         uint32_t max_blue_duty, uint32_t max_red_duty,
                         int period_ms);
    void AdvanceRampLocked();
    uint32_t BlueDuty(uint8_t brightness_percent = 100, bool mixed = false) const;
    uint32_t RedDuty(uint8_t brightness_percent = 100, bool mixed = false) const;
    uint32_t DutyFromPercent(uint8_t limit_percent, uint8_t brightness_percent) const;
    static void OnAnimationTimer(void* arg);
    void HandleTimer();
};

#endif // _ZHIBAN_AI_READER_DUAL_LED_H_