#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>
#include <sys/time.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_eth.h"
#include "esp_eth_driver.h"
#include "esp_timer.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"
#include "mqtt_client.h"
#include "esp_tls.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"

// Network, broker, and power meter credentials/addresses live in config.h,
// which is excluded from version control (see .gitignore).
// Copy config.h.example to config.h and fill in your own values.
#include "config.h"

static const char *TAG = "TELEMETRY_APP";

// ==========================================
// 1. TEST CONFIGURATION & PINOUT
// ==========================================
static volatile uint32_t g_sampling_rate_ms = 1000;

#define ETH_SPI_HOST        SPI2_HOST
#define PIN_NUM_MISO        19
#define PIN_NUM_MOSI        23
#define PIN_NUM_SCLK        18
#define PIN_NUM_CS          15
#define PIN_NUM_INT         4
#define PIN_NUM_RST         5

// ==========================================
// 2. ABB M1M MODBUS REGISTER MAP
// ==========================================
#define REG_VOLTAGE         0x5B02
#define REG_CURRENT         0x5B10
#define REG_FREQ            0x5B32
#define REG_ENERGY          0x5000

// ==========================================
// 3. DATA STRUCTURES
// ==========================================
typedef struct {
    float v, i, f, kwh;
    int64_t t_poll_start_us;
} sensor_data_t;

typedef struct {
    float delay_ms;
    float proc_time_ms;
    uint32_t ack_seq;
} ack_metrics_t;

typedef struct {
    int msg_id;
    int64_t t_poll_start_us;
    int64_t t_pub_start_us;
    uint32_t seq;
    bool used;
} pending_pub_t;

// ==========================================
// 4. GLOBAL VARIABLES & MUTEXES
// ==========================================
static ack_metrics_t g_last_ack = { .delay_ms = -1.0f, .proc_time_ms = -1.0f, .ack_seq = 0 };
static portMUX_TYPE ack_mux = portMUX_INITIALIZER_UNLOCKED;
static pending_pub_t pending[100];
static portMUX_TYPE pending_mux = portMUX_INITIALIZER_UNLOCKED;

static esp_mqtt_client_handle_t mqtt_client = NULL;
static QueueHandle_t sensor_queue = NULL;
static bool s_mqtt_connected = false;
static bool s_eth_connected = false;
static bool s_wifi_got_ip = false;
static volatile uint32_t g_modbus_fail = 0;

/* HELPERS */
static int64_t get_monotonic_us(void) { return esp_timer_get_time(); }

// BUG FIX: Discard stale entries (>30s) so they can't collide with a
// wrapped-around msg_id and get matched to the wrong publish.
static void pending_add(int msg_id, int64_t t_poll, int64_t t_pub, uint32_t seq) {
    portENTER_CRITICAL(&pending_mux);
    for (int i = 0; i < 100; i++) {
        if (!pending[i].used) {
            pending[i] = (pending_pub_t){ .msg_id = msg_id, .t_poll_start_us = t_poll, .t_pub_start_us = t_pub, .seq = seq, .used = true };
            portEXIT_CRITICAL(&pending_mux);
            return;
        }
    }
    portEXIT_CRITICAL(&pending_mux);
}

static bool pending_take(int msg_id, int64_t *t_poll, int64_t *t_pub, uint32_t *seq) {
    bool ok = false;
    int64_t now = get_monotonic_us();
    portENTER_CRITICAL(&pending_mux);
    for (int i = 0; i < 100; i++) {
        if (pending[i].used && pending[i].msg_id == msg_id &&
            (now - pending[i].t_pub_start_us) < 30000000 /* reject stale slot >30s */) {
            *t_poll = pending[i].t_poll_start_us; *t_pub = pending[i].t_pub_start_us; *seq = pending[i].seq;
            pending[i].used = false; ok = true; break;
        }
    }
    portEXIT_CRITICAL(&pending_mux);
    return ok;
}

static int recv_exact(int sock, uint8_t *buf, int len) {
    int got = 0;
    while (got < len) {
        int r = recv(sock, buf + got, len - got, 0);
        if (r <= 0) return -1;
        got += r;
    }
    return got;
}

