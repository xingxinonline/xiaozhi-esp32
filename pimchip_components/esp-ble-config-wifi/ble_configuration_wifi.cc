#include "ble_configuration_wifi.h"
#include <cstdio>
#include <memory>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <esp_err.h>
#include <esp_event.h>
#include <esp_wifi.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <lwip/ip_addr.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <cJSON.h>

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_gatt_common_api.h"
#include "ssid_manager.h"
#if  USEING_BLUFI
#include "esp_blufi_api.h"
#include "esp_blufi.h"


#endif

#define TAG "BleConfigureWifi"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

#if  USEING_BLUFI
#define EXAMPLE_WIFI_CONNECTION_MAXIMUM_RETRY 2
#define EXAMPLE_INVALID_REASON                255
#define EXAMPLE_INVALID_RSSI                  -128

#define WIFI_LIST_NUM   10

static wifi_config_t sta_config;
static wifi_config_t ap_config;

/* FreeRTOS event group to signal when we are connected & ready to make a request */
static EventGroupHandle_t wifi_event_group;

/* The event group allows multiple bits for each event,
   but we only care about one event - are we connected
   to the AP with an IP? */
const int CONNECTED_BIT = BIT0;

static uint8_t example_wifi_retry = 0;

static bool gl_sta_connected = false;
static bool gl_sta_got_ip = false;
static bool ble_is_connected = false;
static uint8_t gl_sta_bssid[6];
static uint8_t gl_sta_ssid[32];
static int gl_sta_ssid_len;
static wifi_sta_list_t gl_sta_list;
static bool gl_sta_is_connecting = false;
static esp_blufi_extra_info_t gl_sta_conn_info;

extern "C" {
void blufi_dh_negotiate_data_handler(uint8_t *data, int len, uint8_t **output_data, int *output_len, bool *need_free);
int blufi_aes_encrypt(uint8_t iv8, uint8_t *crypt_data, int crypt_len);
int blufi_aes_decrypt(uint8_t iv8, uint8_t *crypt_data, int crypt_len);
uint16_t blufi_crc_checksum(uint8_t iv8, uint8_t *data, int len);

extern esp_err_t blufi_security_init(void);
extern void blufi_security_deinit(void);
}

#else


esp_gatt_char_prop_t a_property ;
esp_attr_value_t gatts_demo_char1_val;
prepare_type_env_t a_prepare_write_env;
static char test_device_name[ESP_BLE_ADV_NAME_LEN_MAX] = "LANDOUBAO_DEMO";
static uint8_t adv_config_done = 0;
static void gatts_wificfg_profile_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param);

esp_ble_adv_params_t cfgwifi_adv_params = {
    .adv_int_min = 0x20,
    .adv_int_max = 0x40,
    .adv_type = ADV_TYPE_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static struct gatts_profile_inst heart_rate_profile_tab[TOTAL_PROFILE_NUM] = {
    [WIFICFG_PROFILE_APP_IDX] = {
        .gatts_cb = &gatts_wificfg_profile_event_handler,
        .gatts_if = ESP_GATT_IF_NONE,       /* Not get the gatt_if, so initial is ESP_GATT_IF_NONE */
    },
};

#define adv_config_flag      (1 << 0)
#define scan_rsp_config_flag (1 << 1)

#endif

WifiConfigGATTsApp& WifiConfigGATTsApp::GetInstance() {
    static WifiConfigGATTsApp instance;
    return instance;
}

WifiConfigGATTsApp::WifiConfigGATTsApp()
{
    event_group_ = xEventGroupCreate();
}

WifiConfigGATTsApp::~WifiConfigGATTsApp()
{
    if (scan_timer_) {
        esp_timer_stop(scan_timer_);
        esp_timer_delete(scan_timer_);
    }
    if (event_group_) {
        vEventGroupDelete(event_group_);
    }
    // Unregister event handlers if they were registered
}

//static void gatts_wificfg_profile_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param);
#if  USEING_BLUFI

static void example_record_wifi_conn_info(int rssi, uint8_t reason)
{
    memset(&gl_sta_conn_info, 0, sizeof(esp_blufi_extra_info_t));
    if (gl_sta_is_connecting) {
        gl_sta_conn_info.sta_max_conn_retry_set = true;
        gl_sta_conn_info.sta_max_conn_retry = EXAMPLE_WIFI_CONNECTION_MAXIMUM_RETRY;
    } else {
        gl_sta_conn_info.sta_conn_rssi_set = true;
        gl_sta_conn_info.sta_conn_rssi = rssi;
        gl_sta_conn_info.sta_conn_end_reason_set = true;
        gl_sta_conn_info.sta_conn_end_reason = reason;
    }
}

void WifiConfigGATTsApp::example_wifi_connect(void)
{
    example_wifi_retry = 0;
    gl_sta_is_connecting = (esp_wifi_connect() == ESP_OK);
    example_record_wifi_conn_info(EXAMPLE_INVALID_RSSI, EXAMPLE_INVALID_REASON);
    /*
    std::string ssid((const char*)sta_config.sta.ssid, strlen((const char*)sta_config.sta.ssid));
    std::string psswd((const char*)sta_config.sta.password, strlen((const char*)sta_config.sta.password));
    if (!ConnectToWifi(ssid, psswd)){
        ESP_LOGE(TAG,"Connect to wifi failed!\n");
    }else{
        ESP_LOGI(TAG,"Connect to wifi OK!\n");
    }*/
}

static bool example_wifi_reconnect(void)
{
    bool ret;
    if (gl_sta_is_connecting && example_wifi_retry++ < EXAMPLE_WIFI_CONNECTION_MAXIMUM_RETRY) {
        ESP_LOGI(TAG,"BLUFI WiFi starts reconnection\n");
        gl_sta_is_connecting = (esp_wifi_connect() == ESP_OK);
        example_record_wifi_conn_info(EXAMPLE_INVALID_RSSI, EXAMPLE_INVALID_REASON);
        ret = true;
    } else {
        ret = false;
    }
    return ret;
}

static int softap_get_current_connection_number(void)
{
    esp_err_t ret;
    ret = esp_wifi_ap_get_sta_list(&gl_sta_list);
    if (ret == ESP_OK)
    {
        return gl_sta_list.num;
    }

    return 0;
}

static void ip_event_handler(void* arg, esp_event_base_t event_base,
    int32_t event_id, void* event_data)
{
    wifi_mode_t mode;

    switch (event_id) {
        case IP_EVENT_STA_GOT_IP: {
            esp_blufi_extra_info_t info;

            xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
            esp_wifi_get_mode(&mode);

            memset(&info, 0, sizeof(esp_blufi_extra_info_t));
            memcpy(info.sta_bssid, gl_sta_bssid, 6);
            info.sta_bssid_set = true;
            info.sta_ssid = gl_sta_ssid;
            info.sta_ssid_len = gl_sta_ssid_len;
            gl_sta_got_ip = true;
            if (ble_is_connected == true) {
                esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONN_SUCCESS, softap_get_current_connection_number(), &info);
            } else {
                ESP_LOGI(TAG,"BLUFI BLE is not connected yet\n");
            }
            break;
        }
        default:
            break;
    }
    return;
}

