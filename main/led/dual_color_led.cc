#include "dual_color_led.h"
#include "application.h"
#include "device_state.h"
#include <esp_log.h>

#define TAG "DualColorLed"

#define LEDC_TIMER          LEDC_TIMER_1
#define LEDC_MODE           LEDC_LOW_SPEED_MODE
#define LEDC_RED_CHANNEL    LEDC_CHANNEL_0
#define LEDC_BLUE_CHANNEL   LEDC_CHANNEL_1

#define LEDC_DUTY_MAX       (8191)  // 13-bit resolution
#define LEDC_FADE_TIME_UP   (800)   // 渐亮时间 0.8s
#define LEDC_FADE_TIME_DOWN (800)   // 渐灭时间 0.8s
#define LEDC_HOLD_TIME_MS   (200)   // 最亮时停留 0.2s

#define BLINK_INFINITE      (-1)

// 亮度定义
#define BRIGHTNESS_OFF      0
#define BRIGHTNESS_LOW      10
#define BRIGHTNESS_IDLE     15
#define BRIGHTNESS_DEFAULT  100   // 默认最亮
#define BRIGHTNESS_HIGH     100


DualColorLed::DualColorLed(gpio_num_t red_gpio, gpio_num_t blue_gpio) {
    assert(red_gpio != GPIO_NUM_NC);
    assert(blue_gpio != GPIO_NUM_NC);
    
    InitLedc(red_gpio, blue_gpio);
    
    esp_timer_create_args_t blink_timer_args = {
        .callback = [](void *arg) {
            auto led = static_cast<DualColorLed*>(arg);
            led->OnTimer();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "DualLedTimer",
        .skip_unhandled_events = false,
    };
    ESP_ERROR_CHECK(esp_timer_create(&blink_timer_args, &blink_timer_));
}

DualColorLed::~DualColorLed() {
    esp_timer_stop(blink_timer_);
    if (ledc_initialized_) {
        ledc_fade_stop(red_channel_.speed_mode, red_channel_.channel);
        ledc_fade_stop(blue_channel_.speed_mode, blue_channel_.channel);
        ledc_fade_func_uninstall();
    }
}

void DualColorLed::InitLedc(gpio_num_t red_gpio, gpio_num_t blue_gpio) {
    // 配置 LEDC 定时器
    ledc_timer_config_t ledc_timer = {};
    ledc_timer.duty_resolution = LEDC_TIMER_13_BIT;
    ledc_timer.freq_hz = 4000;
    ledc_timer.speed_mode = LEDC_MODE;
    ledc_timer.timer_num = LEDC_TIMER;
    ledc_timer.clk_cfg = LEDC_AUTO_CLK;
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    // 配置红色通道
    // 配置红色通道（共阳极：高电平灭，低电平亮，需反转输出）
    red_channel_.channel    = LEDC_RED_CHANNEL;
    red_channel_.duty       = 0;
    red_channel_.gpio_num   = red_gpio;
    red_channel_.speed_mode = LEDC_MODE;
    red_channel_.hpoint     = 0;
    red_channel_.timer_sel  = LEDC_TIMER;
    red_channel_.flags.output_invert = 1;  // LED 上拉到 3.3V，反转输出
    ledc_channel_config(&red_channel_);

    // 配置蓝色通道（共阳极：高电平灭，低电平亮，需反转输出）
    blue_channel_.channel    = LEDC_BLUE_CHANNEL;
    blue_channel_.duty       = 0;
    blue_channel_.gpio_num   = blue_gpio;
    blue_channel_.speed_mode = LEDC_MODE;
    blue_channel_.hpoint     = 0;
    blue_channel_.timer_sel  = LEDC_TIMER;
    blue_channel_.flags.output_invert = 1;  // LED 上拉到 3.3V，反转输出
    ledc_channel_config(&blue_channel_);

    // 安装渐变服务
    ledc_fade_func_install(0);

    // 注册渐变回调（红色和蓝色通道都需要）
    ledc_cbs_t ledc_callbacks = {
        .fade_cb = FadeCallback
    };
    ledc_cb_register(red_channel_.speed_mode, red_channel_.channel, &ledc_callbacks, this);
    ledc_cb_register(blue_channel_.speed_mode, blue_channel_.channel, &ledc_callbacks, this);

    ledc_initialized_ = true;
}

void DualColorLed::SetRed(uint8_t brightness) {
    red_duty_ = (brightness == 100) ? LEDC_DUTY_MAX : (brightness * LEDC_DUTY_MAX / 100);
    blue_duty_ = 0;
}

void DualColorLed::SetBlue(uint8_t brightness) {
    red_duty_ = 0;
    blue_duty_ = (brightness == 100) ? LEDC_DUTY_MAX : (brightness * LEDC_DUTY_MAX / 100);
}

void DualColorLed::SetPurple(uint8_t red_brightness, uint8_t blue_brightness) {
    red_duty_ = (red_brightness == 100) ? LEDC_DUTY_MAX : (red_brightness * LEDC_DUTY_MAX / 100);
    blue_duty_ = (blue_brightness == 100) ? LEDC_DUTY_MAX : (blue_brightness * LEDC_DUTY_MAX / 100);
}

void DualColorLed::TurnOn() {
    if (!ledc_initialized_) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    esp_timer_stop(blink_timer_);
    ledc_fade_stop(red_channel_.speed_mode, red_channel_.channel);
    ledc_fade_stop(blue_channel_.speed_mode, blue_channel_.channel);
    
    ledc_set_duty(red_channel_.speed_mode, red_channel_.channel, red_duty_);
    ledc_update_duty(red_channel_.speed_mode, red_channel_.channel);
    
    ledc_set_duty(blue_channel_.speed_mode, blue_channel_.channel, blue_duty_);
    ledc_update_duty(blue_channel_.speed_mode, blue_channel_.channel);
}

void DualColorLed::TurnOff() {
    if (!ledc_initialized_) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    esp_timer_stop(blink_timer_);
    ledc_fade_stop(red_channel_.speed_mode, red_channel_.channel);
    ledc_fade_stop(blue_channel_.speed_mode, blue_channel_.channel);
    
    ledc_set_duty(red_channel_.speed_mode, red_channel_.channel, 0);
    ledc_update_duty(red_channel_.speed_mode, red_channel_.channel);
    
    ledc_set_duty(blue_channel_.speed_mode, blue_channel_.channel, 0);
    ledc_update_duty(blue_channel_.speed_mode, blue_channel_.channel);
}

void DualColorLed::TurnOffRed() {
    if (!ledc_initialized_) {
        return;
    }
    ledc_set_duty(red_channel_.speed_mode, red_channel_.channel, 0);
    ledc_update_duty(red_channel_.speed_mode, red_channel_.channel);
}

void DualColorLed::TurnOffBlue() {
    if (!ledc_initialized_) {
        return;
    }
    ledc_set_duty(blue_channel_.speed_mode, blue_channel_.channel, 0);
    ledc_update_duty(blue_channel_.speed_mode, blue_channel_.channel);
}

void DualColorLed::StartContinuousBlink(int interval_ms) {
    StartBlinkTask(BLINK_INFINITE, interval_ms);
}

void DualColorLed::StartBlinkTask(int times, int interval_ms) {
    if (!ledc_initialized_) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    esp_timer_stop(blink_timer_);
    ledc_fade_stop(red_channel_.speed_mode, red_channel_.channel);
    ledc_fade_stop(blue_channel_.speed_mode, blue_channel_.channel);
    use_fade_ = false;

    // 立即关闭不需要的颜色
    if (red_duty_ == 0) {
        ledc_set_duty(red_channel_.speed_mode, red_channel_.channel, 0);
        ledc_update_duty(red_channel_.speed_mode, red_channel_.channel);
    }
    if (blue_duty_ == 0) {
        ledc_set_duty(blue_channel_.speed_mode, blue_channel_.channel, 0);
        ledc_update_duty(blue_channel_.speed_mode, blue_channel_.channel);
    }

    blink_counter_ = (times == BLINK_INFINITE) ? BLINK_INFINITE : times * 2;
    blink_interval_ms_ = interval_ms;
    blink_state_ = false;  // 从灭开始，第一次定时器触发时变亮
    
    // 立即点亮
    blink_state_ = true;
    ledc_set_duty(red_channel_.speed_mode, red_channel_.channel, red_duty_);
    ledc_update_duty(red_channel_.speed_mode, red_channel_.channel);
    ledc_set_duty(blue_channel_.speed_mode, blue_channel_.channel, blue_duty_);
    ledc_update_duty(blue_channel_.speed_mode, blue_channel_.channel);
    
    esp_timer_start_periodic(blink_timer_, interval_ms * 1000);
}

void DualColorLed::OnTimer() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (use_fade_) {
        // 呼吸模式：停留结束后开始渐灭
        OnHoldTimerUnlocked();
    } else {
        // 闪烁模式
        OnBlinkTimerUnlocked();
    }
}