static esp_err_t modbus_read_robust(int sock, uint16_t addr, uint16_t count, uint16_t *out) {
    uint8_t tx[12], rx[260];
    static uint16_t tid = 0; tid++;
    tx[0] = tid >> 8; tx[1] = tid & 0xFF;
    tx[2] = 0; tx[3] = 0; tx[4] = 0; tx[5] = 6;
    tx[6] = MODBUS_UNIT_ID; tx[7] = 0x03;
    tx[8] = addr >> 8; tx[9] = addr & 0xFF;
    tx[10] = count >> 8; tx[11] = count & 0xFF;

    if (send(sock, tx, 12, 0) != 12) return ESP_FAIL;
    int expected = 9 + (count * 2);
    if (recv_exact(sock, rx, expected) != expected) return ESP_FAIL;
    if (rx[0] != tx[0] || rx[1] != tx[1]) return ESP_FAIL;
    if (rx[7] & 0x80) return ESP_FAIL;
    for (int i = 0; i < count; i++) out[i] = ((uint16_t)rx[9 + i*2] << 8) | rx[10 + i*2];
    return ESP_OK;
}

/* EVENT HANDLERS */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            s_mqtt_connected = true;
            ESP_LOGI(TAG, "MQTT Connected. Subscribing to config...");
            esp_mqtt_client_subscribe(mqtt_client, TOPIC_CONFIG, 1);
            break;

        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "New Config Received on %.*s", event->topic_len, event->topic);
            cJSON *root = cJSON_ParseWithLength(event->data, event->data_len);
            if (root) {
                cJSON *ms = cJSON_GetObjectItem(root, "sampling_ms");
                if (cJSON_IsNumber(ms)) {
                    g_sampling_rate_ms = ms->valueint;
                    ESP_LOGI(TAG, "Updated Sampling Rate: %lu ms", (unsigned long)g_sampling_rate_ms);
                }
                cJSON_Delete(root);
            }
            break;

        case MQTT_EVENT_DISCONNECTED:
            s_mqtt_connected = false;
            break;

        case MQTT_EVENT_PUBLISHED: {
            int64_t t_poll, t_pub; uint32_t seq;
            if (pending_take(event->msg_id, &t_poll, &t_pub, &seq)) {
                int64_t now = get_monotonic_us();
                ack_metrics_t ack = {
                    .delay_ms = (float)(now - t_poll) / 1000.0f,
                    .proc_time_ms = (float)(now - t_pub) / 1000.0f,
                    .ack_seq = seq
                };
                portENTER_CRITICAL(&ack_mux);
                g_last_ack = ack;
                portEXIT_CRITICAL(&ack_mux);
            }
            break;
        }

        default:
            break;
    }
}

static void eth_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (id == ETHERNET_EVENT_CONNECTED) s_eth_connected = true;
    else if (id == ETHERNET_EVENT_DISCONNECTED) s_eth_connected = false;
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_got_ip = false;
        ESP_LOGW(TAG, "WiFi Disconnected. Reconnecting...");
        esp_wifi_connect();
    }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_wifi_got_ip = true;
        ESP_LOGI(TAG, "WiFi Connection Ready");
    }
}

/* HARDWARE INITIALIZATION */
static void init_ethernet(void) {
    spi_bus_config_t buscfg = { .miso_io_num = PIN_NUM_MISO, .mosi_io_num = PIN_NUM_MOSI, .sclk_io_num = PIN_NUM_SCLK, .quadwp_io_num = -1, .quadhd_io_num = -1 };
    spi_bus_initialize(ETH_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    spi_device_interface_config_t devcfg = { .clock_speed_hz = 10*1000*1000, .spics_io_num = PIN_NUM_CS, .queue_size = 20 };
    eth_w5500_config_t w5500_config = ETH_W5500_DEFAULT_CONFIG(ETH_SPI_HOST, &devcfg);
    w5500_config.int_gpio_num = PIN_NUM_INT;
    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500_config, &((eth_mac_config_t)ETH_MAC_DEFAULT_CONFIG()));
    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&((eth_phy_config_t)ETH_PHY_DEFAULT_CONFIG()));
    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth_handle = NULL; esp_eth_driver_install(&eth_config, &eth_handle);
    esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL);
    esp_netif_inherent_config_t netif_cfg = ESP_NETIF_INHERENT_DEFAULT_ETH();
    esp_netif_config_t cfg = { .base = &netif_cfg, .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH };
    esp_netif_t *eth_netif = esp_netif_new(&cfg);
    esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handle));
    esp_netif_ip_info_t ip_info; IP4_ADDR(&ip_info.ip, 192, 168, 10, 1); IP4_ADDR(&ip_info.gw, 192, 168, 10, 1); IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
    esp_netif_dhcpc_stop(eth_netif); esp_netif_set_ip_info(eth_netif, &ip_info); esp_eth_start(eth_handle);
}