void WifiConfigGATTsApp::wifi_event_handler(void* arg, esp_event_base_t event_base,
    int32_t event_id, void* event_data)
{
    wifi_event_sta_connected_t *event;
    wifi_event_sta_disconnected_t *disconnected_event;
    wifi_mode_t mode;
    auto& self = WifiConfigGATTsApp::GetInstance();

    switch (event_id) {
    case WIFI_EVENT_STA_START:
        self.example_wifi_connect();
        break;
    case WIFI_EVENT_STA_CONNECTED:
        gl_sta_connected = true;
        gl_sta_is_connecting = false;
        event = (wifi_event_sta_connected_t*) event_data;
        memcpy(gl_sta_bssid, event->bssid, 6);
        memcpy(gl_sta_ssid, event->ssid, event->ssid_len);
        gl_sta_ssid_len = event->ssid_len;
        if (strlen((const char*)sta_config.sta.ssid) > 0){
            std::string ssid((const char*)sta_config.sta.ssid, strlen((const char*)sta_config.sta.ssid));
            std::string psswd((const char*)sta_config.sta.password, strlen((const char*)sta_config.sta.password));
            self.Save(ssid, psswd);
            //esp_restart();
            xTaskCreate([](void *ctx) {
                // 等待200ms确保HTTP响应完全发送
                vTaskDelay(pdMS_TO_TICKS(200));
                // 再等待1000ms确保所有连接都已关闭
                vTaskDelay(pdMS_TO_TICKS(2000));
                // 执行重启
                esp_restart();
            }, "reboot_task", 4096, &self, 5, NULL);
        }
        break;
    case WIFI_EVENT_STA_DISCONNECTED:
        /* Only handle reconnection during connecting */
        if (gl_sta_connected == false && example_wifi_reconnect() == false) {
            gl_sta_is_connecting = false;
            disconnected_event = (wifi_event_sta_disconnected_t*) event_data;
            example_record_wifi_conn_info(disconnected_event->rssi, disconnected_event->reason);
        }
        /* This is a workaround as ESP32 WiFi libs don't currently
        auto-reassociate. */
        gl_sta_connected = false;
        gl_sta_got_ip = false;
        memset(gl_sta_ssid, 0, 32);
        memset(gl_sta_bssid, 0, 6);
        gl_sta_ssid_len = 0;
        xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
        break;
    case WIFI_EVENT_AP_START:
        esp_wifi_get_mode(&mode);

        /* TODO: get config or information of softap, then set to report extra_info */
        if (ble_is_connected == true) {
            if (gl_sta_connected) {
                esp_blufi_extra_info_t info;
                memset(&info, 0, sizeof(esp_blufi_extra_info_t));
                memcpy(info.sta_bssid, gl_sta_bssid, 6);
                info.sta_bssid_set = true;
                info.sta_ssid = gl_sta_ssid;
                info.sta_ssid_len = gl_sta_ssid_len;
                esp_blufi_send_wifi_conn_report(mode, gl_sta_got_ip ? ESP_BLUFI_STA_CONN_SUCCESS : ESP_BLUFI_STA_NO_IP, softap_get_current_connection_number(), &info);
            } else if (gl_sta_is_connecting) {
                esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONNECTING, softap_get_current_connection_number(), &gl_sta_conn_info);
            } else {
                esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONN_FAIL, softap_get_current_connection_number(), &gl_sta_conn_info);
            }
        } else {
            ESP_LOGI(TAG,"BLUFI BLE is not connected yet\n");
        }
        break;
    case WIFI_EVENT_SCAN_DONE: {
        uint16_t apCount = 0;
        esp_wifi_scan_get_ap_num(&apCount);
        if (apCount == 0) {
            ESP_LOGI(TAG,"Nothing AP found");
            break;
        }
        wifi_ap_record_t *ap_list = (wifi_ap_record_t *)malloc(sizeof(wifi_ap_record_t) * apCount);
        if (!ap_list) {
            ESP_LOGE(TAG,"malloc error, ap_list is NULL");
            esp_wifi_clear_ap_list();
            break;
        }
        ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&apCount, ap_list));
        esp_blufi_ap_record_t * blufi_ap_list = (esp_blufi_ap_record_t *)malloc(apCount * sizeof(esp_blufi_ap_record_t));
        if (!blufi_ap_list) {
            if (ap_list) {
                free(ap_list);
            }
            ESP_LOGE(TAG,"malloc error, blufi_ap_list is NULL");
            break;
        }
        for (int i = 0; i < apCount; ++i)
        {
            blufi_ap_list[i].rssi = ap_list[i].rssi;
            memcpy(blufi_ap_list[i].ssid, ap_list[i].ssid, sizeof(ap_list[i].ssid));
        }

        if (ble_is_connected == true) {
            esp_blufi_send_wifi_list(apCount, blufi_ap_list);
        } else {
            ESP_LOGI(TAG,"BLUFI BLE is not connected yet\n");
        }

        esp_wifi_scan_stop();
        free(ap_list);
        free(blufi_ap_list);
        break;
    }
    case WIFI_EVENT_AP_STACONNECTED: {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        //ESP_LOGI(TAG,"station "MACSTR" join, AID=%d", MAC2STR(event->mac), event->aid);
        break;
    }
    case WIFI_EVENT_AP_STADISCONNECTED: {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        //ESP_LOGI(TAG,"station "MACSTR" leave, AID=%d, reason=%d", MAC2STR(event->mac), event->aid, event->reason);
        break;
    }

    default:
        break;
    }
    return;
}

