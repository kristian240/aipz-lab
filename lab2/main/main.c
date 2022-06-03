#include "cJSON.h"
#include "driver/adc.h"
#include "driver/gpio.h"
#include "esp_adc_cal.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"
#include "nvs_flash.h"

#define ADC_EXAMPLE_CALI_SCHEME ESP_ADC_CAL_VAL_EFUSE_TP_FIT
#define ADC_EXAMPLE_ATTEN ADC_ATTEN_DB_11
#define EXAMPLE_ESP_MAXIMUM_RETRY 5
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

#define SETTINGS_STORAGE "app_settings"
enum AppSettingEnum
{
    ResetCounter,
    DoorSensorChannel,
    LightSensorChannel,
    IpAddress,
    DefaultGateway,
    Subnet,
    WifiSsid,
    WifiPassword,
};
static const char *AppSetting[] = {"reset-counter", "light-pin", "ip", "wifi-ssid", "wifi-password"};

static nvs_handle_t settings_handle;
static httpd_handle_t server_handle;
static bool calibration_enabled = false;
static esp_adc_cal_characteristics_t adc1_chars;
static EventGroupHandle_t s_wifi_event_group;
static bool digital_data_value = false;
static QueueHandle_t gpio_evt_queue = NULL;

esp_err_t update_reset_counter()
{
    int counter = 0;
    ESP_LOGI("APP", "Updating reset counter.");
    esp_err_t err = nvs_get_i32(settings_handle, AppSetting[ResetCounter], &counter);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND)
    {
        return err;
    }
    counter++;
    ESP_LOGI("APP", "Reset counter updated to: %d", counter);
    nvs_set_i32(settings_handle, AppSetting[ResetCounter], counter);
    nvs_commit(settings_handle);
    return ESP_OK;
}

static void adc_calibration_init(void)
{
    esp_err_t err = esp_adc_cal_check_efuse(ADC_EXAMPLE_CALI_SCHEME);
    if (err == ESP_ERR_NOT_SUPPORTED)
    {
        ESP_LOGW("ADC", "Calibration scheme not supported, skip software calibration");
        return;
    }
    if (err == ESP_ERR_INVALID_VERSION)
    {
        ESP_LOGW("ADC", "eFuse not burnt, skip software calibration");
        return;
    }
    if (err == ESP_OK)
    {
        esp_adc_cal_characterize(ADC_UNIT_1, ADC_EXAMPLE_ATTEN, ADC_WIDTH_BIT_DEFAULT, 0, &adc1_chars);
        calibration_enabled = true;
        return;
    }

    ESP_LOGE("ADC", "Invalid arg");
}

static adc1_channel_t get_light_sensor_channel()
{
    int8_t channel = ADC1_CHANNEL_2;
    esp_err_t err = nvs_get_i8(settings_handle, AppSetting[LightSensorChannel], &channel);
    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        ESP_LOGI("Light sensor", "Channel not set, using default value!");
    }

    ESP_LOGI("Light sensor", "Channel used: %d", channel);

    return (adc1_channel_t)channel;
}

static void init_light_sensor()
{
    adc1_channel_t sensor_channel = get_light_sensor_channel();

    adc_calibration_init();
    ESP_ERROR_CHECK(adc1_config_width(ADC_WIDTH_BIT_DEFAULT));
    ESP_ERROR_CHECK(adc1_config_channel_atten(sensor_channel, ADC_EXAMPLE_ATTEN));
}

static int get_light_data()
{
    adc1_channel_t sensor_channel = get_light_sensor_channel();

    int light_value = adc1_get_raw(sensor_channel);
    ESP_LOGI("Light sensor", "raw: %d", light_value);

    return light_value;
}

static esp_err_t get_data_handler(httpd_req_t *req)
{
    int light_data_value = get_light_data();

    ESP_LOGI("Light sensor", "raw: %d", light_data_value);
    ESP_LOGI("Digital sensor", "raw: %d", digital_data_value);

    httpd_resp_set_type(req, "application/json");
    cJSON *root = cJSON_CreateObject();
    cJSON *room = cJSON_AddObjectToObject(root, "room");
    cJSON_AddBoolToObject(room, "door", digital_data_value);
    cJSON *light = cJSON_AddObjectToObject(room, "light");
    cJSON_AddNumberToObject(light, "raw", light_data_value);
    if (calibration_enabled)
    {
        uint32_t voltage = esp_adc_cal_raw_to_voltage(light_data_value, &adc1_chars);
        ESP_LOGI("Light sensor", "cal: %d mV", voltage);
        cJSON_AddNumberToObject(light, "voltage", voltage);
    }
    const char *sys_info = cJSON_Print(root);
    httpd_resp_sendstr(req, sys_info);
    free((void *)sys_info);
    cJSON_Delete(root);
    return ESP_OK;
}

