#include "wifi_board.h"
#include "codecs/box_audio_codec.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "led/dual_color_led.h"
#include "assets/lang_config.h"

#include <esp_log.h>
#include <driver/i2c_master.h>
#include <wifi_station.h>

#define TAG "LandoubaoDevBoard"

class LandoubaoDevBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    Button boot_button_;

    void InitializeI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)1,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void InitializeButtons() {
        // 短按：音量 +10%（到顶后循环回 10%）
        boot_button_.OnClick([this]() {
            auto codec = GetAudioCodec();
            if (codec) {
                int volume = codec->output_volume() + 10;
                if (volume > 100) {
                    volume = 10;  // 循环回最小
                }
                codec->SetOutputVolume(volume);
                ESP_LOGI(TAG, "Volume: %d%%", volume);
            }
        });

        // 双击：音量 -10%（最低到 0%）
        boot_button_.OnDoubleClick([this]() {
            auto codec = GetAudioCodec();
            if (codec) {
                int volume = codec->output_volume() - 10;
                if (volume < 0) {
                    volume = 0;  // 最低静音
                }
                codec->SetOutputVolume(volume);
                ESP_LOGI(TAG, "Volume: %d%%", volume);
            }
        });

        // 长按：进入配网模式
        boot_button_.OnLongPress([this]() {
            ResetWifiConfiguration();
        });
    }

public:
    LandoubaoDevBoard() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializeI2c();
        InitializeButtons();
    }

    virtual Led* GetLed() override {
        static DualColorLed led(LED_RED_GPIO, LED_BLUE_GPIO);
        return &led;
    }

    virtual AudioCodec* GetAudioCodec() override {
        static BoxAudioCodec audio_codec(
            i2c_bus_, 
            AUDIO_INPUT_SAMPLE_RATE, 
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, 
            AUDIO_I2S_GPIO_BCLK, 
            AUDIO_I2S_GPIO_WS, 
            AUDIO_I2S_GPIO_DOUT, 
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, 
            AUDIO_CODEC_ES8311_ADDR, 
            AUDIO_CODEC_ES7210_ADDR, 
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }
};

DECLARE_BOARD(LandoubaoDevBoard);