static void init_wifi(void) {
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT(); esp_wifi_init(&cfg);
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);
    wifi_config_t wifi_config = { .sta = { .ssid = WIFI_SSID, .password = WIFI_PASS, .threshold.authmode = WIFI_AUTH_WPA2_PSK } };
    esp_wifi_set_mode(WIFI_MODE_STA); esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_netif_dns_info_t dns; inet_pton(AF_INET, "8.8.8.8", &dns.ip.u_addr.ip4);
    esp_netif_set_dns_info(sta_netif, ESP_NETIF_DNS_MAIN, &dns);
}

static void init_mqtt(void) {
    const esp_mqtt_client_config_t mqtt_cfg = { .broker.address.uri = MQTT_BROKER_URI, .credentials.username = MQTT_USERNAME, .credentials.authentication.password = MQTT_PASSWORD, .broker.verification.crt_bundle_attach = esp_crt_bundle_attach, .task.priority = 6 };
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
}

// ==========================================
// 5. MODBUS TASK: CORE 1
// ==========================================
void task_modbus_core1(void *pvParameters) {
    sensor_data_t data; int sock = -1; uint16_t regs[10];
    vTaskDelay(pdMS_TO_TICKS(2000));

    while(1) {
        int64_t t_start = get_monotonic_us();
        bool ok = false;
        if (s_eth_connected) {
            if (sock < 0) {
                sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
                struct sockaddr_in dest = { .sin_family = AF_INET, .sin_port = htons(PM_PORT) };
                inet_pton(AF_INET, PM_IP_ADDR, &dest.sin_addr.s_addr);
                struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
                setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                if (connect(sock, (struct sockaddr *)&dest, sizeof(dest)) != 0) { close(sock); sock = -1; }
                else { ESP_LOGI(TAG, "Modbus Connected to ABB PM"); }
            }
            if (sock >= 0) {
                data.t_poll_start_us = get_monotonic_us();
                int fail_count = 0;
                if (modbus_read_robust(sock, REG_VOLTAGE, 2, regs) == ESP_OK)
                    data.v = (float)(((uint32_t)regs[0] << 16) | regs[1]) / 10.0f;
                else fail_count++;
                if (modbus_read_robust(sock, REG_CURRENT, 2, regs) == ESP_OK)
                    data.i = (float)(((uint32_t)regs[0] << 16) | regs[1]) / 100.0f;
                else fail_count++;
                if (modbus_read_robust(sock, REG_FREQ, 1, regs) == ESP_OK)
                    data.f = (float)regs[0] / 100.0f;
                else fail_count++;
                if (modbus_read_robust(sock, REG_ENERGY, 4, regs) == ESP_OK) {
                    uint64_t raw = ((uint64_t)regs[0] << 48) | ((uint64_t)regs[1] << 32) | ((uint64_t)regs[2] << 16) | regs[3];
                    data.kwh = (float)raw / 100.0f;
                } else fail_count++;

                if (fail_count >= 3) { close(sock); sock = -1; g_modbus_fail++; }
                else ok = true;
            }
        }
        if (ok) xQueueSend(sensor_queue, &data, pdMS_TO_TICKS(10));

        int wait = (int)g_sampling_rate_ms - (int)((get_monotonic_us() - t_start) / 1000);
        vTaskDelay(pdMS_TO_TICKS(wait < 10 ? 10 : wait));
    }
}