static const httpd_uri_t light = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = get_data_handler,
    .user_ctx = NULL};

static void start_web_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;

    // Start the httpd server
    ESP_LOGI("WebServer", "Starting server on port: '%d'", config.server_port);
    if (httpd_start(&server_handle, &config) == ESP_OK)
    {
        // Set URI handlers
        ESP_LOGI("WebServer", "Registering URI handlers");
        httpd_register_uri_handler(server_handle, &light);
        return;
    }

    ESP_LOGI("WebServer", "Error starting server!");
    return;
}

static char *get_ip_address(void)
{
    size_t length = 0;
    char *ip = "192.168.0.150";
    nvs_get_str(settings_handle, AppSetting[IpAddress], NULL, &length);

    if (length == 0)
    {
        ESP_LOGI("Network", "IP not set, using default value!");
        ESP_LOGI("Network", "IP used: %s", ip);
        return ip;
    }

    nvs_get_str(settings_handle, AppSetting[IpAddress], ip, &length);

    ESP_LOGI("Network", "IP used: %s", ip);

    return ip;
}

static char *get_subnet_mask(void)
{
    size_t length = 0;
    nvs_get_str(settings_handle, AppSetting[Subnet], NULL, &length);

    if (length == 0)
    {
        ESP_LOGI("Network", "Subnet not set, using default value!");
        ESP_LOGI("Network", "Subnet used: %s", "255.255.255.0");
        return "255.255.255.0";
    }

    char *subnet = "";
    nvs_get_str(settings_handle, AppSetting[Subnet], subnet, &length);

    ESP_LOGI("Network", "Subnet used: %s", subnet);

    return subnet;
}

static char *get_gw_address(void)
{
    size_t length = 0;
    nvs_get_str(settings_handle, AppSetting[DefaultGateway], NULL, &length);

    if (length == 0)
    {
        ESP_LOGI("Network", "Default gateway not set, using default value!");
        ESP_LOGI("Network", "Default gateway used: %s", "192.168.178.1");
        return "192.168.178.1";
    }

    char *gw = "";
    nvs_get_str(settings_handle, AppSetting[DefaultGateway], gw, &length);

    ESP_LOGI("Network", "Default gateway used: %s", gw);

    return gw;
}

static void init_network_adapter(void)
{
    char *ip = get_ip_address();
    char *subnet = get_subnet_mask();
    char *default_gateway = get_gw_address();

    esp_netif_ip_info_t ip_info = {
        .ip = {.addr = ipaddr_addr(ip)},
        .netmask = {.addr = ipaddr_addr(subnet)},
        .gw = {.addr = ipaddr_addr(default_gateway)}};

    esp_netif_set_ip_info(esp_netif_create_default_wifi_sta(), &ip_info);
}

static char *get_ssid(void)
{
    size_t length = 0;
    char *ssid = "A1 WLAN_28F2A5";
    nvs_get_str(settings_handle, AppSetting[WifiSsid], NULL, &length);

    if (length == 0)
    {
        ESP_LOGI("Network", "SSID not set, using default value!");
        ESP_LOGI("Network", "SSID used: %s", ssid);
        return ssid;
    }

    nvs_get_str(settings_handle, AppSetting[WifiSsid], ssid, &length);

    ESP_LOGI("Network", "SSID used: %s", ssid);

    return ssid;
}

static char *get_wifi_password(void)
{
    size_t length = 0;
    char *password = "Kljucicbrdo11";
    nvs_get_str(settings_handle, AppSetting[WifiPassword], NULL, &length);

    if (length == 0)
    {
        ESP_LOGI("Network", "Wifi Password not set, using default value!");
        ESP_LOGI("Network", "Wifi Password used: %s", password);
        return password;
    }

    nvs_get_str(settings_handle, AppSetting[WifiPassword], password, &length);

    ESP_LOGI("Network", "Wifi Password used: %s", password);

    return password;
}

