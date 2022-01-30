/* WiFi station Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "driver/uart.h"
#include "esp_vfs_dev.h"
#include "esp_vfs_fat.h"

#include "lwip/err.h"
#include "lwip/sys.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"

#include "esp_console.h"
#include "linenoise/linenoise.h"
#include "argtable3/argtable3.h"

/* The examples use WiFi configuration that you can set via project configuration menu

   If you'd rather not, just change the below entries to strings with
   the config you want - ie #define EXAMPLE_WIFI_SSID "mywifissid"
*/

#define EXAMPLE_ESP_MAXIMUM_RETRY  CONFIG_ESP_MAXIMUM_RETRY

#define REQ_START " HTTP/1.1\r\nHost: "
#define MAX_RETRY 5

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static const char *TAG = "wifi station";

static int s_retry_num = 0;

static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
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

static struct {
	struct arg_str *SSID;
	struct arg_str *password;
	struct arg_end *end;
} wifiArgs;

int wifi_init_sta(int argc, char **argv)
{
	if(arg_parse(argc, argv, (void **)&wifiArgs))
	{
		arg_print_errors(stderr, wifiArgs.end, argv[0]);
		return 1;
	}
	const char *SSID = wifiArgs.SSID->sval[0];
	const char *password = wifiArgs.password->sval[0];
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
	     .threshold.authmode = WIFI_AUTH_WPA2_PSK,

            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };
    strcpy((char *)wifi_config.sta.ssid, SSID);
    strcpy((char *)wifi_config.sta.password, password);
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
                 SSID, password);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s",
        		SSID, password);
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }

    /* The event will not be processed after unregister */
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip));
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id));
    vEventGroupDelete(s_wifi_event_group);
    return 0;
}

void splitAddress(char **address, char **resource)
{
	long unsigned int i;
	*resource = NULL;
	for(i = 0; i < strlen(*address); i++)
	{
		if((*address)[i] == '/')
		{
			break;
		}
	}
	if(i < strlen(*address) - 1)
	{
		int len = strlen(*address);
		(*address)[i] = '\0';
		*resource = (char *)calloc(len - i + 1, sizeof(char));
		for(long unsigned int j = i + 1; j < len; j++)
		{
			(*resource)[j - i - 1] = (*address)[j];
		}
		*address = (char *)realloc(*address, (i + 1)*sizeof(char));
	}else{
		*resource = (char *)calloc(1, sizeof(char));
	}
	ESP_LOGI(TAG, "Server: %s", *address);
	ESP_LOGI(TAG, "Resource: %s", *resource);
}

static struct
{
	struct arg_str *getpost;
	struct arg_str *URL;
	struct arg_str *body;
	struct arg_end *end;
}HTTPArgs;

int HTTP_req(int argc, char **argv)
{
	if(arg_parse(argc, argv, (void **)&HTTPArgs))
	{
		arg_print_errors(stderr, wifiArgs.end, argv[0]);
		return 1;
	}
	const char *URL = HTTPArgs.URL->sval[0];
	char *site, *body;
	site = (char *)calloc(strlen(URL) + 1, sizeof(char));
	strcpy(site, URL);
	splitAddress(&site, &body);
	char *fullReq = (char *)calloc(strlen(REQ_START) + strlen(body) + strlen(site)  + 10, sizeof(char));
	strcpy(fullReq, "GET /");
	strcat(fullReq, body);
	strcat(fullReq, REQ_START);
	strcat(fullReq, site);
	strcat(fullReq, "\r\n\r\n");
	ESP_LOGI(TAG, "Sending following request:\n%s\n", fullReq);
	const struct addrinfo hints = {
		.ai_family = AF_INET,
		.ai_socktype = SOCK_STREAM,
	};
	struct addrinfo *res;
	struct in_addr *addr;
	int s, r;
	char recv_buf[127];
	int retryTimes = 0;
	while(retryTimes < MAX_RETRY)
	{
		retryTimes++;
		int err = getaddrinfo(site, "80", &hints, &res);
		if(err != 0 || res == NULL)
		{
			ESP_LOGE(TAG, "DNS lookup failed err=%d res=%p", err, res);
			vTaskDelay(1000 / portTICK_PERIOD_MS);
			continue;
		}

		/* Code to print the resolved IP.
		   Note: inet_ntoa is non-reentrant, look at ipaddr_ntoa_r for "real" code */
		addr = &((struct sockaddr_in *)res->ai_addr)->sin_addr;
		ESP_LOGI(TAG, "DNS lookup succeeded. IP=%s", inet_ntoa(*addr));

		s = socket(res->ai_family, res->ai_socktype, 0);
		if(s < 0)
		{
			ESP_LOGE(TAG, "... Failed to allocate socket.");
			freeaddrinfo(res);
			vTaskDelay(1000 / portTICK_PERIOD_MS);
			continue;
		}
		ESP_LOGI(TAG, "... allocated socket");

		if(connect(s, res->ai_addr, res->ai_addrlen) != 0)
		{
			ESP_LOGE(TAG, "... socket connect failed errno=%d", errno);
			close(s);
			freeaddrinfo(res);
			vTaskDelay(4000 / portTICK_PERIOD_MS);
			continue;
		}

		ESP_LOGI(TAG, "... connected");
		freeaddrinfo(res);

		if (write(s, fullReq, strlen(fullReq)) < 0)
		{
			ESP_LOGE(TAG, "... socket send failed");
			close(s);
			vTaskDelay(4000 / portTICK_PERIOD_MS);
			continue;
		}
		ESP_LOGI(TAG, "... socket send success");

		struct timeval receiving_timeout;
		receiving_timeout.tv_sec = 5;
		receiving_timeout.tv_usec = 0;
		if (setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &receiving_timeout, sizeof(receiving_timeout)) < 0)
		{
			ESP_LOGE(TAG, "... failed to set socket receiving timeout");
			close(s);
			vTaskDelay(4000 / portTICK_PERIOD_MS);
			continue;
		}
		ESP_LOGI(TAG, "... set socket receiving timeout success");

		/* Read HTTP response */
		do
		{
			bzero(recv_buf, sizeof(recv_buf));
			r = read(s, recv_buf, sizeof(recv_buf)-1);
			for(int i = 0; i < r; i++)
			{
				putchar(recv_buf[i]);
			}
		} while(r > 0);

		ESP_LOGI(TAG, "... done reading from socket. Last read return=%d errno=%d.", r, errno);
		close(s);
		free(fullReq);
		return 0;
	}
	return 1;
}

