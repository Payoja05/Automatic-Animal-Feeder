#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "driver/ledc.h"
#include "esp_http_server.h"

#define SERVO_PIN           15       // GPIO pin for servo control
#define SERVO_TIMER         LEDC_TIMER_0
#define SERVO_CHANNEL       LEDC_CHANNEL_0
#define SERVO_FREQUENCY     50          // 50Hz for servo control
#define SERVO_RESOLUTION    LEDC_TIMER_13_BIT

// Pulse width values for servo positions
#define SERVO_90_DEGREES    (4096 * 0.5 / 20)  // 90 degrees (center position)
#define SERVO_75_DEGREES    (4096 * 1.5 / 20) // 75 degrees (rotate)

// WiFi configuration
#define WIFI_SSID       "pet_feeder"
#define WIFI_PASSWORD   "12341234"
#define MAX_RETRY       5

static const char *TAG = "automatic_feeder";
static TimerHandle_t servo_reset_timer = NULL;
static TimerHandle_t auto_feed_timer = NULL;
static httpd_handle_t server = NULL;
static int auto_feed_interval = 0; // In minutes, 0 means disabled

// Initialize servo motor
static void servo_init(void)
{
    // Configure LEDC timer for servo control
    ledc_timer_config_t ledc_timer = {
        .duty_resolution = SERVO_RESOLUTION,
        .freq_hz = SERVO_FREQUENCY,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = SERVO_TIMER,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&ledc_timer);

    // Configure LEDC channel for servo control
    ledc_channel_config_t ledc_channel = {
        .channel = SERVO_CHANNEL,
        .duty = SERVO_90_DEGREES,  // Default 90 degrees
        .gpio_num = SERVO_PIN,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_sel = SERVO_TIMER,
    };
    ledc_channel_config(&ledc_channel);

    // Set servo to default position (90 degrees)
    ledc_set_duty(LEDC_LOW_SPEED_MODE, SERVO_CHANNEL, SERVO_90_DEGREES);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, SERVO_CHANNEL);
}

// Set servo position
static void set_servo_position(uint32_t duty)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, SERVO_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, SERVO_CHANNEL);
}

// Execute feeding action
static void do_feed(void)
{
    ESP_LOGI(TAG, "Moving servo to 75 degrees for feeding");

    // Move servo to 75 degrees
    set_servo_position(SERVO_75_DEGREES);

    // Start timer to reset servo after 5 seconds
    xTimerReset(servo_reset_timer, 100);
}

// Timer callback to reset servo to default position
static void servo_reset_timer_callback(TimerHandle_t xTimer)
{
    ESP_LOGI(TAG, "Resetting servo to 90 degrees (default position)");
    set_servo_position(SERVO_90_DEGREES);
}

// Automatic feeding timer callback
static void auto_feed_timer_callback(TimerHandle_t xTimer)
{
    ESP_LOGI(TAG, "Auto feeding triggered");
    do_feed();
}

// WiFi event handler
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        static int s_retry_num = 0;
        if (s_retry_num < MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retrying to connect to the AP");
        } else {
            ESP_LOGI(TAG, "Failed to connect to the AP");
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

// Initialize WiFi as station
static void wifi_init_sta(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_sta finished");
}

// Update timer interval
static void update_auto_feed_timer(int minutes)
{
    auto_feed_interval = minutes;

    if (auto_feed_timer != NULL) {
        xTimerStop(auto_feed_timer, 0);

        if (minutes > 0) {
            // Convert minutes to ticks
            uint32_t timer_period = pdMS_TO_TICKS(minutes * 60 * 1000);

            xTimerChangePeriod(auto_feed_timer, timer_period, 0);
            xTimerStart(auto_feed_timer, 0);

            ESP_LOGI(TAG, "Auto feeding timer set to %d minutes", minutes);
        } else {
            ESP_LOGI(TAG, "Auto feeding timer disabled");
        }
    }
}

// Feed handler - activates the servo when requested
static esp_err_t feed_handler(httpd_req_t *req)
{
    do_feed();

    // Prepare response
    const char *resp = "Feeding started, servo will reset to 90 degrees in 5 seconds";
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, resp, strlen(resp));

    return ESP_OK;
}

