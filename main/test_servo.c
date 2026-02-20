#include <stdio.h>
#include <string.h>
#include <stdlib.h>
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
#include "cJSON.h"

#define SERVO_PIN           15          // GPIO pin for servo control
#define SERVO_TIMER         LEDC_TIMER_0
#define SERVO_CHANNEL       LEDC_CHANNEL_0
#define SERVO_FREQUENCY     50          // 50Hz for servo control
#define SERVO_RESOLUTION    LEDC_TIMER_13_BIT

// Default PWM values for servo positions
#define SERVO_DEFAULT_POSITION  (4096 * 1.5 / 20)  // 90 degrees (center position)
#define PWM_MIN_VALUE           (4096 * 0.5 / 20)  // Min PWM value (~0 degrees)
#define PWM_MAX_VALUE           (4096 * 2.5 / 20)  // Max PWM value (~180 degrees)

// WiFi configuration
#define WIFI_SSID       "pixel"
#define WIFI_PASSWORD   "12341234"
#define MAX_RETRY       5

static const char *TAG = "custom_pwm_feeder";
static TimerHandle_t servo_reset_timer = NULL;
static httpd_handle_t server = NULL;
static uint32_t default_pwm_position = SERVO_DEFAULT_POSITION;
static uint32_t current_pwm_value = SERVO_DEFAULT_POSITION;
static uint32_t feed_pwm_value = (4096 * 1.25 / 20); // Default feed position
static uint32_t reset_delay_ms = 2000; // Default reset delay in ms

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
        .duty = default_pwm_position,  // Default position
        .gpio_num = SERVO_PIN,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_sel = SERVO_TIMER,
    };
    ledc_channel_config(&ledc_channel);

    // Set servo to default position
    ledc_set_duty(LEDC_LOW_SPEED_MODE, SERVO_CHANNEL, default_pwm_position);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, SERVO_CHANNEL);
    current_pwm_value = default_pwm_position;
}

// Set servo position
static void set_servo_position(uint32_t duty)
{
    // Ensure duty is within valid range
    if (duty < PWM_MIN_VALUE) {
        duty = PWM_MIN_VALUE;
    } else if (duty > PWM_MAX_VALUE) {
        duty = PWM_MAX_VALUE;
    }

    ledc_set_duty(LEDC_LOW_SPEED_MODE, SERVO_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, SERVO_CHANNEL);
    current_pwm_value = duty;

    ESP_LOGI(TAG, "Servo position set to PWM value: %lu", (unsigned long)duty);
}

// Timer callback to reset servo to default position
static void servo_reset_timer_callback(TimerHandle_t xTimer)
{
    ESP_LOGI(TAG, "Resetting servo to default position (PWM: %lu)", (unsigned long)default_pwm_position);
    set_servo_position(default_pwm_position);
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

// Feed handler - activates the servo when requested
static esp_err_t feed_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Moving servo to feed position (PWM: %lu)", (unsigned long)feed_pwm_value);

    // Move servo to feed position
    set_servo_position(feed_pwm_value);

    // Start timer to reset servo after delay
    xTimerChangePeriod(servo_reset_timer, pdMS_TO_TICKS(reset_delay_ms), 100);
    xTimerReset(servo_reset_timer, 100);

    // Prepare response
    char resp[100];
    snprintf(resp, sizeof(resp), "Feeding started with PWM %lu, will reset in %lu ms",
             (unsigned long)feed_pwm_value, (unsigned long)reset_delay_ms);

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, resp, strlen(resp));

    return ESP_OK;
}

