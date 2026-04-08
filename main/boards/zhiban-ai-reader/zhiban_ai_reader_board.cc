#include "wifi_board.h"
#include "application.h"
#include "button.h"
#include "codecs/box_audio_codec.h"
#include "config.h"
#include "mcp_server.h"
#include "led/gpio_led.h"

#include <driver/i2c_master.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_log.h>

#define TAG "ZhibanAiReaderBoard"

class ZhibanAiReaderBatteryMonitor {
private:
    adc_oneshot_unit_handle_t battery_adc_handle_ = nullptr;
    adc_oneshot_unit_handle_t charge_adc_handle_ = nullptr;
    adc_cali_handle_t battery_adc_cali_handle_ = nullptr;
    adc_cali_handle_t charge_adc_cali_handle_ = nullptr;
    bool battery_adc_calibrated_ = false;
    bool charge_adc_calibrated_ = false;

    bool InitializeCalibration(adc_unit_t unit, adc_channel_t channel, adc_cali_handle_t* out_handle) {
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
        adc_cali_curve_fitting_config_t cali_config = {
            .unit_id = unit,
            .chan = channel,
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        return adc_cali_create_scheme_curve_fitting(&cali_config, out_handle) == ESP_OK;
#else
        (void)unit;
        (void)channel;
        (void)out_handle;
        return false;
#endif
    }

    void DeinitializeCalibration(adc_cali_handle_t cali_handle, bool calibrated) {
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
        if (calibrated && cali_handle != nullptr) {
            ESP_ERROR_CHECK(adc_cali_delete_scheme_curve_fitting(cali_handle));
        }
#else
        (void)cali_handle;
        (void)calibrated;
#endif
    }

    void InitializeChannel(adc_unit_t unit,
                           adc_channel_t channel,
                           adc_oneshot_unit_handle_t* adc_handle,
                           adc_cali_handle_t* cali_handle,
                           bool* calibrated) {
        adc_oneshot_unit_init_cfg_t init_config = {
            .unit_id = unit,
            .ulp_mode = ADC_ULP_MODE_DISABLE,
        };
        ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, adc_handle));

        adc_oneshot_chan_cfg_t channel_config = {
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        ESP_ERROR_CHECK(adc_oneshot_config_channel(*adc_handle, channel, &channel_config));
        *calibrated = InitializeCalibration(unit, channel, cali_handle);
    }

    int ReadVoltageMv(adc_oneshot_unit_handle_t adc_handle,
                      adc_channel_t channel,
                      adc_cali_handle_t cali_handle,
                      bool calibrated) const {
        int raw = 0;
        int voltage_mv = 0;
        ESP_ERROR_CHECK(adc_oneshot_read(adc_handle, channel, &raw));

        if (calibrated) {
            ESP_ERROR_CHECK(adc_cali_raw_to_voltage(cali_handle, raw, &voltage_mv));
        } else {
            voltage_mv = raw * 3300 / 4095;
        }

        return voltage_mv;
    }

public:
    ZhibanAiReaderBatteryMonitor() {
        InitializeChannel(BATTERY_ADC_UNIT,
                          BATTERY_ADC_CHANNEL,
                          &battery_adc_handle_,
                          &battery_adc_cali_handle_,
                          &battery_adc_calibrated_);
        InitializeChannel(CHARGE_ADC_UNIT,
                          CHARGE_ADC_CHANNEL,
                          &charge_adc_handle_,
                          &charge_adc_cali_handle_,
                          &charge_adc_calibrated_);
    }

    ~ZhibanAiReaderBatteryMonitor() {
        DeinitializeCalibration(battery_adc_cali_handle_, battery_adc_calibrated_);
        DeinitializeCalibration(charge_adc_cali_handle_, charge_adc_calibrated_);

        if (battery_adc_handle_ != nullptr) {
            ESP_ERROR_CHECK(adc_oneshot_del_unit(battery_adc_handle_));
        }
        if (charge_adc_handle_ != nullptr) {
            ESP_ERROR_CHECK(adc_oneshot_del_unit(charge_adc_handle_));
        }
    }

    int GetBatteryLevel() const {
        float battery_voltage = static_cast<float>(ReadVoltageMv(battery_adc_handle_,
                                                                 BATTERY_ADC_CHANNEL,
                                                                 battery_adc_cali_handle_,
                                                                 battery_adc_calibrated_)) /
                                1000.0f;
        float normalized_voltage = battery_voltage - 1.5f;
        if (normalized_voltage < 0.0f) {
            normalized_voltage = 0.0f;
        }

        int level = static_cast<int>(normalized_voltage * 200.0f);
        if (level > 100) {
            level = 100;
        }
        return level;
    }

    bool IsCharging() const {
        int charge_voltage_mv = ReadVoltageMv(charge_adc_handle_,
                                              CHARGE_ADC_CHANNEL,
                                              charge_adc_cali_handle_,
                                              charge_adc_calibrated_);
        return charge_voltage_mv < 1000;
    }
};

class ZhibanAiReaderBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    Button boot_button_;
    ZhibanAiReaderBatteryMonitor* battery_monitor_ = nullptr;

    void InitializeI2c() {
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_1,
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
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });

#if CONFIG_USE_DEVICE_AEC
        boot_button_.OnDoubleClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateIdle) {
                app.SetAecMode(app.GetAecMode() == kAecOff ? kAecOnDeviceSide : kAecOff);
            }
        });
#endif
    }

    void InitializePowerMonitor() {
        battery_monitor_ = new ZhibanAiReaderBatteryMonitor();
    }

    void InitializeTools() {
        auto& mcp_server = McpServer::GetInstance();
        mcp_server.AddTool("self.system.reconfigure_wifi",
            "End this conversation and enter WiFi configuration mode.\n"
            "**CAUTION** You must ask the user to confirm this action.",
            PropertyList(), [this](const PropertyList& properties) {
                EnterWifiConfigMode();
                return true;
            });
    }

public:
    ZhibanAiReaderBoard() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializeI2c();
        InitializeButtons();
        InitializePowerMonitor();
        InitializeTools();
    }

    ~ZhibanAiReaderBoard() override {
        delete battery_monitor_;
    }

    virtual Led* GetLed() override {
        static GpioLed led(STATUS_LED_GPIO, 1);
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
            AUDIO_PA_EN,
            AUDIO_CODEC_ES8311_ADDR,
            AUDIO_CODEC_ES7210_ADDR,
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        if (battery_monitor_ == nullptr) {
            return false;
        }

        charging = battery_monitor_->IsCharging();
        discharging = !charging;
        level = battery_monitor_->GetBatteryLevel();
        return true;
    }
};

DECLARE_BOARD(ZhibanAiReaderBoard);