void WifiConfigGATTsApp::initialise_wifi(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    wifi_event_group = xEventGroupCreate();
    //ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    assert(ap_netif);

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &WifiConfigGATTsApp::wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_event_handler, NULL));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
    example_record_wifi_conn_info(EXAMPLE_INVALID_RSSI, EXAMPLE_INVALID_REASON);
    ESP_ERROR_CHECK( esp_wifi_start() );
}

void WifiConfigGATTsApp::blufi_event_callback(esp_blufi_cb_event_t event, esp_blufi_cb_param_t *param)
{
    auto& self = WifiConfigGATTsApp::GetInstance();
    /* actually, should post to blufi_task handle the procedure,
     * now, as a example, we do it more simply */
    switch (event) {
    case ESP_BLUFI_EVENT_INIT_FINISH:
        ESP_LOGI(TAG,"BLUFI init finish\n");
        esp_blufi_adv_start();
        break;
    case ESP_BLUFI_EVENT_DEINIT_FINISH:
        ESP_LOGI(TAG,"BLUFI deinit finish\n");
        break;
    case ESP_BLUFI_EVENT_BLE_CONNECT:
        ESP_LOGI(TAG,"BLUFI ble connect\n");
        ble_is_connected = true;
        esp_blufi_adv_stop();
        blufi_security_init();
        break;
    case ESP_BLUFI_EVENT_BLE_DISCONNECT:
        ESP_LOGI(TAG,"BLUFI ble disconnect\n");
        ble_is_connected = false;
        blufi_security_deinit();
        esp_blufi_adv_start();
        break;
    case ESP_BLUFI_EVENT_SET_WIFI_OPMODE:
        ESP_LOGI(TAG,"BLUFI Set WIFI opmode %d\n", param->wifi_mode.op_mode);
        //ESP_ERROR_CHECK( esp_wifi_set_mode(param->wifi_mode.op_mode) );
        break;
    case ESP_BLUFI_EVENT_REQ_CONNECT_TO_AP:
        ESP_LOGI(TAG,"BLUFI requset wifi connect to AP\n");
        /* there is no wifi callback when the device has already connected to this wifi
        so disconnect wifi before connection.
        */
        esp_wifi_disconnect();
        self.example_wifi_connect();
        break;
    case ESP_BLUFI_EVENT_REQ_DISCONNECT_FROM_AP:
        ESP_LOGI(TAG,"BLUFI requset wifi disconnect from AP\n");
        esp_wifi_disconnect();
        break;
    case ESP_BLUFI_EVENT_REPORT_ERROR:
        ESP_LOGE(TAG,"BLUFI report error, error code %d\n", param->report_error.state);
        esp_blufi_send_error_info(param->report_error.state);
        break;
    case ESP_BLUFI_EVENT_GET_WIFI_STATUS: {
        wifi_mode_t mode;
        esp_blufi_extra_info_t info;

        esp_wifi_get_mode(&mode);

        if (gl_sta_connected) {
            memset(&info, 0, sizeof(esp_blufi_extra_info_t));
            memcpy(info.sta_bssid, gl_sta_bssid, 6);
            info.sta_bssid_set = true;
            info.sta_ssid = gl_sta_ssid;
            info.sta_ssid_len = gl_sta_ssid_len;
            esp_blufi_send_wifi_conn_report(mode, gl_sta_got_ip ? ESP_BLUFI_STA_CONN_SUCCESS : ESP_BLUFI_STA_NO_IP, softap_get_current_connection_number(), &info);
        } else if (gl_sta_is_connecting) {
            esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONNECTING, softap_get_current_connection_number(), &gl_sta_conn_info);
        } else {
            esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONN_FAIL, softap_get_current_connection_number(), &gl_sta_conn_info);
        }
        ESP_LOGI(TAG,"BLUFI get wifi status from AP\n");

        break;
    }
    case ESP_BLUFI_EVENT_RECV_SLAVE_DISCONNECT_BLE:
        ESP_LOGI(TAG,"blufi close a gatt connection");
        esp_blufi_disconnect();
        break;
    case ESP_BLUFI_EVENT_DEAUTHENTICATE_STA:
        /* TODO */
        break;
	case ESP_BLUFI_EVENT_RECV_STA_BSSID:
        memcpy(sta_config.sta.bssid, param->sta_bssid.bssid, 6);
        sta_config.sta.bssid_set = 1;
        esp_wifi_set_config(WIFI_IF_STA, &sta_config);
        ESP_LOGI(TAG,"Recv STA BSSID %s\n", sta_config.sta.ssid);
        break;
	case ESP_BLUFI_EVENT_RECV_STA_SSID:
        strncpy((char *)sta_config.sta.ssid, (char *)param->sta_ssid.ssid, param->sta_ssid.ssid_len);
        sta_config.sta.ssid[param->sta_ssid.ssid_len] = '\0';
        esp_wifi_set_config(WIFI_IF_STA, &sta_config);
        ESP_LOGI(TAG,"Recv STA SSID %s\n", sta_config.sta.ssid);
        break;
	case ESP_BLUFI_EVENT_RECV_STA_PASSWD:
        strncpy((char *)sta_config.sta.password, (char *)param->sta_passwd.passwd, param->sta_passwd.passwd_len);
        sta_config.sta.password[param->sta_passwd.passwd_len] = '\0';
        esp_wifi_set_config(WIFI_IF_STA, &sta_config);
        ESP_LOGI(TAG,"Recv STA PASSWORD %s\n", sta_config.sta.password);
        break;
	case ESP_BLUFI_EVENT_RECV_SOFTAP_SSID:
        strncpy((char *)ap_config.ap.ssid, (char *)param->softap_ssid.ssid, param->softap_ssid.ssid_len);
        ap_config.ap.ssid[param->softap_ssid.ssid_len] = '\0';
        ap_config.ap.ssid_len = param->softap_ssid.ssid_len;
        esp_wifi_set_config(WIFI_IF_AP, &ap_config);
        ESP_LOGI(TAG,"Recv SOFTAP SSID %s, ssid len %d\n", ap_config.ap.ssid, ap_config.ap.ssid_len);
        break;
	case ESP_BLUFI_EVENT_RECV_SOFTAP_PASSWD:
        strncpy((char *)ap_config.ap.password, (char *)param->softap_passwd.passwd, param->softap_passwd.passwd_len);
        ap_config.ap.password[param->softap_passwd.passwd_len] = '\0';
        esp_wifi_set_config(WIFI_IF_AP, &ap_config);
        ESP_LOGI(TAG,"Recv SOFTAP PASSWORD %s len = %d\n", ap_config.ap.password, param->softap_passwd.passwd_len);
        break;
	case ESP_BLUFI_EVENT_RECV_SOFTAP_MAX_CONN_NUM:
        if (param->softap_max_conn_num.max_conn_num > 4) {
            return;
        }
        ap_config.ap.max_connection = param->softap_max_conn_num.max_conn_num;
        esp_wifi_set_config(WIFI_IF_AP, &ap_config);
        ESP_LOGI(TAG,"Recv SOFTAP MAX CONN NUM %d\n", ap_config.ap.max_connection);
        break;
	case ESP_BLUFI_EVENT_RECV_SOFTAP_AUTH_MODE:
        if (param->softap_auth_mode.auth_mode >= WIFI_AUTH_MAX) {
            return;
        }
        ap_config.ap.authmode = param->softap_auth_mode.auth_mode;
        esp_wifi_set_config(WIFI_IF_AP, &ap_config);
        ESP_LOGI(TAG,"Recv SOFTAP AUTH MODE %d\n", ap_config.ap.authmode);
        break;
	case ESP_BLUFI_EVENT_RECV_SOFTAP_CHANNEL:
        if (param->softap_channel.channel > 13) {
            return;
        }
        ap_config.ap.channel = param->softap_channel.channel;
        esp_wifi_set_config(WIFI_IF_AP, &ap_config);
        ESP_LOGI(TAG,"Recv SOFTAP CHANNEL %d\n", ap_config.ap.channel);
        break;
    case ESP_BLUFI_EVENT_GET_WIFI_LIST:{
        wifi_scan_config_t scanConf = {
            .ssid = NULL,
            .bssid = NULL,
            .channel = 0,
            .show_hidden = false
        };
        esp_err_t ret = esp_wifi_scan_start(&scanConf, true);
        if (ret != ESP_OK) {
            esp_blufi_send_error_info(ESP_BLUFI_WIFI_SCAN_FAIL);
        }
        break;
    }
    case ESP_BLUFI_EVENT_RECV_CUSTOM_DATA:
        ESP_LOGI(TAG,"Recv Custom Data %" PRIu32 "\n", param->custom_data.data_len);
        esp_log_buffer_hex("Custom Data", param->custom_data.data, param->custom_data.data_len);
        break;
	case ESP_BLUFI_EVENT_RECV_USERNAME:
        /* Not handle currently */
        break;
	case ESP_BLUFI_EVENT_RECV_CA_CERT:
        /* Not handle currently */
        break;
	case ESP_BLUFI_EVENT_RECV_CLIENT_CERT:
        /* Not handle currently */
        break;
	case ESP_BLUFI_EVENT_RECV_SERVER_CERT:
        /* Not handle currently */
        break;
	case ESP_BLUFI_EVENT_RECV_CLIENT_PRIV_KEY:
        /* Not handle currently */
        break;;
	case ESP_BLUFI_EVENT_RECV_SERVER_PRIV_KEY:
        /* Not handle currently */
        break;
    default:
        break;
    }
}