static void initConsole(void)
{
	const uart_config_t uart_config = {
		.baud_rate = CONFIG_ESP_CONSOLE_UART_BAUDRATE,
		.data_bits = UART_DATA_8_BITS,
		.parity = UART_PARITY_DISABLE,
		.stop_bits = UART_STOP_BITS_1,
		.source_clk = UART_SCLK_REF_TICK,
	};

	ESP_ERROR_CHECK( uart_driver_install(CONFIG_ESP_CONSOLE_UART_NUM,
			256, 0, 0, NULL, 0) );
	ESP_ERROR_CHECK( uart_param_config(CONFIG_ESP_CONSOLE_UART_NUM, &uart_config) );

	esp_vfs_dev_uart_use_driver(CONFIG_ESP_CONSOLE_UART_NUM);
	linenoiseSetMultiLine(true);
	linenoiseAllowEmpty(false);

	esp_console_config_t  console_config = {
		.max_cmdline_args = 4,
		.max_cmdline_length = 256,
	};
	ESP_ERROR_CHECK(esp_console_init(&console_config));
	//WiFi command
	esp_console_cmd_t wifiCmd = {
		.command = "wifi",
		.help = "Connects to a WiFi AP",
		.hint = NULL,
		.func = &wifi_init_sta,
		.argtable = &wifiArgs,
	};
	wifiArgs.SSID = arg_str1(NULL, NULL, "<s>", "SSID of the AP");
	wifiArgs.password = arg_str0(NULL, NULL, "<s>", "Password");
	wifiArgs.end = arg_end(2);
	ESP_ERROR_CHECK(esp_console_cmd_register(&wifiCmd));
	//HTTP request command
	esp_console_cmd_t HTTPCmd = {
		.command = "http",
		.help = "GETs or POSTs an http request",
		.hint = NULL,
		.func = &HTTP_req,
		.argtable = &HTTPArgs,
	};
	HTTPArgs.getpost = arg_str1(NULL, NULL, "<s>", "GET or POST command");
	HTTPArgs.URL = arg_str1(NULL, NULL, "<s>", "Full site URL (without http://)");
	HTTPArgs.body = arg_str0(NULL, NULL, "<s>", "POST request body");
	HTTPArgs.end = arg_end(2);
	ESP_ERROR_CHECK(esp_console_cmd_register(&HTTPCmd));
	ESP_ERROR_CHECK(esp_console_register_help_command());
	linenoiseSetCompletionCallback(&esp_console_get_completion);
	linenoiseSetHintsCallback((linenoiseHintsCallback*) &esp_console_get_hint);
}

void worker(void *pvParams)
{
	char *input;
	printf("Please, print \"help\" to view the command list\n");
	int ret;
	esp_err_t err;
	while(1)
	{
		input = linenoise("> ");
		if(input == NULL) continue;
		err = esp_console_run(input, &ret);
		if(err == ESP_ERR_NOT_FOUND)
		{
			ESP_LOGE(TAG, "Unrecognised command");
		}
	}
}

void app_main(void)
{
    //Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

	initConsole();

    xTaskCreate(worker, "Main task", 4096, NULL, 0, NULL);
}
