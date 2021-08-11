/* 
** WiFi Enable Train controller
** Jason Fry
** Summer 2021
*/
#include <string.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "driver/ledc.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>

#define CONFIG_EXAMPLE_IPV4 y;

static const char *TAG = "tcp_example";

#define MIN_FADE_RATE	10	// minimum number of miliseconds per % change in duty cycle
static int cur_duty = 0;
static int set_duty(int duty, int time);
static int get_duty(void);

static int tcp_server_send(int sock, char *buf);

static void tcp_server_talk(int sock);

#define PORT                        3333
#define KEEPALIVE_IDLE              5
#define KEEPALIVE_INTERVAL          5
#define KEEPALIVE_COUNT             3
static void tcp_server_task(void *pvParameter);

#define LEDC_LS_TIMER          LEDC_TIMER_1
#define LEDC_LS_MODE           LEDC_LOW_SPEED_MODE
#define LEDC_LS_CH0_GPIO       (1)
#define LEDC_LS_CH0_CHANNEL    LEDC_CHANNEL_0
#define LEDC_LS_CH1_GPIO       (2)
#define LEDC_LS_CH1_CHANNEL    LEDC_CHANNEL_1
static void my_ledc_init(void);

#define ESP_WIFI_SSID      ("ssid")		// replace with correct ssid
#define ESP_WIFI_PASS      ("password")	// replace with correct password
#define EXAMPLE_ESP_MAXIMUM_RETRY  5
void wifi_init_sta(void);

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
static int s_retry_num = 0;
static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data);

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;

void app_main(void){
    ESP_ERROR_CHECK(nvs_flash_init());
    my_ledc_init();
    wifi_init_sta();

#ifdef CONFIG_EXAMPLE_IPV4
    xTaskCreate(tcp_server_task, "tcp_server", 4096, (void*)AF_INET, 5, NULL);
#endif
#ifdef CONFIG_EXAMPLE_IPV6
    xTaskCreate(tcp_server_task, "tcp_server", 4096, (void*)AF_INET6, 5, NULL);
#endif
}

// sets duty when both initial and final duties are >= 0
// returns 0 when successful, non-zero otherwise
static int set_duty_pos(int duty, int time){
	int _duty = (255*duty)/100;	// scale duty to correct precision
	if(duty < 0){
		ESP_LOGE(TAG, "set_duty_neg incorrectly used (duty neg)");
		return ESP_ERR_INVALID_ARG;
	}
	if(cur_duty < 0){
		ESP_LOGE(TAG, "set_duty_neg incorrectly used (cur_duty neg");
		return ESP_ERR_INVALID_STATE;
	}
	int success = ledc_set_fade_with_time(LEDC_LS_MODE, LEDC_LS_CH0_CHANNEL, _duty, time);
	if(success != ESP_OK)
		return success;
    success = ledc_fade_start(LEDC_LS_MODE, LEDC_LS_CH0_CHANNEL, LEDC_FADE_WAIT_DONE);
    if(success == ESP_OK){
    	cur_duty = duty;
    }
    return success;
}

// sets duty when both initial and final duties are <= 0
// returns 0 when successful, non-zero otherwise
static int set_duty_neg(int duty, int time){
	int _duty = -1*(255*duty)/100;	// scale duty to correct precision
	if(duty > 0){
		ESP_LOGE(TAG, "set_duty_pos incorrectly used (duty pos)");
		return ESP_ERR_INVALID_ARG;
	}
	if(cur_duty > 0){
		ESP_LOGE(TAG, "set_duty_pos incorrectly used (cur_duty pos");
		return ESP_ERR_INVALID_STATE;
	}
	int success = ledc_set_fade_with_time(LEDC_LS_MODE, LEDC_LS_CH1_CHANNEL, _duty, time);
	if(success == ESP_OK)
		success = ledc_fade_start(LEDC_LS_MODE, LEDC_LS_CH1_CHANNEL, LEDC_FADE_WAIT_DONE);
    if(success == ESP_OK){
    	cur_duty = duty;
    }
    return success;
}