static esp_blufi_callbacks_t blufi_callbacks = {
    .event_cb = &WifiConfigGATTsApp::blufi_event_callback,
    .negotiate_data_handler = blufi_dh_negotiate_data_handler,
    .encrypt_func = blufi_aes_encrypt,
    .decrypt_func = blufi_aes_decrypt,
    .checksum_func = blufi_crc_checksum,
};

esp_err_t esp_blufi_host_init(void)
{
    int ret;
    ret = esp_bluedroid_init();
    if (ret) {
        ESP_LOGE(TAG,"%s init bluedroid failed: %s\n", __func__, esp_err_to_name(ret));
        return ESP_FAIL;
    }

    ret = esp_bluedroid_enable();
    if (ret) {
        ESP_LOGE(TAG,"%s init bluedroid failed: %s\n", __func__, esp_err_to_name(ret));
        return ESP_FAIL;
    }
    //ESP_LOGI(TAG,"BD ADDR: "ESP_BD_ADDR_STR"\n", ESP_BD_ADDR_HEX(esp_bt_dev_get_address()));

    return ESP_OK;

}

void WifiConfigGATTsApp::Start()
{
    initialise_wifi();
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));
    ESP_ERROR_CHECK(esp_blufi_host_init());
    ESP_ERROR_CHECK(esp_blufi_register_callbacks(&blufi_callbacks));

    //ESP_ERROR_CHECK(esp_blufi_gap_register_callback());
    ESP_ERROR_CHECK(esp_ble_gap_register_callback(esp_blufi_gap_event_handler));
    esp_blufi_profile_init();
    return;
}

