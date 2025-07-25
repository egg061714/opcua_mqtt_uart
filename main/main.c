#include <stdio.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "driver/uart.h"

#define WIFI_SSID      "esp32_user"
#define WIFI_PASS      "0916747615"
#define MQTT_BROKER    "mqtt://192.168.0.66:1883"  // 先用本地部屬mqtt broker
#define MQTT_TOPIC     "esp32_data"   //訂閱網頁後台的topic

#define UART_TX_PIN 17
#define UART_RX_PIN 16
#define UART_PORT   UART_NUM_1
#define UART_BAUD   9600

char rs232_buffer[128];



static const char *TAG = "opcua_mqtt_uart";
static esp_mqtt_client_handle_t client;


////////UART初始化///////
static void uart_init(void) {
    const uart_config_t uart_config = {
        .baud_rate = UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    uart_driver_install(UART_PORT, 1024 * 2, 0, 0, NULL, 0);
    uart_param_config(UART_PORT, &uart_config);
    uart_set_pin(UART_PORT, UART_TX_PIN, UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
}

void mqtt_data_handler(const char *json_data) {
    cJSON *root = cJSON_Parse(json_data);
    if (!root) return;

    cJSON *deviceType_item = cJSON_GetObjectItem(root, "deviceType");
    cJSON *deviceID_item = cJSON_GetObjectItem(root, "deviceID");
    cJSON *currentQty_item = cJSON_GetObjectItem(root, "currentQty");
    cJSON *setQty_item = cJSON_GetObjectItem(root, "setQty");
    cJSON *status_item = cJSON_GetObjectItem(root, "status");
    cJSON *temp_item = cJSON_GetObjectItem(root, "temp");
    cJSON *bootTime_item = cJSON_GetObjectItem(root, "bootTime");
    cJSON *runningTime_item = cJSON_GetObjectItem(root, "runningTime");
    cJSON *errocode_item = cJSON_GetObjectItem(root, "errocode");

    if (!cJSON_IsString(deviceID_item) || !cJSON_IsString(currentQty_item) || !cJSON_IsString(status_item) || !cJSON_IsString(deviceType_item) || !cJSON_IsString(setQty_item) || !cJSON_IsString(temp_item) ||!cJSON_IsString(bootTime_item) || !cJSON_IsString(runningTime_item) || !cJSON_IsString(errocode_item)) {
        ESP_LOGE(TAG, "JSON 格式錯誤");
        cJSON_Delete(root);
        return;
    }

    const char *device_id = deviceID_item->valuestring;
    const char *current_qty = currentQty_item->valuestring;
    const char *status = status_item->valuestring;
    const char *deviceType = deviceType_item->valuestring;
    const char *setQty = setQty_item->valuestring;
    const char *temp = temp_item->valuestring;
    const char *bootTime = bootTime_item->valuestring;
    const char *runningTime = runningTime_item->valuestring;
    const char *errocode = errocode_item->valuestring;

    // 拼接 RS-232 格式
    sprintf(rs232_buffer, "\x02%s'%s'%s'%s'%s'%s'%s'%s'%s\x0D", device_id, current_qty, status, deviceType, setQty, temp, bootTime, runningTime, errocode);
    ESP_LOGI(TAG, "RS-232 發送: %s", rs232_buffer);

    uart_write_bytes(UART_PORT, rs232_buffer, strlen(rs232_buffer));
    cJSON_Delete(root);
}





static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) 
{
    ESP_LOGI(TAG, "MQTT 事件 ID: %" PRId32, event_id);
    esp_mqtt_event_handle_t event = event_data;
    switch (event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT 連線成功！");
            esp_mqtt_client_subscribe(client, MQTT_TOPIC, 0);
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT 斷線！");
            break;
        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "收到 MQTT 訊息: %.*s", event->data_len, event->data);
            char json_str[256];
            snprintf(json_str, sizeof(json_str), "%.*s", event->data_len, event->data);
            mqtt_data_handler(json_str);  // 直接解析 JSON 並輸出 RS-232
            break;
        default:
            ESP_LOGI(TAG, "其他 MQTT 事件: %" PRId32, event_id);
            break;
    }
}

// 初始化 MQTT
static void mqtt_app_start(void) {
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER,  // MQTT Broker 連線設定
    };

    client = esp_mqtt_client_init(&mqtt_cfg);
    
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, client);
    esp_mqtt_client_start(client);
}

// WiFi 事件處理
static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    if (event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi 斷線，重新連接...");
        esp_wifi_connect();
    } else if (event_id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "WiFi 連線成功！");
        mqtt_app_start();  // WiFi 連線成功後，啟動 MQTT
    }
}

// 初始化 WiFi
static void wifi_init(void) {
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip);
    
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();
}

// 主程式
void app_main(void) {
    // 初始化 NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    uart_init();
    // 初始化 WiFi
    wifi_init();


}