// Set PWM value handler
static esp_err_t set_pwm_handler(httpd_req_t *req)
{
    char content[100];
    size_t recv_size = MIN(req->content_len, sizeof(content) - 1);

    int ret = httpd_req_recv(req, content, recv_size);
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    content[recv_size] = '\0';

    cJSON *root = cJSON_Parse(content);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *pwm_value_json = cJSON_GetObjectItem(root, "pwm");
    cJSON *position_type_json = cJSON_GetObjectItem(root, "position");
    cJSON *delay_json = cJSON_GetObjectItem(root, "delay");

    esp_err_t err = ESP_OK;
    char resp[100];

    if (pwm_value_json != NULL && cJSON_IsNumber(pwm_value_json)) {
        uint32_t pwm_value = (uint32_t)pwm_value_json->valuedouble;

        // Check if this is for default or feed position
        if (position_type_json != NULL && cJSON_IsString(position_type_json)) {
            if (strcmp(position_type_json->valuestring, "default") == 0) {
                default_pwm_position = pwm_value;
                set_servo_position(default_pwm_position);
                snprintf(resp, sizeof(resp), "Default position set to PWM: %lu", (unsigned long)default_pwm_position);
            } else if (strcmp(position_type_json->valuestring, "feed") == 0) {
                feed_pwm_value = pwm_value;
                snprintf(resp, sizeof(resp), "Feed position set to PWM: %lu", (unsigned long)feed_pwm_value);
            } else if (strcmp(position_type_json->valuestring, "current") == 0) {
                set_servo_position(pwm_value);
                snprintf(resp, sizeof(resp), "Current position set to PWM: %lu", (unsigned long)pwm_value);
            } else {
                snprintf(resp, sizeof(resp), "Unknown position type: %s", position_type_json->valuestring);
                err = ESP_FAIL;
            }
        } else {
            // If no position type specified, update current position
            set_servo_position(pwm_value);
            snprintf(resp, sizeof(resp), "Current position set to PWM: %lu", (unsigned long)pwm_value);
        }
    } else {
        snprintf(resp, sizeof(resp), "Missing or invalid PWM value");
        err = ESP_FAIL;
    }

    // Check if delay value was provided
    if (delay_json != NULL && cJSON_IsNumber(delay_json)) {
        reset_delay_ms = (uint32_t)delay_json->valuedouble;
        char delay_msg[50];
        snprintf(delay_msg, sizeof(delay_msg), ", reset delay set to %lu ms", (unsigned long)reset_delay_ms);
        strncat(resp, delay_msg, sizeof(resp) - strlen(resp) - 1);
    }

    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");

    cJSON *resp_json = cJSON_CreateObject();
    cJSON_AddStringToObject(resp_json, "message", resp);
    cJSON_AddNumberToObject(resp_json, "current_pwm", current_pwm_value);
    cJSON_AddNumberToObject(resp_json, "default_pwm", default_pwm_position);
    cJSON_AddNumberToObject(resp_json, "feed_pwm", feed_pwm_value);
    cJSON_AddNumberToObject(resp_json, "reset_delay_ms", reset_delay_ms);

    char *resp_str = cJSON_Print(resp_json);
    httpd_resp_send(req, resp_str, strlen(resp_str));
    free(resp_str);
    cJSON_Delete(resp_json);

    return err;
}

// Get current settings handler
static esp_err_t get_settings_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "current_pwm", current_pwm_value);
    cJSON_AddNumberToObject(root, "default_pwm", default_pwm_position);
    cJSON_AddNumberToObject(root, "feed_pwm", feed_pwm_value);
    cJSON_AddNumberToObject(root, "reset_delay_ms", reset_delay_ms);
    cJSON_AddNumberToObject(root, "min_pwm", PWM_MIN_VALUE);
    cJSON_AddNumberToObject(root, "max_pwm", PWM_MAX_VALUE);

    char *json_str = cJSON_Print(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));

    free(json_str);
    cJSON_Delete(root);

    return ESP_OK;
}

