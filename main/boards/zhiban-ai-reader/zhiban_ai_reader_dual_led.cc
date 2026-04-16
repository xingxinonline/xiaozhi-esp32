#include "zhiban_ai_reader_dual_led.h"

#include <algorithm>
#include <cassert>

#include <esp_log.h>

namespace {

constexpr char kTag[] = "ZhibanDualLed";
constexpr int kLedcTimer = LEDC_TIMER_1;
constexpr int kLedcMode = LEDC_LOW_SPEED_MODE;
constexpr int kBlueChannel = LEDC_CHANNEL_0;
constexpr int kRedChannel = LEDC_CHANNEL_1;
constexpr uint32_t kLedcDutyMax = 8191;
constexpr int kRampTickMs = 20;
constexpr uint8_t kIdleBluePercent = 45;
constexpr uint8_t kActivePulseMinPercent = 35;

} // namespace

ZhibanAiReaderDualLed::ZhibanAiReaderDualLed(gpio_num_t blue_gpio, gpio_num_t red_gpio) {
    assert(blue_gpio != GPIO_NUM_NC);
    assert(red_gpio != GPIO_NUM_NC);

    InitLedc(blue_gpio, red_gpio);

    esp_timer_create_args_t timer_args = {
        .callback = &ZhibanAiReaderDualLed::OnAnimationTimer,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "zhiban_led",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &animation_timer_));
}

ZhibanAiReaderDualLed::~ZhibanAiReaderDualLed() {
    if (animation_timer_ != nullptr) {
        esp_timer_stop(animation_timer_);
        esp_timer_delete(animation_timer_);
    }
}

void ZhibanAiReaderDualLed::OnStateChanged() {
    ApplyScene(last_scene_);
}

void ZhibanAiReaderDualLed::ApplyScene(LightScene scene) {
    std::lock_guard<std::mutex> lock(mutex_);
    last_scene_ = scene;
    StopAnimationLocked();

    switch (scene) {
        case LightScene::Booting:
            StartBlinkLocked(BlueDuty(), 0, profile_.blink_fast_ms);
            break;
        case LightScene::WifiConfiguring:
            StartBlinkLocked(BlueDuty(), 0, profile_.blink_slow_ms);
            break;
        case LightScene::Activating:
            StartBlinkLocked(BlueDuty(100, true), RedDuty(100, true), profile_.blink_slow_ms);
            break;
        case LightScene::IdleReady:
            StartRampLocked(0, 0, BlueDuty(kIdleBluePercent), 0, profile_.breathe_period_ms);
            break;
        case LightScene::Connecting:
            active_blue_duty_ = BlueDuty();
            active_red_duty_ = 0;
            StartConnectPulseLocked();
            break;
        case LightScene::ListeningPassive:
            SetColorLocked(BlueDuty(), 0);
            break;
        case LightScene::ListeningActive:
            StartRampLocked(BlueDuty(kActivePulseMinPercent), 0, BlueDuty(), 0, 420);
            break;
        case LightScene::Speaking:
            SetColorLocked(0, RedDuty());
            break;
        case LightScene::Upgrading:
            StartBlinkLocked(BlueDuty(100, true), RedDuty(100, true), profile_.blink_fast_ms);
            break;
        case LightScene::RecoveringOrError:
            StartBlinkLocked(0, RedDuty(), profile_.blink_fast_ms);
            break;
    }
}

void ZhibanAiReaderDualLed::InitLedc(gpio_num_t blue_gpio, gpio_num_t red_gpio) {
    ledc_timer_config_t ledc_timer = {};
    ledc_timer.duty_resolution = LEDC_TIMER_13_BIT;
    ledc_timer.freq_hz = 4000;
    ledc_timer.speed_mode = static_cast<ledc_mode_t>(kLedcMode);
    ledc_timer.timer_num = static_cast<ledc_timer_t>(kLedcTimer);
    ledc_timer.clk_cfg = LEDC_AUTO_CLK;
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    blue_channel_.channel = static_cast<ledc_channel_t>(kBlueChannel);
    blue_channel_.duty = 0;
    blue_channel_.gpio_num = blue_gpio;
    blue_channel_.speed_mode = static_cast<ledc_mode_t>(kLedcMode);
    blue_channel_.hpoint = 0;
    blue_channel_.timer_sel = static_cast<ledc_timer_t>(kLedcTimer);
    blue_channel_.flags.output_invert = 1;
    ESP_ERROR_CHECK(ledc_channel_config(&blue_channel_));

    red_channel_.channel = static_cast<ledc_channel_t>(kRedChannel);
    red_channel_.duty = 0;
    red_channel_.gpio_num = red_gpio;
    red_channel_.speed_mode = static_cast<ledc_mode_t>(kLedcMode);
    red_channel_.hpoint = 0;
    red_channel_.timer_sel = static_cast<ledc_timer_t>(kLedcTimer);
    red_channel_.flags.output_invert = 1;
    ESP_ERROR_CHECK(ledc_channel_config(&red_channel_));

    ledc_initialized_ = true;
}

void ZhibanAiReaderDualLed::StopAnimationLocked() {
    if (animation_timer_ != nullptr) {
        esp_timer_stop(animation_timer_);
    }
    animation_mode_ = AnimationMode::None;
    output_enabled_ = false;
    ramp_increasing_ = true;
    connect_step_ = 0;
    ramp_step_ = 0;
}

void ZhibanAiReaderDualLed::SetOutputLocked(uint32_t blue_duty, uint32_t red_duty) {
    if (!ledc_initialized_) {
        return;
    }

    ledc_set_duty(blue_channel_.speed_mode, blue_channel_.channel, blue_duty);
    ledc_update_duty(blue_channel_.speed_mode, blue_channel_.channel);
    ledc_set_duty(red_channel_.speed_mode, red_channel_.channel, red_duty);
    ledc_update_duty(red_channel_.speed_mode, red_channel_.channel);
}