void DualColorLed::OnBlinkTimerUnlocked() {
    blink_state_ = !blink_state_;
    
    if (blink_state_) {
        // 亮
        ledc_set_duty(red_channel_.speed_mode, red_channel_.channel, red_duty_);
        ledc_update_duty(red_channel_.speed_mode, red_channel_.channel);
        ledc_set_duty(blue_channel_.speed_mode, blue_channel_.channel, blue_duty_);
        ledc_update_duty(blue_channel_.speed_mode, blue_channel_.channel);
    } else {
        // 灭
        ledc_set_duty(red_channel_.speed_mode, red_channel_.channel, 0);
        ledc_update_duty(red_channel_.speed_mode, red_channel_.channel);
        ledc_set_duty(blue_channel_.speed_mode, blue_channel_.channel, 0);
        ledc_update_duty(blue_channel_.speed_mode, blue_channel_.channel);

        // 非无限模式时检查是否结束
        if (blink_counter_ != BLINK_INFINITE) {
            blink_counter_--;
            if (blink_counter_ <= 0) {
                esp_timer_stop(blink_timer_);
            }
        }
    }
}

void DualColorLed::OnHoldTimerUnlocked() {
    // 停留结束，开始渐灭
    fade_up_ = false;
    int fade_time = LEDC_FADE_TIME_DOWN;
    
    if (red_duty_ > 0) {
        ledc_set_fade_with_time(red_channel_.speed_mode,
                                red_channel_.channel, 0, fade_time);
        ledc_fade_start(red_channel_.speed_mode,
                        red_channel_.channel, LEDC_FADE_NO_WAIT);
    }
    if (blue_duty_ > 0) {
        ledc_set_fade_with_time(blue_channel_.speed_mode,
                                blue_channel_.channel, 0, fade_time);
        ledc_fade_start(blue_channel_.speed_mode,
                        blue_channel_.channel, LEDC_FADE_NO_WAIT);
    }
}