// sets motor duty cycle to 'duty' with a fade time of 'time' or 'MIN_DUTY_FADE_RATE'*change (whichever is larger)
// returns 0 when successful, non-zero otherwise
static int set_duty(int duty, int time){
	int duty_delta = duty > cur_duty ? duty - cur_duty : cur_duty - duty;
	int min_fade_time = duty == 0 ? 1 : duty_delta * MIN_FADE_RATE;
	int fade_time = time > min_fade_time ? time : min_fade_time;
	int success = 0;
	if(cur_duty >= 0 && duty >= 0){
		success = set_duty_pos(duty, fade_time);
	}
	else if(cur_duty <= 0 && duty <= 0){
		success = set_duty_neg(duty, fade_time);
	}
	else if(cur_duty > 0 && duty < 0){
		int pos_time = (((cur_duty*1000)/duty_delta)*fade_time)/1000;
		int neg_time  = (-1*((duty*1000)/duty_delta)*fade_time)/1000;
		success = set_duty_pos(0, pos_time);
		if(success == ESP_OK){
			success = set_duty_neg(duty, neg_time);
		}
	}
	else if(cur_duty < 0 && duty > 0){
		int neg_time = (-1*((cur_duty*1000)/duty_delta)*fade_time)/1000;
		int pos_time = (((duty*1000)/duty_delta)*fade_time)/1000;
		success |= set_duty_neg(0, neg_time);
		if(success == ESP_OK){
			success = set_duty_pos(duty, pos_time);
		}
	}

	if(success == ESP_OK)
		ESP_LOGI(TAG, "Set duty cycle to %d%%", duty);
	return success;
}

// returns current duty
static int get_duty(void){
	return cur_duty;
}

// send() can return less bytes than supplied length.
// Walk-around for robust implementation.
static int tcp_server_send(int sock, char *buf){
    int send_len = strlen(buf);
    int to_write = send_len;
    while (to_write > 0) {
        int written = send(sock, buf + (send_len - to_write), to_write, 0);
        if (written < 0) {
            ESP_LOGE(TAG, "Error occurred during sending: errno %d", errno);
            return errno;
        }
        to_write -= written;
        ESP_LOGI(TAG, "sent %d bytes", written);
        ESP_LOGI(TAG, "len == %d", send_len);
    }
    return 0;
}

enum {GET, SET};

// recivies and executes commands from client, returns response
static void tcp_server_talk(int sock){
    int recv_len;
    char rx_buffer[128];

    do {
        recv_len = recv(sock, rx_buffer, sizeof(rx_buffer) - 1, 0);
        if (recv_len < 0) {
            ESP_LOGE(TAG, "Error occurred during receiving: errno %d", errno);
        } else if (recv_len == 0) {
            ESP_LOGW(TAG, "Connection closed");
        } else {
            rx_buffer[recv_len] = 0; // Null-terminate whatever is received and treat it like a string
            ESP_LOGI(TAG, "Received %d bytes: %s", recv_len, rx_buffer);

            char *cmd_str = strtok(rx_buffer, " ");
            int cmd = atoi(cmd_str);
            char tx_buffer[32];
            switch(cmd){
				case GET:{	// get current duty cycle
					sprintf(tx_buffer, "%d", get_duty());
					tcp_server_send(sock, tx_buffer);
					break;
				}
				case SET:{	// set duty cycle to 'duty' with 'time' fade
					char *duty_str = strtok(NULL, " ");
					char *time_str = strtok(NULL, " ");
					int duty = atoi(duty_str);
					int time = atoi(time_str);
					set_duty(duty, time);
					break;
				}
            }
        }
    } while (recv_len > 0);
    set_duty(0, 0); // stop train when client disconnects
}

// initializes listening socket and accepts clients one at a time
// refers each client to tcp_server_talk
// closes client socket
// looped by xTask
static void tcp_server_task(void *pvParameters){
    char addr_str[128];
    int addr_family = (int)pvParameters;
    int ip_protocol = 0;
    int keepAlive = 1;
    int keepIdle = KEEPALIVE_IDLE;
    int keepInterval = KEEPALIVE_INTERVAL;
    int keepCount = KEEPALIVE_COUNT;
    struct sockaddr_storage dest_addr;

    if (addr_family == AF_INET) {
        struct sockaddr_in *dest_addr_ip4 = (struct sockaddr_in *)&dest_addr;
        dest_addr_ip4->sin_addr.s_addr = htonl(INADDR_ANY);
        dest_addr_ip4->sin_family = AF_INET;
        dest_addr_ip4->sin_port = htons(PORT);
        ip_protocol = IPPROTO_IP;
    }
#ifdef CONFIG_EXAMPLE_IPV6
    else if (addr_family == AF_INET6) {
        struct sockaddr_in6 *dest_addr_ip6 = (struct sockaddr_in6 *)&dest_addr;
        bzero(&dest_addr_ip6->sin6_addr.un, sizeof(dest_addr_ip6->sin6_addr.un));
        dest_addr_ip6->sin6_family = AF_INET6;
        dest_addr_ip6->sin6_port = htons(PORT);
        ip_protocol = IPPROTO_IPV6;
    }
#endif

    int listen_sock = socket(addr_family, SOCK_STREAM, ip_protocol);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }
    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#if defined(CONFIG_EXAMPLE_IPV4) && defined(CONFIG_EXAMPLE_IPV6)
    // Note that by default IPV6 binds to both protocols, it is must be disabled
    // if both protocols used at the same time (used in CI)
    setsockopt(listen_sock, IPPROTO_IPV6, IPV6_V6ONLY, &opt, sizeof(opt));