#else

void example_exec_write_event_env(prepare_type_env_t *prepare_write_env, esp_ble_gatts_cb_param_t *param){
    ESP_LOGI(TAG,"example_exec_write_event_env");
    
    return;
}
void example_write_event_env(esp_gatt_if_t gatts_if, prepare_type_env_t *prepare_write_env, esp_ble_gatts_cb_param_t *param){
    ESP_LOGI(TAG,"example_write_event_env");
    esp_gatt_status_t status = ESP_GATT_OK;
    cJSON *json = cJSON_Parse((const char *)param->write.value);
    //free(buf);
    if (!json) {
        cJSON_Delete(json);
        ESP_LOGE("TAG", "Invalid JSON:%s", param->write.value);
        return;
    }

    cJSON *ssid_item = cJSON_GetObjectItemCaseSensitive(json, "ssid");
    cJSON *password_item = cJSON_GetObjectItemCaseSensitive(json, "password");

    if (!cJSON_IsString(ssid_item) || (ssid_item->valuestring == NULL)) {
        ESP_LOGE("TAG", "Invalid ssid!");
        return;
    }

    std::string ssid_str = ssid_item->valuestring;
    ESP_LOGE("TAG", "SSID:%s.",ssid_item->valuestring);
    if (cJSON_IsString(password_item)){
        ESP_LOGE("TAG", "Password:%s.",password_item->valuestring);
    }else{
        ESP_LOGE("TAG", "Password:<None>");
    }

    if (param->write.need_rsp){
        if (param->write.is_prep) {
            
        }else{
            esp_ble_gatts_send_response(gatts_if, param->write.conn_id, param->write.trans_id, status, NULL);
        }
    }
    cJSON_Delete(json);
    return;
}

