#ifndef _WIFI_CONFIGURATION_AP_H_
#define _WIFI_CONFIGURATION_AP_H_

#include <string>
#include <esp_event.h>
#include <esp_timer.h>
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_gatt_common_api.h"

#define USEING_BLUFI                    1

#if  USEING_BLUFI
#include "esp_blufi_api.h"
#include "esp_blufi.h"
// void blufi_dh_negotiate_data_handler(uint8_t *data, int len, uint8_t **output_data, int *output_len, bool *need_free);
// int blufi_aes_encrypt(uint8_t iv8, uint8_t *crypt_data, int crypt_len);
// int blufi_aes_decrypt(uint8_t iv8, uint8_t *crypt_data, int crypt_len);
// uint16_t blufi_crc_checksum(uint8_t iv8, uint8_t *data, int len);

// esp_err_t blufi_security_init(void);
// void blufi_security_deinit(void);

#else
#define TOTAL_PROFILE_NUM                       1
#define WIFICFG_PROFILE_APP_IDX                 0

struct gatts_profile_inst {
    esp_gatts_cb_t gatts_cb;
    uint16_t gatts_if;
    uint16_t app_id;
    uint16_t conn_id;
    uint16_t service_handle;
    esp_gatt_srvc_id_t service_id;
    uint16_t char_handle;
    esp_bt_uuid_t char_uuid;
    esp_gatt_perm_t perm;
    esp_gatt_char_prop_t property;
    uint16_t descr_handle;
    esp_bt_uuid_t descr_uuid;
};

typedef struct {
    uint8_t                 *prepare_buf;
    int                     prepare_len;
} prepare_type_env_t;

#endif
class WifiConfigGATTsApp {
public:
    static WifiConfigGATTsApp& GetInstance();
    void SetSsidPrefix(const std::string &&ssid_prefix);
    void SetDeviceId(const std::string &&device_id);
    void SetLanguage(const std::string &&language);
    void Start();

    std::string GetSsid();

    // Delete copy constructor and assignment operator
    WifiConfigGATTsApp(const WifiConfigGATTsApp&) = delete;
    WifiConfigGATTsApp& operator=(const WifiConfigGATTsApp&) = delete;
    static void blufi_event_callback(esp_blufi_cb_event_t event, esp_blufi_cb_param_t *param);
private:
    // Private constructor
    WifiConfigGATTsApp();
    ~WifiConfigGATTsApp();

    EventGroupHandle_t event_group_;
    std::string ssid_prefix_;
    std::string device_id_;
    std::string language_;
    esp_timer_handle_t scan_timer_ = nullptr;
    bool is_connecting_ = false;

    bool ConnectToWifi(const std::string &ssid, const std::string &password);
    void Save(const std::string &ssid, const std::string &password);
    
#if  USEING_BLUFI
    void initialise_wifi(void);
    void example_wifi_connect(void);
    static void wifi_event_handler(void* arg, esp_event_base_t event_base,int32_t event_id, void* event_data);
#else

    // Event handlers
    static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);
    static void gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param);
#endif
};

#endif // _WIFI_CONFIGURATION_AP_H_