#endif

    ESP_LOGI(TAG, "Socket created");

    int err = bind(listen_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err != 0) {
        ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
        ESP_LOGE(TAG, "IPPROTO: %d", addr_family);
        goto CLEAN_UP;
    }
    ESP_LOGI(TAG, "Socket bound, port %d", PORT);

    err = listen(listen_sock, 1);
    if (err != 0) {
        ESP_LOGE(TAG, "Error occurred during listen: errno %d", errno);
        goto CLEAN_UP;
    }

    while (1) {

        ESP_LOGI(TAG, "Socket listening");

        struct sockaddr_storage source_addr; // Large enough for both IPv4 or IPv6
        socklen_t addr_len = sizeof(source_addr);
        int sock = accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);
        if (sock < 0) {
            ESP_LOGE(TAG, "Unable to accept connection: errno %d", errno);
            break;
        }

        // Set tcp keepalive option
        setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &keepAlive, sizeof(int));
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &keepIdle, sizeof(int));
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &keepInterval, sizeof(int));
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &keepCount, sizeof(int));
        // Convert ip address to string
        if (source_addr.ss_family == PF_INET) {
            inet_ntoa_r(((struct sockaddr_in *)&source_addr)->sin_addr, addr_str, sizeof(addr_str) - 1);
        }
#ifdef CONFIG_EXAMPLE_IPV6
        else if (source_addr.ss_family == PF_INET6) {
            inet6_ntoa_r(((struct sockaddr_in6 *)&source_addr)->sin6_addr, addr_str, sizeof(addr_str) - 1);
        }
#endif
        ESP_LOGI(TAG, "Socket accepted ip address: %s", addr_str);

        tcp_server_talk(sock);

        shutdown(sock, 0);
        close(sock);
    }

CLEAN_UP:
    close(listen_sock);
    vTaskDelete(NULL);
}

// initializes ledc to drive hbridge
static void my_ledc_init(void){
    /*
     * Prepare and set configuration of timers
     * that will be used by LED Controller
     */
    ledc_timer_config_t ledc_timer = {
        .duty_resolution = LEDC_TIMER_8_BIT, // resolution of PWM duty
        .freq_hz = 10000,                      // frequency of PWM signal
        .speed_mode = LEDC_LS_MODE,           // timer mode
        .timer_num = LEDC_LS_TIMER,            // timer index
        .clk_cfg = LEDC_AUTO_CLK,              // Auto select the source clock
    };
    // Set configuration of timer0 for high speed channels
    ledc_timer_config(&ledc_timer);
    /*
     * Prepare individual configuration
     * for each channel of LED Controller
     * by selecting:
     * - controller's channel number
     * - output duty cycle, set initially to 0
     * - GPIO number where LED is connected to
     * - speed mode, either high or low
     * - timer servicing selected channel
     *   Note: if different channels use one timer,
     *         then frequency and bit_num of these channels
     *         will be the same
     */
    ledc_channel_config_t ledc_channel[2] = {

        {
            .channel    = LEDC_LS_CH0_CHANNEL,
            .duty       = 0,
            .gpio_num   = LEDC_LS_CH0_GPIO,
            .speed_mode = LEDC_LS_MODE,
            .hpoint     = 0,
            .timer_sel  = LEDC_LS_TIMER
        },
        {
            .channel    = LEDC_LS_CH1_CHANNEL,
            .duty       = 0,
            .gpio_num   = LEDC_LS_CH1_GPIO,
            .speed_mode = LEDC_LS_MODE,
            .hpoint     = 0,
            .timer_sel  = LEDC_LS_TIMER
        },

    };

    // Set LED Controller with previously prepared configuration
	ledc_channel_config(&ledc_channel[0]);
	ledc_channel_config(&ledc_channel[1]);

    // Initialize fade service.
    ledc_fade_func_install(0);
}

// initializes wifi
void wifi_init_sta(void){
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = ESP_WIFI_SSID,
            .password = ESP_WIFI_PASS,
            /* Setting a password implies station will connect to all security modes including WEP/WPA.
             * However these modes are deprecated and not advisable to be used. Incase your Access point
             * doesn't support WPA2, these mode can be enabled by commenting below line */
	     .threshold.authmode = WIFI_AUTH_WPA2_PSK,

            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s password:%s",
                 ESP_WIFI_SSID, ESP_WIFI_PASS);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s",
                 ESP_WIFI_SSID, ESP_WIFI_PASS);
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }

    /* The event will not be processed after unregister */
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip));
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id));
    vEventGroupDelete(s_wifi_event_group);
}

static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data){
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < EXAMPLE_ESP_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG,"connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}