void ZhibanAiReaderDualLed::SetColorLocked(uint32_t blue_duty, uint32_t red_duty) {
    active_blue_duty_ = blue_duty;
    active_red_duty_ = red_duty;
    output_enabled_ = true;
    SetOutputLocked(blue_duty, red_duty);
}

void ZhibanAiReaderDualLed::StartBlinkLocked(uint32_t blue_duty, uint32_t red_duty, int interval_ms) {
    active_blue_duty_ = blue_duty;
    active_red_duty_ = red_duty;
    output_enabled_ = true;
    animation_mode_ = AnimationMode::Blink;
    SetOutputLocked(active_blue_duty_, active_red_duty_);
    ESP_ERROR_CHECK(esp_timer_start_periodic(animation_timer_, interval_ms * 1000ULL));
}

void ZhibanAiReaderDualLed::StartConnectPulseLocked() {
    animation_mode_ = AnimationMode::ConnectPulse;
    connect_short_gap_ms_ = std::max(60, profile_.connect_pulse_ms / 2);
    connect_pause_ms_ = std::max(240, profile_.connect_pulse_ms * 3);
    connect_step_ = 0;
    AdvanceConnectPulseLocked();
}

void ZhibanAiReaderDualLed::AdvanceConnectPulseLocked() {
    int next_delay_ms = profile_.connect_pulse_ms;

    switch (connect_step_) {
        case 0:
            SetOutputLocked(active_blue_duty_, active_red_duty_);
            next_delay_ms = profile_.connect_pulse_ms;
            break;
        case 1:
            SetOutputLocked(0, 0);
            next_delay_ms = connect_short_gap_ms_;
            break;
        case 2:
            SetOutputLocked(active_blue_duty_, active_red_duty_);
            next_delay_ms = profile_.connect_pulse_ms;
            break;
        default:
            SetOutputLocked(0, 0);
            next_delay_ms = connect_pause_ms_;
            break;
    }

    connect_step_ = (connect_step_ + 1) % 4;
    ESP_ERROR_CHECK(esp_timer_start_once(animation_timer_, next_delay_ms * 1000ULL));
}

void ZhibanAiReaderDualLed::StartRampLocked(uint32_t min_blue_duty, uint32_t min_red_duty,
                                            uint32_t max_blue_duty, uint32_t max_red_duty,
                                            int period_ms) {
    animation_mode_ = AnimationMode::Ramp;
    ramp_min_blue_duty_ = min_blue_duty;
    ramp_min_red_duty_ = min_red_duty;
    ramp_max_blue_duty_ = max_blue_duty;
    ramp_max_red_duty_ = max_red_duty;
    ramp_total_steps_ = std::max(1, period_ms / (2 * kRampTickMs));
    ramp_step_ = 0;
    ramp_increasing_ = true;
    SetOutputLocked(ramp_min_blue_duty_, ramp_min_red_duty_);
    ESP_ERROR_CHECK(esp_timer_start_periodic(animation_timer_, kRampTickMs * 1000ULL));
}

void ZhibanAiReaderDualLed::AdvanceRampLocked() {
    int effective_step = ramp_increasing_ ? ramp_step_ : (ramp_total_steps_ - ramp_step_);

    uint32_t blue_duty = ramp_min_blue_duty_;
    uint32_t red_duty = ramp_min_red_duty_;
    if (ramp_total_steps_ > 0) {
        blue_duty += ((ramp_max_blue_duty_ - ramp_min_blue_duty_) * effective_step) / ramp_total_steps_;
        red_duty += ((ramp_max_red_duty_ - ramp_min_red_duty_) * effective_step) / ramp_total_steps_;
    }
    SetOutputLocked(blue_duty, red_duty);

    ramp_step_++;
    if (ramp_step_ > ramp_total_steps_) {
        ramp_step_ = 0;
        ramp_increasing_ = !ramp_increasing_;
    }
}

uint32_t ZhibanAiReaderDualLed::BlueDuty(uint8_t brightness_percent, bool mixed) const {
    return DutyFromPercent(mixed ? profile_.mixed_max_duty_percent : profile_.blue_max_duty_percent,
                           brightness_percent);
}

uint32_t ZhibanAiReaderDualLed::RedDuty(uint8_t brightness_percent, bool mixed) const {
    return DutyFromPercent(mixed ? profile_.mixed_max_duty_percent : profile_.red_max_duty_percent,
                           brightness_percent);
}

uint32_t ZhibanAiReaderDualLed::DutyFromPercent(uint8_t limit_percent, uint8_t brightness_percent) const {
    return (static_cast<uint32_t>(limit_percent) * brightness_percent * kLedcDutyMax) / 10000U;
}

void ZhibanAiReaderDualLed::OnAnimationTimer(void* arg) {
    auto* led = static_cast<ZhibanAiReaderDualLed*>(arg);
    led->HandleTimer();
}

void ZhibanAiReaderDualLed::HandleTimer() {
    std::lock_guard<std::mutex> lock(mutex_);

    switch (animation_mode_) {
        case AnimationMode::Blink:
            output_enabled_ = !output_enabled_;
            if (output_enabled_) {
                SetOutputLocked(active_blue_duty_, active_red_duty_);
            } else {
                SetOutputLocked(0, 0);
            }
            break;
        case AnimationMode::ConnectPulse:
            AdvanceConnectPulseLocked();
            break;
        case AnimationMode::Ramp:
            AdvanceRampLocked();
            break;
        case AnimationMode::None:
            break;
    }
}