// ==========================================
// 6. CPU UTILIZATION TASK (Liu & Layland method,
//    sliding-window delta, 64-bit counters, per-core ID)
// ==========================================
static void task_print_stats(void *pvParameters) {
    static char json[256];
    static uint64_t prev_runtime[20] = {0};
    static char prev_names[20][16] = {{0}};
    static uint64_t prev_total = 0;

    vTaskDelay(pdMS_TO_TICKS(5000));

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        if (!s_mqtt_connected) continue;

        TaskStatus_t *task_array;
        UBaseType_t task_count;
        uint64_t total_runtime;

        task_count = uxTaskGetNumberOfTasks();
        task_array = pvPortMalloc(task_count * sizeof(TaskStatus_t));
        if (!task_array) continue;

        task_count = uxTaskGetSystemState(task_array, task_count, &total_runtime);
        if (total_runtime == 0 || total_runtime == prev_total) {
            vPortFree(task_array); continue;
        }

        uint64_t delta_total = total_runtime - prev_total;

        for (int i = 0; i < task_count && i < 20; i++) {
            uint64_t prev = 0;
            for (int j = 0; j < 20; j++) {
                if (strncmp(prev_names[j], task_array[i].pcTaskName, 15) == 0) {
                    prev = prev_runtime[j]; break;
                }
            }

            uint64_t delta_task = task_array[i].ulRunTimeCounter - prev;
            float util = (float)delta_task / (float)delta_total * 100.0f;

            int len = snprintf(json, sizeof(json),
                "{\"task\":\"%s\",\"util\":%.2f,\"core\":%d,\"architecture\":\"parallel\"}",
                task_array[i].pcTaskName,
                util,
                (int)task_array[i].xCoreID);

            esp_mqtt_client_publish(mqtt_client, TOPIC_STATS, json, len, 0, 0);

            for (int j = 0; j < 20; j++) {
                if (strncmp(prev_names[j], task_array[i].pcTaskName, 15) == 0 || prev_names[j][0] == '\0') {
                    strncpy(prev_names[j], task_array[i].pcTaskName, 15);
                    prev_runtime[j] = task_array[i].ulRunTimeCounter;
                    break;
                }
            }
        }

        prev_total = total_runtime;
        vPortFree(task_array);
    }
}

// ==========================================
// 7. MAIN LOOP: CORE 0
// ==========================================
void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    gpio_install_isr_service(0);

    init_ethernet();
    init_wifi();
    sensor_queue = xQueueCreate(100, sizeof(sensor_data_t));

    xTaskCreatePinnedToCore(task_modbus_core1, "Modbus_C1", 8192, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(task_print_stats, "Stats", 4096, NULL, 1, NULL, 0);

    init_mqtt();

    sensor_data_t recv; char p[512]; uint32_t seq = 0;

    while (1) {
        if (xQueueReceive(sensor_queue, &recv, portMAX_DELAY) == pdTRUE) {
            seq++;
            if (s_mqtt_connected) {
                ack_metrics_t ack;
                portENTER_CRITICAL(&ack_mux); ack = g_last_ack; portEXIT_CRITICAL(&ack_mux);

                int actual_len = snprintf(p, sizeof(p),
                    "{\"seq\":%lu,\"v\":%.2f,\"i\":%.2f,\"f\":%.2f,\"kwh\":%.2f,"
                    "\"ack_seq\":%lu,\"delay\":%.3f,\"proc\":%.3f,\"architecture\":\"parallel\"}",
                    (unsigned long)seq, recv.v, recv.i, recv.f, recv.kwh,
                    (unsigned long)ack.ack_seq, ack.delay_ms, ack.proc_time_ms);

                int64_t t_pub = get_monotonic_us();
                int msg_id = esp_mqtt_client_publish(mqtt_client, TOPIC_PM_RAW, p, actual_len, 1, 0);
                if (msg_id > 0) pending_add(msg_id, recv.t_poll_start_us, t_pub, seq);
            }
        }
    }
}