static void gatts_wificfg_profile_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param) 
{
    uint8_t beacon_adv_data[] = {
        /* Flags */
        0x02, ESP_BLE_AD_TYPE_FLAG, 0x06,               // Length 2, Data Type ESP_BLE_AD_TYPE_FLAG, Data 1 (LE General Discoverable Mode, BR/EDR Not Supported)
        /* TX Power Level */
        0x02, ESP_BLE_AD_TYPE_TX_PWR, 0xEB,             // Length 2, Data Type ESP_BLE_AD_TYPE_TX_PWR, Data 2 (-21)
        /* Complete 16-bit Service UUIDs */
        0x03, ESP_BLE_AD_TYPE_16SRV_CMPL, 0xAB, 0xCD    // Length 3, Data Type ESP_BLE_AD_TYPE_16SRV_CMPL, Data 3 (UUID)
    };

    uint8_t raw_scan_rsp_data[] = {
        /* Complete Local Name */
        0x0F, ESP_BLE_AD_TYPE_NAME_CMPL, 'E', 'S', 'P', '_', 'G', 'A', 'T', 'T', 'S', '_', 'X', 'X', 'X', 'X'   // Length 15, Data Type ESP_BLE_AD_TYPE_NAME_CMPL, Data (ESP_GATTS_DEMO)
    };

    switch (event) {
    case ESP_GATTS_REG_EVT:{
        esp_err_t tmp_ret;
        //2. server 把service 发送给client,设置BLE 名称
        ESP_LOGI("EVENT_HANDLER", "REGISTER_APP_EVT, status %d, app_id %d\n", param->reg.status, param->reg.app_id);
        heart_rate_profile_tab[WIFICFG_PROFILE_APP_IDX].service_id.is_primary = true;
        heart_rate_profile_tab[WIFICFG_PROFILE_APP_IDX].service_id.id.inst_id = 0x00;
        heart_rate_profile_tab[WIFICFG_PROFILE_APP_IDX].service_id.id.uuid.len = ESP_UUID_LEN_16;
        heart_rate_profile_tab[WIFICFG_PROFILE_APP_IDX].service_id.id.uuid.uuid.uuid16 = 0x1234;
        tmp_ret = esp_ble_gap_set_device_name(test_device_name);
        if (tmp_ret){
            ESP_LOGE("EVENT_HANDLER", "set device name failed, error code = %x", tmp_ret);
        }
        tmp_ret = esp_ble_gap_config_adv_data_raw(beacon_adv_data, sizeof(beacon_adv_data));
        if (tmp_ret){
            ESP_LOGE("EVENT_HANDLER", "config raw adv data failed, error code = %x", tmp_ret);
        }
        adv_config_done |= adv_config_flag;
        tmp_ret = esp_ble_gap_config_scan_rsp_data_raw(raw_scan_rsp_data, sizeof(raw_scan_rsp_data));
        if (tmp_ret){
            ESP_LOGE("EVENT_HANDLER", "config raw scan rsp data failed, error code = %x", tmp_ret);
        }
        adv_config_done |= scan_rsp_config_flag;

        tmp_ret = esp_ble_gatts_create_service(gatts_if, &heart_rate_profile_tab[WIFICFG_PROFILE_APP_IDX].service_id, 4);
        if (tmp_ret){
            ESP_LOGE("EVENT_HANDLER", "create service failed, error code = %x", tmp_ret);
        }
        break;
    }
    case ESP_GATTS_READ_EVT:{
        //GATT读取事件，手机读取开发板的数据
        //ESP_LOGI("EVENT_HANDLER", "GATT_READ_EVT, conn_id %d, trans_id %d, handle %d\n", param->read.conn_id, param->read.trans_id, param->read.handle);
        ESP_LOGI("EVENT_HANDLER", "GATT_READ_EVT");
        esp_gatt_rsp_t rsp;
        memset(&rsp, 0, sizeof(esp_gatt_rsp_t));
        rsp.attr_value.handle = param->read.handle;
        rsp.attr_value.len = 4;
        rsp.attr_value.value[0] = 0xde;
        rsp.attr_value.value[1] = 0xed;
        rsp.attr_value.value[2] = 0xbe;
        rsp.attr_value.value[3] = 0xef;
        esp_ble_gatts_send_response(gatts_if, param->read.conn_id, param->read.trans_id,
                                    ESP_GATT_OK, &rsp);
        break;
    }
    case ESP_GATTS_WRITE_EVT:{
        //GATT写事件，手机给开发板的发送数据，不需要回复
        //ESP_LOGI("EVENT_HANDLER", "GATT_WRITE_EVT, conn_id %d, trans_id %d, handle %d", param->write.conn_id, param->write.trans_id, param->write.handle);
        ESP_LOGI("EVENT_HANDLER", "GATT_WRITE_EVT");
        if (!param->write.is_prep){
            ESP_LOGI("EVENT_HANDLER", "GATT_WRITE_EVT, value len %d, value :", param->write.len);
            esp_log_buffer_hex("EVENT_HANDLER", param->write.value, param->write.len);
            if (heart_rate_profile_tab[WIFICFG_PROFILE_APP_IDX].descr_handle == param->write.handle && param->write.len == 2){
    
                uint16_t descr_value = param->write.value[1]<<8 | param->write.value[0];
                if (descr_value == 0x0001){
                    if (a_property & ESP_GATT_CHAR_PROP_BIT_NOTIFY){
                        ESP_LOGI("EVENT_HANDLER", "notify enable");
                        uint8_t notify_data[15];
                        for (int i = 0; i < sizeof(notify_data); ++i)
                        {
                            notify_data[i] = i%0xff;
                        }
                        //the size of notify_data[] need less than MTU size
                        esp_ble_gatts_send_indicate(gatts_if, param->write.conn_id, heart_rate_profile_tab[WIFICFG_PROFILE_APP_IDX].char_handle,
                                                sizeof(notify_data), notify_data, false);
                    }
                }else if (descr_value == 0x0002){
                    if (a_property & ESP_GATT_CHAR_PROP_BIT_INDICATE){
                        ESP_LOGI("EVENT_HANDLER", "indicate enable");
                        uint8_t indicate_data[15];
                        for (int i = 0; i < sizeof(indicate_data); ++i)
                        {
    
                            indicate_data[i] = i%0xff;
                        }
                        //the size of indicate_data[] need less than MTU size
                        esp_ble_gatts_send_indicate(gatts_if, param->write.conn_id, heart_rate_profile_tab[WIFICFG_PROFILE_APP_IDX].char_handle,
                                                sizeof(indicate_data), indicate_data, true);
                    }
                }
                else if (descr_value == 0x0000){
                    ESP_LOGI("EVENT_HANDLER", "notify/indicate disable ");
                }else{
    
                    ESP_LOGE("EVENT_HANDLER", "unknown descr value");
                    esp_log_buffer_hex("EVENT_HANDLER", param->write.value, param->write.len);
                }

            }
        }
        example_write_event_env(gatts_if, &a_prepare_write_env, param);
        break;
    }
    case ESP_GATTS_EXEC_WRITE_EVT: //GATT写事件，手机给开发板的发送数据，需要回复
        ESP_LOGI("EVENT_HANDLER","ESP_GATTS_EXEC_WRITE_EVT");
        esp_ble_gatts_send_response(gatts_if, param->write.conn_id, param->write.trans_id, ESP_GATT_OK, NULL);
        example_exec_write_event_env(&a_prepare_write_env, param);
        break;
    case ESP_GATTS_CREATE_EVT:{
        ESP_LOGI("EVENT_HANDLER", "CREATE_SERVICE_EVT, status %d,  service_handle %d\n", param->create.status, param->create.service_handle);
        //1. 将Characteristic 添加到service中 
        heart_rate_profile_tab[WIFICFG_PROFILE_APP_IDX].service_handle = param->create.service_handle;
        heart_rate_profile_tab[WIFICFG_PROFILE_APP_IDX].char_uuid.len = ESP_UUID_LEN_16;
        heart_rate_profile_tab[WIFICFG_PROFILE_APP_IDX].char_uuid.uuid.uuid16 = 0xABAB;
        esp_err_t star_ret = esp_ble_gatts_start_service(heart_rate_profile_tab[WIFICFG_PROFILE_APP_IDX].service_handle);
        if (star_ret){
            ESP_LOGE("EVENT_HANDLER", "esp_ble_gatts_start_service failed, error code=%d", star_ret);
        }
        // 把Characteristic 添加到service中
        a_property = ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_NOTIFY;
        esp_err_t add_char_ret = esp_ble_gatts_add_char(heart_rate_profile_tab[WIFICFG_PROFILE_APP_IDX].service_handle, &heart_rate_profile_tab[WIFICFG_PROFILE_APP_IDX].char_uuid,
                                                        ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
                                                        a_property,
                                                        &gatts_demo_char1_val, NULL);
        if (add_char_ret){
            ESP_LOGE("EVENT_HANDLER", "add char failed, error code = %x", add_char_ret);
        }
        break;
    }
    case ESP_GATTS_ADD_CHAR_EVT:{
        uint16_t length = 0;
        const uint8_t *prf_char;
        //ESP_LOGI("EVENT_HANDLER", "ADD_CHAR_EVT, status %x,  attr_handle %x, service_handle %x\n",
        //        param->add_char.status, param->add_char.attr_handle, param->add_char.service_handle);
        ESP_LOGI("EVENT_HANDLER", "ESP_GATTS_ADD_CHAR_EVT");
        //3.添加Characteristic  对应的Descriptor 
        heart_rate_profile_tab[WIFICFG_PROFILE_APP_IDX].char_handle = param->add_char.attr_handle;
        heart_rate_profile_tab[WIFICFG_PROFILE_APP_IDX].descr_uuid.len = ESP_UUID_LEN_16;
        heart_rate_profile_tab[WIFICFG_PROFILE_APP_IDX].descr_uuid.uuid.uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;

        esp_err_t get_attr_ret = esp_ble_gatts_get_attr_value(param->add_char.attr_handle,  &length, &prf_char);
        if (get_attr_ret == ESP_FAIL){
            ESP_LOGE("EVENT_HANDLER", "ILLEGAL HANDLE");
        }

        ESP_LOGI("EVENT_HANDLER", "the gatts demo char length = %x\n", length);
        for(int i = 0; i < length; i++){
            ESP_LOGI("EVENT_HANDLER", "prf_char[%x] =%x\n",i,prf_char[i]);
        }

        esp_err_t add_descr_ret = esp_ble_gatts_add_char_descr(heart_rate_profile_tab[WIFICFG_PROFILE_APP_IDX].service_handle, &heart_rate_profile_tab[WIFICFG_PROFILE_APP_IDX].descr_uuid,
                                                                ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE, NULL, NULL);
        if (add_descr_ret){
            ESP_LOGE("EVENT_HANDLER", "add char descr failed, error code =%x", add_descr_ret);
        }
        break;
    }
    case ESP_GATTS_ADD_CHAR_DESCR_EVT:// 添加描述事件
        heart_rate_profile_tab[WIFICFG_PROFILE_APP_IDX].descr_handle = param->add_char_descr.attr_handle;
        //ESP_LOGI("EVENT_HANDLER", "ADD_DESCR_EVT, status %x, attr_handle %x, service_handle %x\n",
        //         param->add_char_descr.status, param->add_char_descr.attr_handle, param->add_char_descr.service_handle);
        ESP_LOGI("EVENT_HANDLER", "ESP_GATTS_ADD_CHAR_DESCR_EVT");
        break;
    case ESP_GATTS_DELETE_EVT:
        break;
    case ESP_GATTS_START_EVT:
        //ESP_LOGI("EVENT_HANDLER", "SERVICE_START_EVT, status %d, service_handle %d\n", 
        //         param->start.status, param->start.service_handle);
        ESP_LOGI("EVENT_HANDLER", "ESP_GATTS_START_EVT");
        //if(param->start.service_handle == ota_handle_table[0]){
        //esp_err_t create_attr_ret1 = esp_ble_gatts_create_attr_tab(gatt_uart, gatts_if, IDX_APP_MAX, 0);
        //BLE_SERVER_DEBUG("esp_ble_gatts_create_attr_tab %d-%d", IDX_APP_MAX, 0);
        //if (create_attr_ret1){
        //    BLE_SERVER_DEBUG("create attr table2 failed, error code = %x", create_attr_ret1);
        //}
        //}
        break;
    case ESP_GATTS_STOP_EVT:
        ESP_LOGI("EVENT_HANDLER", "SERVICE_STOP_EVT");
        break;
    case ESP_GATTS_CONNECT_EVT: {
        // GATT 连接事件
        ESP_LOGI("EVENT_HANDLER", "ESP_GATTS_CONNECT_EVT");
        esp_ble_conn_update_params_t conn_params = {0};
        memcpy(conn_params.bda, param->connect.remote_bda, sizeof(esp_bd_addr_t));
        /* For the IOS system, please reference the apple official documents about the ble connection parameters restrictions. */
        conn_params.latency = 0;
        conn_params.max_int = 0x20;    // max_int = 0x20*1.25ms = 40ms
        conn_params.min_int = 0x10;    // min_int = 0x10*1.25ms = 20ms
        conn_params.timeout = 400;    // timeout = 400*10ms = 4000ms
        ESP_LOGI("EVENT_HANDLER", "ESP_GATTS_CONNECT_EVT, conn_id %d, remote %02x:%02x:%02x:%02x:%02x:%02x:",
                    param->connect.conn_id,
                    param->connect.remote_bda[0], param->connect.remote_bda[1], param->connect.remote_bda[2],
                    param->connect.remote_bda[3], param->connect.remote_bda[4], param->connect.remote_bda[5]);
        heart_rate_profile_tab[WIFICFG_PROFILE_APP_IDX].conn_id = param->connect.conn_id;
        //start sent the update connection parameters to the peer device.
        esp_ble_gap_update_conn_params(&conn_params);
        break;
    }
    case ESP_GATTS_DISCONNECT_EVT://断开连接事件
        ESP_LOGI("EVENT_HANDLER", "ESP_GATTS_DISCONNECT_EVT, disconnect reason 0x%x", param->disconnect.reason);
        esp_ble_gap_start_advertising(&cfgwifi_adv_params);
        break;
    case ESP_GATTS_CONF_EVT: //GATT配置事件
        ESP_LOGI("EVENT_HANDLER", "ESP_GATTS_CONF_EVT, status %d attr_handle %d", param->conf.status, param->conf.handle);
        if (param->conf.status != ESP_GATT_OK){
   
            esp_log_buffer_hex("EVENT_HANDLER", param->conf.value, param->conf.len);
        }
        break;
    case ESP_GATTS_MTU_EVT:
        ESP_LOGI("EVENT_HANDLER", "ESP_GATTS_MTU_EVT, MTU %d", param->mtu.mtu);
        break;
    default:
        break;
    }
}