void DualColorLed::StartFadeTask() {
    if (!ledc_initialized_) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    esp_timer_stop(blink_timer_);
    ledc_fade_stop(red_channel_.speed_mode, red_channel_.channel);
    ledc_fade_stop(blue_channel_.speed_mode, blue_channel_.channel);
    
    // 立即关闭不需要的颜色
    if (red_duty_ == 0) {
        ledc_set_duty(red_channel_.speed_mode, red_channel_.channel, 0);
        ledc_update_duty(red_channel_.speed_mode, red_channel_.channel);
    }
    if (blue_duty_ == 0) {
        ledc_set_duty(blue_channel_.speed_mode, blue_channel_.channel, 0);
        ledc_update_duty(blue_channel_.speed_mode, blue_channel_.channel);
    }
    
    use_fade_ = true;
    StartFadeUnlocked();
}

void DualColorLed::OnFadeEnd() {
    if (!use_fade_) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 如果刚渐亮完成，等待一段时间再开始渐灭
    if (fade_up_) {
        // 使用定时器延迟触发渐灭
        esp_timer_stop(blink_timer_);
        esp_timer_start_once(blink_timer_, LEDC_HOLD_TIME_MS * 1000);
        return;
    }
    
    // 开始下一次渐亮
    StartFadeUnlocked();
}

void DualColorLed::StartFadeUnlocked() {
    fade_up_ = true;
    int fade_time = LEDC_FADE_TIME_UP;
    
    if (red_duty_ > 0) {
        ledc_set_fade_with_time(red_channel_.speed_mode,
                                red_channel_.channel, red_duty_, fade_time);
        ledc_fade_start(red_channel_.speed_mode,
                        red_channel_.channel, LEDC_FADE_NO_WAIT);
    }
    if (blue_duty_ > 0) {
        ledc_set_fade_with_time(blue_channel_.speed_mode,
                                blue_channel_.channel, blue_duty_, fade_time);
        ledc_fade_start(blue_channel_.speed_mode,
                        blue_channel_.channel, LEDC_FADE_NO_WAIT);
    }
}

IRAM_ATTR bool DualColorLed::FadeCallback(const ledc_cb_param_t *param, void *user_arg) {
    if (param->event == LEDC_FADE_END_EVT) {
        auto led = static_cast<DualColorLed*>(user_arg);
        led->OnFadeEnd();
    }
    return true;
}

void DualColorLed::OnStateChanged() {
    auto& app = Application::GetInstance();
    auto device_state = app.GetDeviceState();
    
    switch (device_state) {
        case kDeviceStateStarting:
        case kDeviceStateConnecting:
            // 启动中/连接中：蓝色快闪（稍等）
            SetBlue(BRIGHTNESS_DEFAULT);
            StartContinuousBlink(100);
            break;
            
        case kDeviceStateWifiConfiguring:
            // 配网模式：蓝色慢闪（需要配网）
            SetBlue(BRIGHTNESS_DEFAULT);
            StartContinuousBlink(500);
            break;
            
        case kDeviceStateIdle:
            // 待机：蓝色呼吸 ✅ 可以唤醒
            SetBlue(BRIGHTNESS_DEFAULT);
            StartFadeTask();
            break;
            
        case kDeviceStateListening:
        case kDeviceStateAudioTesting:
            // 对话中：蓝色常亮 ✅ 请说话
            SetBlue(BRIGHTNESS_DEFAULT);
            TurnOn();
            break;
            
        case kDeviceStateSpeaking:
            // AI回答：红色常亮
            SetRed(BRIGHTNESS_DEFAULT);
            TurnOn();
            break;
            
        case kDeviceStateUpgrading:
            // 升级中：紫色快闪
            SetPurple(BRIGHTNESS_DEFAULT, BRIGHTNESS_DEFAULT);
            StartContinuousBlink(100);
            break;
            
        case kDeviceStateActivating:
            // 激活中：紫色慢闪
            SetPurple(BRIGHTNESS_DEFAULT, BRIGHTNESS_DEFAULT);
            StartContinuousBlink(500);
            break;

        case kDeviceStateFatalError:
            // 网络异常：红色快闪 ❌
            SetRed(BRIGHTNESS_DEFAULT);
            StartContinuousBlink(200);
            break;
            
        default:
            ESP_LOGW(TAG, "Unknown device state: %d", device_state);
            return;
    }
}