// Timer setting handler
static esp_err_t set_timer_handler(httpd_req_t *req)
{
    char buf[50];
    int ret, minutes;

    // Get query string
    ret = httpd_req_get_url_query_str(req, buf, sizeof(buf));
    if (ret != ESP_OK) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    char param[32];
    ret = httpd_query_key_value(buf, "minutes", param, sizeof(param));
    if (ret != ESP_OK) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    minutes = atoi(param);
    update_auto_feed_timer(minutes);

    char resp[100];
    if (minutes > 0) {
        snprintf(resp, sizeof(resp), "Auto feeding timer set to %d minutes", minutes);
    } else {
        snprintf(resp, sizeof(resp), "Auto feeding timer disabled");
    }

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, resp, strlen(resp));

    return ESP_OK;
}

// Get current timer status
static esp_err_t get_timer_status_handler(httpd_req_t *req)
{
    char resp[50];
    snprintf(resp, sizeof(resp), "%d", auto_feed_interval);

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, resp, strlen(resp));

    return ESP_OK;
}

// HTTP GET handler serving the web page
static esp_err_t index_handler(httpd_req_t *req)
{
    const char* html = "<!DOCTYPE html>\n"
                      "<html>\n"
                      "<head>\n"
                      "    <title>Animal Feeder Control</title>\n"
                      "    <meta name='viewport' content='width=device-width, initial-scale=1'>\n"
                      "    <style>\n"
                      "        body { font-family: Arial, sans-serif; text-align: center; margin-top: 50px; }\n"
                      "        .button { background-color: #4CAF50; border: none; color: white; padding: 15px 32px;\n"
                      "                 text-align: center; display: inline-block; font-size: 16px; margin: 4px 2px;\n"
                      "                 cursor: pointer; border-radius: 8px; }\n"
                      "        .status { margin-top: 20px; }\n"
                      "        .timer-section { margin-top: 40px; padding: 20px; border: 1px solid #ddd; border-radius: 8px; }\n"
                      "        select, button { padding: 10px; margin: 10px; }\n"
                      "    </style>\n"
                      "</head>\n"
                      "<body>\n"
                      "    <h1>Automatic Animal Feeder</h1>\n"
                      "    <button class='button' id='feedButton'>Feed Now</button>\n"
                      "    <div class='status' id='status'>Ready</div>\n"
                      "\n"
                      "    <div class='timer-section'>\n"
                      "        <h2>Auto Feeding Timer</h2>\n"
                      "        <select id='timerSelect'>\n"
                      "            <option value='0'>Disabled</option>\n"
                      "            <option value='30'>30 minutes</option>\n"
                      "            <option value='60'>1 hour</option>\n"
                      "            <option value='120'>2 hours</option>\n"
                      "            <option value='180'>3 hours</option>\n"
                      "            <option value='240'>4 hours</option>\n"
                      "            <option value='360'>6 hours</option>\n"
                      "            <option value='720'>12 hours</option>\n"
                      "            <option value='1440'>24 hours</option>\n"
                      "        </select>\n"
                      "        <button id='setTimerButton'>Set Timer</button>\n"
                      "        <div id='timerStatus'>Timer not set</div>\n"
                      "    </div>\n"
                      "\n"
                      "    <script>\n"
                      "        // Manual feed button\n"
                      "        document.getElementById('feedButton').addEventListener('click', function() {\n"
                      "            document.getElementById('status').innerHTML = 'Feeding...';\n"
                      "            fetch('/feed')\n"
                      "                .then(response => response.text())\n"
                      "                .then(data => {\n"
                      "                    document.getElementById('status').innerHTML = data;\n"
                      "                    setTimeout(function() {\n"
                      "                        document.getElementById('status').innerHTML = 'Ready';\n"
                      "                    }, 3000);\n"
                      "                })\n"
                      "                .catch(error => {\n"
                      "                    document.getElementById('status').innerHTML = 'Error: ' + error;\n"
                      "                });\n"
                      "        });\n"
                      "\n"
                      "        // Get current timer status on page load\n"
                      "        window.addEventListener('load', function() {\n"
                      "            fetch('/get_timer')\n"
                      "                .then(response => response.text())\n"
                      "                .then(data => {\n"
                      "                    const minutes = parseInt(data);\n"
                      "                    document.getElementById('timerSelect').value = minutes;\n"
                      "                    updateTimerStatus(minutes);\n"
                      "                })\n"
                      "                .catch(error => {\n"
                      "                    console.error('Error fetching timer status:', error);\n"
                      "                });\n"
                      "        });\n"
                      "\n"
                      "        // Set timer button\n"
                      "        document.getElementById('setTimerButton').addEventListener('click', function() {\n"
                      "            const minutes = document.getElementById('timerSelect').value;\n"
                      "            fetch('/set_timer?minutes=' + minutes)\n"
                      "                .then(response => response.text())\n"
                      "                .then(data => {\n"
                      "                    document.getElementById('status').innerHTML = data;\n"
                      "                    updateTimerStatus(minutes);\n"
                      "                    setTimeout(function() {\n"
                      "                        document.getElementById('status').innerHTML = 'Ready';\n"
                      "                    }, 3000);\n"
                      "                })\n"
                      "                .catch(error => {\n"
                      "                    document.getElementById('status').innerHTML = 'Error: ' + error;\n"
                      "                });\n"
                      "        });\n"
                      "\n"
                      "        function updateTimerStatus(minutes) {\n"
                      "            if (minutes > 0) {\n"
                      "                let timeText = minutes + ' minutes';\n"
                      "                if (minutes == 60) timeText = '1 hour';\n"
                      "                else if (minutes > 60) {\n"
                      "                    const hours = minutes / 60;\n"
                      "                    if (hours === Math.floor(hours)) {\n"
                      "                        timeText = hours + ' hours';\n"
                      "                    }\n"
                      "                }\n"
                      "                document.getElementById('timerStatus').innerHTML = 'Auto feeding every ' + timeText;\n"
                      "            } else {\n"
                      "                document.getElementById('timerStatus').innerHTML = 'Auto feeding disabled';\n"
                      "            }\n"
                      "        }\n"
                      "    </script>\n"
                      "</body>\n"
                      "</html>";

    httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Start HTTP server
static httpd_handle_t start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    // Feed endpoint URI handler
    httpd_uri_t feed = {
        .uri        = "/feed",
        .method     = HTTP_GET,
        .handler    = feed_handler,
        .user_ctx   = NULL
    };

    // Set timer endpoint URI handler
    httpd_uri_t set_timer = {
        .uri        = "/set_timer",
        .method     = HTTP_GET,
        .handler    = set_timer_handler,
        .user_ctx   = NULL
    };

    // Get timer status endpoint URI handler
    httpd_uri_t get_timer = {
        .uri        = "/get_timer",
        .method     = HTTP_GET,
        .handler    = get_timer_status_handler,
        .user_ctx   = NULL
    };

    // Main page URI handler
    httpd_uri_t index = {
        .uri        = "/",
        .method     = HTTP_GET,
        .handler    = index_handler,
        .user_ctx   = NULL
    };

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_register_uri_handler(server, &index);
        httpd_register_uri_handler(server, &feed);
        httpd_register_uri_handler(server, &set_timer);
        httpd_register_uri_handler(server, &get_timer);
        return server;
    }

    ESP_LOGI(TAG, "Error starting server!");
    return NULL;
}

void app_main(void)
{
    // Initialize NVS (needed for WiFi)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "Automatic Animal Feeder starting...");

    // Initialize servo
    servo_init();

    // Create timer for resetting servo position
    servo_reset_timer = xTimerCreate("servo_reset_timer", pdMS_TO_TICKS(5000),
                                     pdFALSE, 0, servo_reset_timer_callback);

    // Create auto feeding timer (initially stopped)
    auto_feed_timer = xTimerCreate("auto_feed_timer", pdMS_TO_TICKS(60 * 60 * 1000), // Default to 1 hour
                                  pdTRUE, 0, auto_feed_timer_callback);

    // Initialize WiFi
    wifi_init_sta();

    // Start webserver
    start_webserver();

    ESP_LOGI(TAG, "System ready - connect to IP address displayed above");
}