void WifiConfigGATTsApp::gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {

    switch (event) {
        case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
            ESP_LOGI(TAG, "Advertisement data raw set complete");
            adv_config_done &= (~adv_config_flag);
            if (adv_config_done==0){
    
                esp_ble_gap_start_advertising(&cfgwifi_adv_params);
            }
            break;
        case ESP_GAP_BLE_SCAN_RSP_DATA_RAW_SET_COMPLETE_EVT:
            ESP_LOGI(TAG, "Scan response data raw set complete");
            adv_config_done &= (~scan_rsp_config_flag);
            if (adv_config_done==0){
    
                esp_ble_gap_start_advertising(&cfgwifi_adv_params);
            }
            break;
        case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
            ESP_LOGI(TAG, "Advertisement data set complete");
            adv_config_done &= (~adv_config_flag);
            if (adv_config_done == 0){
                esp_ble_gap_start_advertising(&cfgwifi_adv_params);
            }
            break;
        case ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT:
            ESP_LOGI(TAG, "Scan response data set complete");
            adv_config_done &= (~scan_rsp_config_flag);
            if (adv_config_done == 0){
                esp_ble_gap_start_advertising(&cfgwifi_adv_params);
            }
            break;
        case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
            if (param->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS) {
                ESP_LOGI(TAG, "Advertisement started successfully");
            } else {
                ESP_LOGE(TAG, "Failed to start advertisement");
            }
            break;
        case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
            if (param->adv_stop_cmpl.status != ESP_BT_STATUS_SUCCESS) {
                ESP_LOGE(TAG, "Advertising stop failed\n");
            } else {
                ESP_LOGI(TAG, "Stop adv successfully\n");
            }
            break;
        case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT: // 设备连接事件,可获取当前连接的设备信息
            ESP_LOGI(TAG, "update connection params status = %d, min_int = %d, max_int = %d,conn_int = %d,latency = %d, timeout = %d",
                param->update_conn_params.status,
                param->update_conn_params.min_int,
                param->update_conn_params.max_int,
                param->update_conn_params.conn_int,
                param->update_conn_params.latency,
                param->update_conn_params.timeout);
        case ESP_GAP_BLE_SET_PKT_LENGTH_COMPLETE_EVT:
            ESP_LOGI(TAG, "Packet length update, status %d, rx %d, tx %d",
                param->pkt_data_length_cmpl.status,
                param->pkt_data_length_cmpl.params.rx_len,
                param->pkt_data_length_cmpl.params.tx_len);
           break;
        default:
            break;
    }
}