// HTTP GET handler serving the web page
static esp_err_t index_handler(httpd_req_t *req)
{
    const char* html = "<!DOCTYPE html>\n"
                      "<html>\n"
                      "<head>\n"
                      "    <title>Custom PWM Animal Feeder</title>\n"
                      "    <meta name='viewport' content='width=device-width, initial-scale=1'>\n"
                      "    <style>\n"
                      "        body { font-family: Arial, sans-serif; margin: 20px; line-height: 1.6; }\n"
                      "        h1 { color: #333; text-align: center; }\n"
                      "        .container { max-width: 800px; margin: 0 auto; padding: 20px; }\n"
                      "        .card { background: #f9f9f9; border-radius: 8px; padding: 20px; margin-bottom: 20px; box-shadow: 0 2px 5px rgba(0,0,0,0.1); }\n"
                      "        .button { background-color: #4CAF50; border: none; color: white; padding: 10px 20px;\n"
                      "                 text-align: center; display: inline-block; font-size: 16px; margin: 4px 2px;\n"
                      "                 cursor: pointer; border-radius: 4px; transition: background-color 0.3s; }\n"
                      "        .button:hover { background-color: #45a049; }\n"
                      "        .button-blue { background-color: #2196F3; }\n"
                      "        .button-blue:hover { background-color: #0b7dda; }\n"
                      "        .status { margin-top: 10px; padding: 10px; background-color: #f1f1f1; border-radius: 4px; }\n"
                      "        .slider { width: 100%; margin: 10px 0; }\n"
                      "        .control-group { margin-bottom: 15px; }\n"
                      "        label { display: inline-block; width: 150px; }\n"
                      "        input[type='number'] { width: 80px; padding: 5px; }\n"
                      "        .row { display: flex; flex-wrap: wrap; margin-bottom: 10px; }\n"
                      "        .col { flex: 1; padding: 0 10px; }\n"
                      "    </style>\n"
                      "</head>\n"
                      "<body>\n"
                      "    <div class='container'>\n"
                      "        <h1>Custom PWM Animal Feeder</h1>\n"
                      "        \n"
                      "        <div class='card'>\n"
                      "            <h2>Feed Control</h2>\n"
                      "            <button class='button' id='feedButton'>Feed Now</button>\n"
                      "            <div class='status' id='feedStatus'>Ready</div>\n"
                      "        </div>\n"
                      "        \n"
                      "        <div class='card'>\n"
                      "            <h2>Current Position</h2>\n"
                      "            <div class='row'>\n"
                      "                <div class='col'>\n"
                      "                    <div class='control-group'>\n"
                      "                        <label>PWM Value:</label>\n"
                      "                        <input type='number' id='currentPwm' min='205' max='410' value='307'>\n"
                      "                    </div>\n"
                      "                </div>\n"
                      "                <div class='col'>\n"
                      "                    <input type='range' id='currentSlider' class='slider' min='205' max='410' value='307'>\n"
                      "                </div>\n"
                      "            </div>\n"
                      "            <button class='button button-blue' id='setCurrentBtn'>Set Current Position</button>\n"
                      "        </div>\n"
                      "        \n"
                      "        <div class='card'>\n"
                      "            <h2>Default & Feed Positions</h2>\n"
                      "            <div class='row'>\n"
                      "                <div class='col'>\n"
                      "                    <div class='control-group'>\n"
                      "                        <label>Default PWM:</label>\n"
                      "                        <input type='number' id='defaultPwm' min='205' max='410' value='307'>\n"
                      "                    </div>\n"
                      "                </div>\n"
                      "                <div class='col'>\n"
                      "                    <input type='range' id='defaultSlider' class='slider' min='205' max='410' value='307'>\n"
                      "                </div>\n"
                      "            </div>\n"
                      "            <button class='button button-blue' id='setDefaultBtn'>Set Default Position</button>\n"
                      "            \n"
                      "            <div class='row' style='margin-top: 20px;'>\n"
                      "                <div class='col'>\n"
                      "                    <div class='control-group'>\n"
                      "                        <label>Feed PWM:</label>\n"
                      "                        <input type='number' id='feedPwm' min='205' max='410' value='256'>\n"
                      "                    </div>\n"
                      "                </div>\n"
                      "                <div class='col'>\n"
                      "                    <input type='range' id='feedSlider' class='slider' min='205' max='410' value='256'>\n"
                      "                </div>\n"
                      "            </div>\n"
                      "            <button class='button button-blue' id='setFeedBtn'>Set Feed Position</button>\n"
                      "        </div>\n"
                      "        \n"
                      "        <div class='card'>\n"
                      "            <h2>Timing</h2>\n"
                      "            <div class='control-group'>\n"
                      "                <label>Reset Delay (ms):</label>\n"
                      "                <input type='number' id='resetDelay' min='500' max='10000' value='2000'>\n"
                      "            </div>\n"
                      "            <button class='button button-blue' id='setDelayBtn'>Set Delay</button>\n"
                      "        </div>\n"
                      "        \n"
                      "        <div class='status' id='status'>System ready</div>\n"
                      "    </div>\n"
                      "\n"
                      "    <script>\n"
                      "        // Initialize with current settings\n"
                      "        window.onload = function() {\n"
                      "            fetchSettings();\n"
                      "            \n"
                      "            // Set up slider-input pairs\n"
                      "            setupSliderInputPair('current');\n"
                      "            setupSliderInputPair('default');\n"
                      "            setupSliderInputPair('feed');\n"
                      "        };\n"
                      "        \n"
                      "        function setupSliderInputPair(prefix) {\n"
                      "            const slider = document.getElementById(prefix + 'Slider');\n"
                      "            const input = document.getElementById(prefix + 'Pwm');\n"
                      "            \n"
                      "            slider.oninput = function() {\n"
                      "                input.value = this.value;\n"
                      "            };\n"
                      "            \n"
                      "            input.oninput = function() {\n"
                      "                slider.value = this.value;\n"
                      "            };\n"
                      "        }\n"
                      "        \n"
                      "        function fetchSettings() {\n"
                      "            fetch('/settings')\n"
                      "                .then(response => response.json())\n"
                      "                .then(data => {\n"
                      "                    // Update all input fields and sliders with current values\n"
                      "                    document.getElementById('currentPwm').value = data.current_pwm;\n"
                      "                    document.getElementById('currentSlider').value = data.current_pwm;\n"
                      "                    \n"
                      "                    document.getElementById('defaultPwm').value = data.default_pwm;\n"
                      "                    document.getElementById('defaultSlider').value = data.default_pwm;\n"
                      "                    \n"
                      "                    document.getElementById('feedPwm').value = data.feed_pwm;\n"
                      "                    document.getElementById('feedSlider').value = data.feed_pwm;\n"
                      "                    \n"
                      "                    document.getElementById('resetDelay').value = data.reset_delay_ms;\n"
                      "                    \n"
                      "                    // Update slider ranges\n"
                      "                    const sliders = document.querySelectorAll('.slider');\n"
                      "                    const inputs = document.querySelectorAll('input[type=\"number\"]');\n"
                      "                    \n"
                      "                    sliders.forEach(slider => {\n"
                      "                        slider.min = data.min_pwm;\n"
                      "                        slider.max = data.max_pwm;\n"
                      "                    });\n"
                      "                    \n"
                      "                    inputs.forEach(input => {\n"
                      "                        if (input.id !== 'resetDelay') {\n"
                      "                            input.min = data.min_pwm;\n"
                      "                            input.max = data.max_pwm;\n"
                      "                        }\n"
                      "                    });\n"
                      "                    \n"
                      "                    document.getElementById('status').innerHTML = 'Settings loaded';\n"
                      "                })\n"
                      "                .catch(error => {\n"
                      "                    document.getElementById('status').innerHTML = 'Error loading settings: ' + error;\n"
                      "                });\n"
                      "        }\n"
                      "        \n"
                      "        document.getElementById('feedButton').addEventListener('click', function() {\n"
                      "            document.getElementById('feedStatus').innerHTML = 'Feeding...';\n"
                      "            fetch('/feed')\n"
                      "                .then(response => response.text())\n"
                      "                .then(data => {\n"
                      "                    document.getElementById('feedStatus').innerHTML = data;\n"
                      "                    setTimeout(function() {\n"
                      "                        document.getElementById('feedStatus').innerHTML = 'Ready';\n"
                      "                    }, 3000);\n"
                      "                })\n"
                      "                .catch(error => {\n"
                      "                    document.getElementById('feedStatus').innerHTML = 'Error: ' + error;\n"
                      "                });\n"
                      "        });\n"
                      "        \n"
                      "        document.getElementById('setCurrentBtn').addEventListener('click', function() {\n"
                      "            const pwmValue = document.getElementById('currentPwm').value;\n"
                      "            setPwmValue(pwmValue, 'current');\n"
                      "        });\n"
                      "        \n"
                      "        document.getElementById('setDefaultBtn').addEventListener('click', function() {\n"
                      "            const pwmValue = document.getElementById('defaultPwm').value;\n"
                      "            setPwmValue(pwmValue, 'default');\n"
                      "        });\n"
                      "        \n"
                      "        document.getElementById('setFeedBtn').addEventListener('click', function() {\n"
                      "            const pwmValue = document.getElementById('feedPwm').value;\n"
                      "            setPwmValue(pwmValue, 'feed');\n"
                      "        });\n"
                      "        \n"
                      "        document.getElementById('setDelayBtn').addEventListener('click', function() {\n"
                      "            const delayValue = document.getElementById('resetDelay').value;\n"
                      "            setPwmValue(0, 'current', delayValue);\n"
                      "        });\n"
                      "        \n"
                      "        function setPwmValue(pwmValue, positionType, delayValue = null) {\n"
                      "            document.getElementById('status').innerHTML = 'Updating settings...';\n"
                      "            \n"
                      "            const data = {\n"
                      "                pwm: parseInt(pwmValue),\n"
                      "                position: positionType\n"
                      "            };\n"
                      "            \n"
                      "            if (delayValue !== null) {\n"
                      "                data.delay = parseInt(delayValue);\n"
                      "            }\n"
                      "            \n"
                      "            fetch('/set_pwm', {\n"
                      "                method: 'POST',\n"
                      "                headers: {\n"
                      "                    'Content-Type': 'application/json'\n"
                      "                },\n"
                      "                body: JSON.stringify(data)\n"
                      "            })\n"
                      "            .then(response => response.json())\n"
                      "            .then(data => {\n"
                      "                document.getElementById('status').innerHTML = data.message;\n"
                      "                \n"
                      "                // Update fields with returned values\n"
                      "                document.getElementById('currentPwm').value = data.current_pwm;\n"
                      "                document.getElementById('currentSlider').value = data.current_pwm;\n"
                      "                document.getElementById('defaultPwm').value = data.default_pwm;\n"
                      "                document.getElementById('defaultSlider').value = data.default_pwm;\n"
                      "                document.getElementById('feedPwm').value = data.feed_pwm;\n"
                      "                document.getElementById('feedSlider').value = data.feed_pwm;\n"
                      "                document.getElementById('resetDelay').value = data.reset_delay_ms;\n"
                      "            })\n"
                      "            .catch(error => {\n"
                      "                document.getElementById('status').innerHTML = 'Error: ' + error;\n"
                      "            });\n"
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
    config.max_uri_handlers = 10; // Increased max handlers

    // Feed endpoint URI handler
    httpd_uri_t feed = {
        .uri        = "/feed",
        .method     = HTTP_GET,
        .handler    = feed_handler,
        .user_ctx   = NULL
    };

    // Set PWM endpoint URI handler
    httpd_uri_t set_pwm = {
        .uri        = "/set_pwm",
        .method     = HTTP_POST,
        .handler    = set_pwm_handler,
        .user_ctx   = NULL
    };

    // Get settings endpoint URI handler
    httpd_uri_t get_settings = {
        .uri        = "/settings",
        .method     = HTTP_GET,
        .handler    = get_settings_handler,
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
        httpd_register_uri_handler(server, &set_pwm);
        httpd_register_uri_handler(server, &get_settings);
        ESP_LOGI(TAG, "HTTP server started");
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

    ESP_LOGI(TAG, "Custom PWM Animal Feeder starting...");

    // Initialize servo
    servo_init();

    // Create timer for resetting servo position
    servo_reset_timer = xTimerCreate("servo_reset_timer", pdMS_TO_TICKS(reset_delay_ms),
                                     pdFALSE, 0, servo_reset_timer_callback);

    // Initialize WiFi
    wifi_init_sta();

    // Start webserver
    start_webserver();

    ESP_LOGI(TAG, "System ready - connect to IP address displayed above");
}