static int s_retry_num = 0;

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    ESP_LOGI("NETWORK", "event occured: event_base: %s, event_id: %d", event_base, event_id);

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        if (s_retry_num++ < EXAMPLE_ESP_MAXIMUM_RETRY)
        {
            esp_wifi_connect();
            ESP_LOGI("NETWORK", "retry to connect to the AP");
        }
        else
        {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI("NETWORK", "connect to the AP fail");
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI("NETWORK", "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void init_wifi(void)
{
    esp_err_t err;
    char *ssid = get_ssid(), *password = get_wifi_password();

    if (strlen(ssid) == 0)
    {
        return;
    }

    s_wifi_event_group = xEventGroupCreate();

    esp_event_handler_instance_t instance_any_id;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));

    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .bssid_set = 0,
            /* Setting a password implies station will connect to all security modes including WEP/WPA.
             * However these modes are deprecated and not advisable to be used. Incase your Access point
             * doesn't support WPA2, these mode can be enabled by commenting below line */
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    memcpy(wifi_config.sta.ssid, ssid, strlen(ssid));
    memcpy(wifi_config.sta.password, password, strlen(password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    ESP_ERROR_CHECK(err);
    if (err)
    {
        ESP_LOGI("Network", "err ocurred: %d", err);
    }

    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI("Network", "wifi_init_sta finished.");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT)
    {
        ESP_LOGI("Network", "connected to ap SSID:%s password:%s",
                 ssid, password);
    }
    else if (bits & WIFI_FAIL_BIT)
    {
        ESP_LOGI("Network", "Failed to connect to SSID:%s, password:%s",
                 ssid, password);
    }
    else
    {
        ESP_LOGE("Network", "UNEXPECTED EVENT");
    }
}

static void init_network(void)
{
    init_network_adapter();
    init_wifi();

    start_web_server();
}

static void IRAM_ATTR gpio_isr_handler(void *arg)
{
    uint32_t gpio_num = (uint32_t)arg;
    xQueueSendFromISR(gpio_evt_queue, &gpio_num, NULL);
}

static void gpio_task_example(void *arg)
{
    uint32_t io_num;
    for (;;)
    {
        if (xQueueReceive(gpio_evt_queue, &io_num, portMAX_DELAY))
        {
            digital_data_value = gpio_get_level(io_num) > 0 ? 1 : 0;
            printf("GPIO[%d] intr, val: %d\n", io_num, digital_data_value);
        }
    }
}

static gpio_num_t get_gpio_pin()
{
    int8_t gpio = GPIO_NUM_8;
    esp_err_t err = nvs_get_i8(settings_handle, AppSetting[DoorSensorChannel], &gpio);
    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        ESP_LOGI("Door sensor", "Pin not set, using default value!");
    }

    ESP_LOGI("Door sensor", "Pin used: %d", gpio);

    return (gpio_num_t)gpio;
}

static void init_gpio(void)
{
    gpio_num_t gpio_pin = get_gpio_pin();

    // zero-initialize the config structure.
    gpio_config_t io_conf = {};
    // interrupt of rising and falling edge
    io_conf.intr_type = GPIO_INTR_ANYEDGE;
    // bit mask of the pins
    io_conf.pin_bit_mask = (1ULL << gpio_pin);
    // disable pull-down mode
    io_conf.pull_down_en = 0;
    // enable pull-up mode
    io_conf.pull_up_en = 1;
    // set as input mode
    io_conf.mode = GPIO_MODE_INPUT;
    // configure GPIO with the given settings
    gpio_config(&io_conf);

    // create a queue to handle gpio event from isr
    gpio_evt_queue = xQueueCreate(10, sizeof(uint32_t));
    // start gpio task
    xTaskCreate(gpio_task_example, "gpio_task_example", 2048, NULL, 10, NULL);

    // install gpio isr service
    gpio_install_isr_service(0);
    // hook isr handler for specific gpio pin
    gpio_isr_handler_add(gpio_pin, gpio_isr_handler, (void *)gpio_pin);

    printf("Minimum free heap size: %d bytes\n", esp_get_minimum_free_heap_size());
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        // NVS partition was truncated and needs to be erased
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_LOGI("NVS", "NVS flash initialized.");

    ESP_LOGI("NVS", "Opening NVS handle...");
    err = nvs_open(SETTINGS_STORAGE, NVS_READWRITE, &settings_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE("NVS", "Error (%s) opening NVS handle!", esp_err_to_name(err));
        return;
    }
    ESP_LOGI("NVS", "NVS handle opened!");

    err = update_reset_counter();
    if (err != ESP_OK)
    {
        ESP_LOGE("APP", "Error (%s) updating reset counter!", esp_err_to_name(err));
        return;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_LOGI("NVS", "TCP/IP stack initialized.");
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_LOGI("NVS", "Event loop created.");

    init_light_sensor();
    init_gpio();
    init_network();
}