void WifiConfigGATTsApp::gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param)
{
    ESP_LOGI("GATTS HANDLLER", "EVT %d, gatts if %d\n", event, gatts_if);

    /* If event is register event, store the gatts_if for each profile */
    if (event == ESP_GATTS_REG_EVT) {
        if (param->reg.status == ESP_GATT_OK) {
            esp_ble_gatt_set_local_mtu(247);
            heart_rate_profile_tab[param->reg.app_id].gatts_if = gatts_if;
            ESP_LOGI("GATTS HANDLLER", "Reg app OK, app_id %04x, status %d\n",
                param->reg.app_id,
                param->reg.status);
        } else {
            ESP_LOGI("GATTS HANDLLER", "Reg app failed, app_id %04x, status %d\n",
                param->reg.app_id,
                param->reg.status);
            return;
        }
    }else if(event ==  ESP_GATTS_MTU_EVT){
        // MTU协商完成
        ESP_LOGI("GATTS HANDLLER", "MTU size: %d", param->mtu.mtu);
    }

    do {
        int idx;
        for (idx = 0; idx < TOTAL_PROFILE_NUM; idx++) {
            if (gatts_if == ESP_GATT_IF_NONE || /* ESP_GATT_IF_NONE, not specify a certain gatt_if, need to call every profile cb function */
                gatts_if == heart_rate_profile_tab[idx].gatts_if) {
                if (heart_rate_profile_tab[idx].gatts_cb) {
                    heart_rate_profile_tab[idx].gatts_cb(event, gatts_if, param);
                }
            }
        }
    } while (0);
}

void WifiConfigGATTsApp::Start()
{
    //ESP_LOGI(TAG, "Free Heap size: %d bytes.",xPortGetFreeHeapSize());
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));

    // Initialize Bluedroid
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    // Register GAP event handler
    ESP_ERROR_CHECK(esp_ble_gatts_register_callback(&WifiConfigGATTsApp::gatts_event_handler));
    ESP_ERROR_CHECK(esp_ble_gap_register_callback(&WifiConfigGATTsApp::gap_event_handler));
    ESP_ERROR_CHECK(esp_ble_gatts_app_register(WIFICFG_PROFILE_APP_IDX));
    
    ESP_ERROR_CHECK(esp_ble_gatt_set_local_mtu(500));
}
#endif
bool WifiConfigGATTsApp::ConnectToWifi(const std::string &ssid, const std::string &password)
{
    if (ssid.empty()) {
        ESP_LOGE(TAG, "SSID cannot be empty");
        return false;
    }
    
    if (ssid.length() > 32) {  // WiFi SSID 最大长度
        ESP_LOGE(TAG, "SSID too long");
        return false;
    }
    
    is_connecting_ = true;
    esp_wifi_scan_stop();
    xEventGroupClearBits(event_group_, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    wifi_config_t wifi_config;
    bzero(&wifi_config, sizeof(wifi_config));
    strcpy((char *)wifi_config.sta.ssid, ssid.c_str());
    strcpy((char *)wifi_config.sta.password, password.c_str());
    wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_config.sta.failure_retry_cnt = 1;
    
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    auto ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to connect to WiFi: %d", ret);
        is_connecting_ = false;
        return false;
    }
    ESP_LOGI(TAG, "Connecting to WiFi %s", ssid.c_str());

    // Wait for the connection to complete for 5 seconds
    EventBits_t bits = xEventGroupWaitBits(event_group_, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdTRUE, pdFALSE, pdMS_TO_TICKS(10000));
    is_connecting_ = false;

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to WiFi %s", ssid.c_str());
        esp_wifi_disconnect();
        return true;
    } else {
        ESP_LOGE(TAG, "Failed to connect to WiFi %s", ssid.c_str());
        return false;
    }
}

void WifiConfigGATTsApp::Save(const std::string &ssid, const std::string &password)
{
    ESP_LOGI(TAG, "Save SSID %s %d", ssid.c_str(), ssid.length());
    SsidManager::GetInstance().AddSsid(ssid, password);
}
