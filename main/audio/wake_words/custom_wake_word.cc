#include "custom_wake_word.h"
#include "audio_service.h"
#include "system_info.h"
#include "assets.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_mn_iface.h>
#include <esp_mn_models.h>
#include <esp_mn_speech_commands.h>
#include <cJSON.h>

#define TAG "CustomWakeWord"

CustomWakeWord::CustomWakeWord()
    : wake_word_pcm_(), wake_word_opus_() {
}

CustomWakeWord::~CustomWakeWord() {
    if (detection_task_handle_ != nullptr) {
        detection_task_should_stop_ = true;
        running_ = false;
        input_buffer_cv_.notify_all();
        while (!detection_task_stopped_) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    if (afe_data_ != nullptr) {
        afe_iface_->destroy(afe_data_);
        afe_data_ = nullptr;
    }

    if (multinet_model_data_ != nullptr && multinet_ != nullptr) {
        multinet_->destroy(multinet_model_data_);
        multinet_model_data_ = nullptr;
    }

    if (wake_word_encode_task_stack_ != nullptr) {
        heap_caps_free(wake_word_encode_task_stack_);
    }

    if (wake_word_encode_task_buffer_ != nullptr) {
        heap_caps_free(wake_word_encode_task_buffer_);
    }

    if (owns_models_ && models_ != nullptr) {
        esp_srmodel_deinit(models_);
    }
}

void CustomWakeWord::ParseWakenetModelConfig() {
    // Read index.json
    auto& assets = Assets::GetInstance();
    void* ptr = nullptr;
    size_t size = 0;
    if (!assets.GetAssetData("index.json", ptr, size)) {
        ESP_LOGE(TAG, "Failed to read index.json");
        return;
    }
    cJSON* root = cJSON_ParseWithLength(static_cast<char*>(ptr), size);
    if (root == nullptr) {
        ESP_LOGE(TAG, "Failed to parse index.json");
        return;
    }
    cJSON* multinet_model = cJSON_GetObjectItem(root, "multinet_model");
    if (cJSON_IsObject(multinet_model)) {
        cJSON* language = cJSON_GetObjectItem(multinet_model, "language");
        cJSON* duration = cJSON_GetObjectItem(multinet_model, "duration");
        cJSON* threshold = cJSON_GetObjectItem(multinet_model, "threshold");
        cJSON* commands = cJSON_GetObjectItem(multinet_model, "commands");
        if (cJSON_IsString(language)) {
            language_ = language->valuestring;
        }
        if (cJSON_IsNumber(duration)) {
            duration_ = duration->valueint;
        }
        if (cJSON_IsNumber(threshold)) {
            threshold_ = threshold->valuedouble;
        }
        if (cJSON_IsArray(commands)) {
            for (int i = 0; i < cJSON_GetArraySize(commands); i++) {
                cJSON* command = cJSON_GetArrayItem(commands, i);
                if (cJSON_IsObject(command)) {
                    cJSON* command_name = cJSON_GetObjectItem(command, "command");
                    cJSON* text = cJSON_GetObjectItem(command, "text");
                    cJSON* action = cJSON_GetObjectItem(command, "action");
                    if (cJSON_IsString(command_name) && cJSON_IsString(text) && cJSON_IsString(action)) {
                        commands_.push_back({command_name->valuestring, text->valuestring, action->valuestring});
                        ESP_LOGI(TAG, "Command: %s, Text: %s, Action: %s", command_name->valuestring, text->valuestring, action->valuestring);
                    }
                }
            }
        }
    }
    cJSON_Delete(root);
}


bool CustomWakeWord::Initialize(AudioCodec* codec, srmodel_list_t* models_list) {
    codec_ = codec;
    commands_.clear();
    owns_models_ = false;

    if (models_list == nullptr) {
        language_ = "cn";
        models_ = esp_srmodel_init("model");
        owns_models_ = true;
#ifdef CONFIG_CUSTOM_WAKE_WORD
        threshold_ = CONFIG_CUSTOM_WAKE_WORD_THRESHOLD / 100.0f;
        commands_.push_back({CONFIG_CUSTOM_WAKE_WORD, CONFIG_CUSTOM_WAKE_WORD_DISPLAY, "wake"});
#endif
    } else {
        models_ = models_list;
        ParseWakenetModelConfig();
    }

    if (models_ == nullptr || models_->num == -1) {
        ESP_LOGE(TAG, "Failed to initialize wakenet model");
        return false;
    }

    // 初始化 multinet (命令词识别)
    mn_name_ = esp_srmodel_filter(models_, ESP_MN_PREFIX, language_.c_str());
    if (mn_name_ == nullptr) {
        ESP_LOGW(TAG, "Language '%s' multinet not found, falling back to any multinet model", language_.c_str());
        mn_name_ = esp_srmodel_filter(models_, ESP_MN_PREFIX, NULL);
    }
    if (mn_name_ == nullptr) {
        ESP_LOGE(TAG, "Failed to initialize multinet, mn_name is nullptr");
        ESP_LOGI(TAG, "Please refer to https://pcn7cs20v8cr.feishu.cn/wiki/CpQjwQsCJiQSWSkYEvrcxcbVnwh to add custom wake word");
        return false;
    }

    multinet_ = esp_mn_handle_from_name(mn_name_);
    multinet_model_data_ = multinet_->create(mn_name_, duration_);
    multinet_->set_det_threshold(multinet_model_data_, threshold_);
    esp_mn_commands_clear();
    for (int i = 0; i < commands_.size(); i++) {
        esp_mn_commands_add(i + 1, commands_[i].command.c_str());
    }
    esp_mn_commands_update();
    
    multinet_->print_active_speech_commands(multinet_model_data_);

    int ref_num = codec_->input_reference() ? 1 : 0;
    std::string input_format;
    for (int i = 0; i < codec_->input_channels() - ref_num; i++) {
        input_format.push_back('M');
    }
    for (int i = 0; i < ref_num; i++) {
        input_format.push_back('R');
    }

    afe_config_t* afe_config = afe_config_init(input_format.c_str(), models_, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    afe_config->aec_init = codec_->input_reference();
    afe_config->aec_mode = AEC_MODE_SR_HIGH_PERF;
    afe_config->afe_perferred_core = 1;
    afe_config->afe_perferred_priority = 1;
    afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;

    afe_iface_ = esp_afe_handle_from_config(afe_config);
    afe_data_ = afe_iface_->create_from_config(afe_config);
    afe_config_free(afe_config);
    if (afe_data_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create AFE data for custom wake word");
        return false;
    }

    ESP_LOGI(TAG, "Custom wake word AFE ready, free heap: %lu, free PSRAM: %lu",
        (unsigned long)esp_get_free_heap_size(),
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    afe_feed_chunk_size_ = afe_iface_->get_feed_chunksize(afe_data_) * codec_->input_channels();
    if (detection_task_handle_ == nullptr) {
        detection_task_should_stop_ = false;
        detection_task_stopped_ = false;
        if (xTaskCreate([](void* arg) {
                auto this_ = (CustomWakeWord*)arg;
                this_->DetectionTask();
                vTaskDelete(NULL);
            }, "custom_ww_detect", 8192, this, 3, &detection_task_handle_) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create custom wake word detection task");
            detection_task_stopped_ = true;
            detection_task_handle_ = nullptr;
            return false;
        }
    }

    return true;
}

void CustomWakeWord::OnWakeWordDetected(std::function<void(const std::string& wake_word)> callback) {
    wake_word_detected_callback_ = callback;
}

void CustomWakeWord::Start() {
    {
        std::lock_guard<std::mutex> lock(input_buffer_mutex_);
        input_buffer_.clear();
        input_buffer_offset_ = 0;
        if (afe_data_ != nullptr) {
            afe_iface_->reset_buffer(afe_data_);
        }
    }
    {
        std::lock_guard<std::mutex> lock(detect_mutex_);
        if (multinet_ != nullptr && multinet_model_data_ != nullptr) {
            multinet_->clean(multinet_model_data_);
        }
    }
    running_ = true;
    input_buffer_cv_.notify_all();
}

void CustomWakeWord::Stop() {
    running_ = false;

    {
        std::lock_guard<std::mutex> lock(input_buffer_mutex_);
        input_buffer_.clear();
        input_buffer_offset_ = 0;
        if (afe_data_ != nullptr) {
            afe_iface_->reset_buffer(afe_data_);
        }
    }
    {
        std::lock_guard<std::mutex> lock(detect_mutex_);
        if (multinet_ != nullptr && multinet_model_data_ != nullptr) {
            multinet_->clean(multinet_model_data_);
        }
    }
    input_buffer_cv_.notify_all();
}

void CustomWakeWord::Feed(const std::vector<int16_t>& data) {
    if (afe_data_ == nullptr || afe_feed_chunk_size_ == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(input_buffer_mutex_);
    // Check running state inside lock to avoid TOCTOU race with Stop()
    if (!running_) {
        return;
    }

    input_buffer_.insert(input_buffer_.end(), data.begin(), data.end());
    while (input_buffer_.size() - input_buffer_offset_ >= afe_feed_chunk_size_) {
        afe_iface_->feed(afe_data_, input_buffer_.data() + input_buffer_offset_);
        input_buffer_offset_ += afe_feed_chunk_size_;
    }

    if (input_buffer_offset_ > 0) {
        if (input_buffer_offset_ >= input_buffer_.size()) {
            input_buffer_.clear();
            input_buffer_offset_ = 0;
        } else if (input_buffer_offset_ >= afe_feed_chunk_size_ * 2) {
            input_buffer_.erase(input_buffer_.begin(), input_buffer_.begin() + input_buffer_offset_);
            input_buffer_offset_ = 0;
        }
    }
}

size_t CustomWakeWord::GetFeedSize() {
    if (afe_data_ == nullptr) {
        return 0;
    }
    return afe_iface_->get_feed_chunksize(afe_data_);
}

void CustomWakeWord::DetectionTask() {
    while (!detection_task_should_stop_) {
        {
            std::unique_lock<std::mutex> lock(input_buffer_mutex_);
            input_buffer_cv_.wait(lock, [this]() {
                return detection_task_should_stop_ || running_;
            });
            if (detection_task_should_stop_) {
                break;
            }
            if (!running_ || afe_data_ == nullptr) {
                continue;
            }
        }

        auto res = afe_iface_->fetch_with_delay(afe_data_, pdMS_TO_TICKS(100));
        if (detection_task_should_stop_) {
            break;
        }
        if (!running_ || res == nullptr || res->ret_value == ESP_FAIL) {
            continue;
        }

        StoreWakeWordData(res->data, res->data_size / sizeof(int16_t));

        std::string detected_wake_word;
        {
            std::lock_guard<std::mutex> lock(detect_mutex_);
            if (!running_ || multinet_ == nullptr || multinet_model_data_ == nullptr) {
                continue;
            }

            esp_mn_state_t mn_state = multinet_->detect(multinet_model_data_, const_cast<int16_t*>(res->data));
            if (mn_state == ESP_MN_STATE_DETECTED) {
                esp_mn_results_t* mn_result = multinet_->get_results(multinet_model_data_);
                for (int i = 0; i < mn_result->num && running_; ++i) {
                    ESP_LOGI(TAG, "Custom wake word detected: command_id=%d, string=%s, prob=%f",
                        mn_result->command_id[i], mn_result->string, mn_result->prob[i]);
                    if (mn_result->command_id[i] <= 0 ||
                        mn_result->command_id[i] > static_cast<int>(commands_.size())) {
                        continue;
                    }
                    auto& command = commands_[mn_result->command_id[i] - 1];
                    if (command.action == "wake") {
                        detected_wake_word = command.text;
                        break;
                    }
                }
                multinet_->clean(multinet_model_data_);
            } else if (mn_state == ESP_MN_STATE_TIMEOUT) {
                ESP_LOGD(TAG, "Command word detection timeout, cleaning state");
                multinet_->clean(multinet_model_data_);
            }
        }

        if (!detected_wake_word.empty()) {
            last_detected_wake_word_ = detected_wake_word;
            running_ = false;
            {
                std::lock_guard<std::mutex> lock(input_buffer_mutex_);
                input_buffer_.clear();
                input_buffer_offset_ = 0;
                if (afe_data_ != nullptr) {
                    afe_iface_->reset_buffer(afe_data_);
                }
            }

            if (wake_word_detected_callback_) {
                wake_word_detected_callback_(last_detected_wake_word_);
            }
        }
    }

    detection_task_handle_ = nullptr;
    detection_task_stopped_ = true;
    input_buffer_cv_.notify_all();
}

void CustomWakeWord::StoreWakeWordData(const int16_t* data, size_t samples) {
    // store audio data to wake_word_pcm_
    wake_word_pcm_.emplace_back(data, data + samples);
    // keep about 2 seconds of data, detect duration is 30ms (sample_rate == 16000, chunksize == 512)
    while (wake_word_pcm_.size() > 2000 / 30) {
        wake_word_pcm_.pop_front();
    }
}

void CustomWakeWord::EncodeWakeWordData() {
    const size_t stack_size = 4096 * 7;
    wake_word_opus_.clear();
    if (wake_word_encode_task_stack_ == nullptr) {
        wake_word_encode_task_stack_ = (StackType_t*)heap_caps_malloc(stack_size, MALLOC_CAP_SPIRAM);
        assert(wake_word_encode_task_stack_ != nullptr);
    }
    if (wake_word_encode_task_buffer_ == nullptr) {
        wake_word_encode_task_buffer_ = (StaticTask_t*)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL);
        assert(wake_word_encode_task_buffer_ != nullptr);
    }

    wake_word_encode_task_ = xTaskCreateStatic([](void* arg) {
        auto this_ = (CustomWakeWord*)arg;
        {
            auto start_time = esp_timer_get_time();
            // Create encoder
            esp_opus_enc_config_t opus_enc_cfg = AS_OPUS_ENC_CONFIG();
            void* encoder_handle = nullptr;
            auto ret = esp_opus_enc_open(&opus_enc_cfg, sizeof(esp_opus_enc_config_t), &encoder_handle);
            if (encoder_handle == nullptr) {
                ESP_LOGE(TAG, "Failed to create audio encoder, error code: %d", ret);
                std::lock_guard<std::mutex> lock(this_->wake_word_mutex_);
                this_->wake_word_opus_.push_back(std::vector<uint8_t>());
                this_->wake_word_cv_.notify_all();
                return;
            }
            // Get frame size
            int frame_size = 0;
            int outbuf_size = 0;
            esp_opus_enc_get_frame_size(encoder_handle, &frame_size, &outbuf_size);
            frame_size = frame_size / sizeof(int16_t);
            // Encode all PCM data
            int packets = 0;
            std::vector<int16_t> in_buffer;
            esp_audio_enc_in_frame_t in = {};
            esp_audio_enc_out_frame_t out = {};
            for (auto& pcm: this_->wake_word_pcm_) {
                if (in_buffer.empty()) {
                    in_buffer = std::move(pcm);
                } else {
                    in_buffer.reserve(in_buffer.size() + pcm.size());
                    in_buffer.insert(in_buffer.end(), pcm.begin(), pcm.end());
                }
                while (in_buffer.size() >= frame_size) {
                    std::vector<uint8_t> opus_buf(outbuf_size);
                    in.buffer = (uint8_t *)(in_buffer.data());
                    in.len = (uint32_t)(frame_size * sizeof(int16_t));
                    out.buffer = opus_buf.data();
                    out.len = outbuf_size;
                    out.encoded_bytes = 0;
                    ret = esp_opus_enc_process(encoder_handle, &in, &out);
                    if (ret == ESP_AUDIO_ERR_OK) {
                        std::lock_guard<std::mutex> lock(this_->wake_word_mutex_);
                        this_->wake_word_opus_.emplace_back(opus_buf.data(), opus_buf.data() + out.encoded_bytes);
                        this_->wake_word_cv_.notify_all();
                        packets++;
                    } else {
                        ESP_LOGE(TAG, "Failed to encode audio, error code: %d", ret);
                    }
                    in_buffer.erase(in_buffer.begin(), in_buffer.begin() + frame_size);
                }
            }
            this_->wake_word_pcm_.clear();
            // Close encoder
            esp_opus_enc_close(encoder_handle);
            auto end_time = esp_timer_get_time();
            ESP_LOGI(TAG, "Encode wake word opus %d packets in %ld ms", packets, (long)((end_time - start_time) / 1000));

            std::lock_guard<std::mutex> lock(this_->wake_word_mutex_);
            this_->wake_word_opus_.push_back(std::vector<uint8_t>());
            this_->wake_word_cv_.notify_all();
        }
        vTaskDelete(NULL);
    }, "encode_wake_word", stack_size, this, 2, wake_word_encode_task_stack_, wake_word_encode_task_buffer_);
}

bool CustomWakeWord::GetWakeWordOpus(std::vector<uint8_t>& opus) {
    std::unique_lock<std::mutex> lock(wake_word_mutex_);
    wake_word_cv_.wait(lock, [this]() {
        return !wake_word_opus_.empty();
    });
    opus.swap(wake_word_opus_.front());
    wake_word_opus_.pop_front();
    return !opus.empty();
}
