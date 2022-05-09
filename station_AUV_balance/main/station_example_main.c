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
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "lwip/err.h"
#include "lwip/sys.h"

#include <stdio.h>
#include "driver/gpio.h"
#include "sdkconfig.h"

#include <sys/param.h>
#include "esp_netif.h"
#include <esp_http_server.h>

#include "driver/uart.h"

/* The examples use WiFi configuration that you can set via project configuration menu

   If you'd rather not, just change the below entries to strings with
   the config you want - ie #define EXAMPLE_WIFI_SSID "mywifissid"
*/
#define EXAMPLE_ESP_WIFI_SSID      "ASUS_14"
#define EXAMPLE_ESP_WIFI_PASS      "viamlabauv"
#define EXAMPLE_ESP_MAXIMUM_RETRY  250

#define BLINK_GPIO 4

#define EX_UART_NUM UART_NUM_0
#define PATTERN_CHR_NUM    (3)         /*!< Set the number of consecutive and identical characters received by receiver which defines a UART pattern*/

#define BUF_SIZE (1024)
#define RD_BUF_SIZE (BUF_SIZE)
#define PACKAGE_SIZE 12
#define PACKAGE_TRANSMIT_SIZE 5

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

#define PI 3.14159

static const char *TAG = "wifi station";
//static const uint8_t Wait_Counter = 120; //seconds

static QueueHandle_t uart0_queue;
static int s_retry_num = 0;
uint8_t data_init = 0;
bool enable_receive_package = false;
bool enable_transmit = false;
uint8_t byte_data = 0;
uint8_t byte_data_transmit = 0;
uint8_t package[PACKAGE_SIZE];
uint8_t package_transmit[PACKAGE_TRANSMIT_SIZE];
//Operation Mode
char Jetson[10] = "Jetson";
char Joystick[10] = "Joystick";
char Balance[10] = "Balance";
char Diving[10] = "Diving";

char Error_max_update;
char Errordot_max_update;
char Offset_max_update;
char Setpoint_update;

float Thruster_Speed, Rudder_Position, Mass_Position, Pistol_Position, Temperature, Pressure, Altimeter, Pitch; 

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

typedef enum
{
	JETSON = 0x00,
	JOYSTICK = 0x01,
	BALANCE = 0x02,
	DIVING = 0x03,
}Operation_Mode_Typedef;

struct 
{
    float Pitch;
    float Mass_Position;
    float Pistol_Position;
    uint8_t Error_max;
    uint8_t Errordot_max;
    uint8_t Offset_max;
    uint8_t Setpoint;
    char Operation_Mode[10];
}Data_Receive;

float Bytes_to_Float(uint8_t* _data_in)
{
	union
	{
		float _value;
		uint8_t _byte[4];
	}_part;
	
	_part._byte[0] = _data_in[3];
	_part._byte[1] = _data_in[2];
	_part._byte[2] = _data_in[1];
	_part._byte[3] = _data_in[0];
	
	return _part._value; 
}

static void String_Copy(char *des, uint8_t des_length, char *source, uint8_t source_length)
{
    for(uint8_t i=0; i<des_length; i++)
    {
        des[i] = NULL;
    }
    for(uint8_t i=0; i<source_length; i++)
    {
        des[i] = source[i];
    }
}

static float map(float x, float in_min, float in_max, float out_min, float out_max)
{
    return (x - in_min)*(out_max - out_min)/(in_max - in_min) + out_min;
}

static void Unpack(void)
{
    Data_Receive.Mass_Position = (float)package[0];
        Data_Receive.Mass_Position = map(Data_Receive.Mass_Position, 0, 58, -20, 20);
    Data_Receive.Pistol_Position = (float)package[1];
        Data_Receive.Pistol_Position = map(Data_Receive.Pistol_Position, 0, 95, 0, 1000);
    Data_Receive.Pitch = Bytes_to_Float(&package[2]);
        Data_Receive.Pitch = Data_Receive.Pitch*180.0/PI;   //rad to do
    Data_Receive.Error_max = package[6];
    Data_Receive.Errordot_max = package[7];
    Data_Receive.Offset_max = package[8];
    Data_Receive.Setpoint = package[9];
    switch(package[10])
    {
        case JETSON:
            String_Copy(Data_Receive.Operation_Mode,strlen(Data_Receive.Operation_Mode),Jetson,strlen(Jetson));
        break;
        case JOYSTICK:
            String_Copy(Data_Receive.Operation_Mode,strlen(Data_Receive.Operation_Mode),Joystick,strlen(Joystick));
        break;
        case BALANCE:
            String_Copy(Data_Receive.Operation_Mode,strlen(Data_Receive.Operation_Mode),Balance,strlen(Balance));
        break;
        case DIVING:
            String_Copy(Data_Receive.Operation_Mode,strlen(Data_Receive.Operation_Mode),Diving,strlen(Diving));
        break;
    }
}

static uint8_t Checksum(uint8_t *arr, uint8_t length)
{
    uint8_t sum = 0;
    for(uint8_t i=0; i<length-1; i++)
    {
        sum += arr[i];
    }
    sum = ~sum;
    sum +=2;
    return sum;
}

static int8_t cap(int8_t a, int8_t b)
{
    int8_t temp = 1;
    for(uint8_t i=0; i<b; i++)
    {
        temp *= a;
    }
    return temp;
}

static void Unpack_data_transmit(char *arr, uint8_t length)
{
    uint8_t a = 1,b;
    int8_t temp = 0;
    for(uint8_t i=0; i<length; i++)
    {
        if(arr[i] != 'A' && arr[i] != 'B' && arr[i] != 'C' && arr[i] != 'D' && arr[i] != 'E')
            arr[i] -= 48;
    }
    if(arr[0] == 'A')
    {
        while(arr[a] != 'B')
            a++;
        for(uint8_t i=a-1; i>0; i--)
        {
            temp += arr[i]*cap(10,a-1-i);
        }
        Setpoint_update = temp;
        temp = 0;
        b = a;

        while(arr[a] != 'C')
            a++;
        for(uint8_t i=a-1; i>b; i--)
        {
            temp += arr[i]*cap(10,a-1-i);
        }
        Error_max_update = temp;
        temp = 0;
        b = a;

        while(arr[a] != 'D')
            a++;
        for(uint8_t i=a-1; i>b; i--)
        {
            temp += arr[i]*cap(10,a-1-i);
        }
        Errordot_max_update = temp;
        temp = 0;
        b = a;

        while(arr[a] != 'E')
            a++;
        for(uint8_t i=a-1; i>b; i--)
        {
            temp += arr[i]*cap(10,a-1-i);
        }
        Offset_max_update = temp;
        temp = 0;
        b = a;
    }
}

static void Pack_data_transmit(void)
{
    package_transmit[0] = Setpoint_update;
    package_transmit[1] = Error_max_update;
    package_transmit[2] = Errordot_max_update;
    package_transmit[3] = Offset_max_update;

    package_transmit[4] = Checksum(package_transmit, PACKAGE_TRANSMIT_SIZE);
}

static void Transmit_data(void)
{
    uart_write_bytes(EX_UART_NUM,(char *)&package_transmit[byte_data_transmit], 1);
    byte_data_transmit ++;
    if(byte_data_transmit == PACKAGE_TRANSMIT_SIZE)
    {
        byte_data_transmit = 0;
        enable_transmit = false;
    }
}

static void uart_event_task(void *pvParameters)
{
    uart_event_t event;
    size_t buffered_size;
    uint8_t* dtmp = (uint8_t*) malloc(RD_BUF_SIZE);
    for(;;) {
        //Waiting for UART event.
        if(xQueueReceive(uart0_queue, (void * )&event, (portTickType)portMAX_DELAY)) {
            bzero(dtmp, RD_BUF_SIZE);
            switch(event.type) {
                //Event of UART receving data
                /*We'd better handler data event fast, there would be much more data events than
                other types of events. If we take too much time on data event, the queue might
                be full.*/
                case UART_DATA:
                    uart_read_bytes(EX_UART_NUM, dtmp, event.size, portMAX_DELAY);
                    if(!enable_receive_package)
                    {
                        switch(data_init)
                        {
                            case 0:
                                if(*dtmp == 'A')
                                    data_init ++;
                            break;
                            case 1:
                                if(*dtmp == 'R')
                                    data_init ++;
                                else
                                    data_init = 0;
                            break;
                            case 2:
                                if(*dtmp == 'M')
                                {
                                    data_init = 0;
                                    //gpio_set_level(BLINK_GPIO, 1);
                                    enable_receive_package = true;
                                }    
                            break;
                        }
                    }
                    else
                    {
                        package[byte_data] = *dtmp;
                        byte_data++;
                        if(byte_data == PACKAGE_SIZE)
                        {
                            if(package[PACKAGE_SIZE-1] == Checksum(package, PACKAGE_SIZE))
                                Unpack();
                            byte_data = 0;
                            enable_receive_package = false;
                            //gpio_set_level(BLINK_GPIO, 0);
                        }
                    }
                    
                    break;
                //Others
                default:
                    ESP_LOGI(TAG, "uart event type: %d", event.type);
                    break;
            }
        }
    }
    free(dtmp);
    dtmp = NULL;
    vTaskDelete(NULL);
}

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

void wifi_init_sta(void)
{
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
            .ssid = EXAMPLE_ESP_WIFI_SSID,
            //.password = EXAMPLE_ESP_WIFI_PASS,
            //.bssid = {0x7c,0x10,0xc9,0x32,0x3d,0x14},
            /* Setting a password implies station will connect to all security modes including WEP/WPA.
             * However these modes are deprecated and not advisable to be used. Incase your Access point
             * doesn't support WPA2, these mode can be enabled by commenting below line */
	     //.threshold.authmode = WIFI_AUTH_WPA2_PSK,
         .threshold.authmode = WIFI_AUTH_OPEN,

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
        ESP_LOGI(TAG, "connected to ap SSID:%s",
                 EXAMPLE_ESP_WIFI_SSID);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s",
                 EXAMPLE_ESP_WIFI_SSID);
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }

    /* The event will not be processed after unregister */
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip));
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id));
    vEventGroupDelete(s_wifi_event_group);
}

/* An HTTP GET handler */
/**********************//***********************/
static esp_err_t AUV_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)index_html_start, index_html_end - index_html_start);
    return ESP_OK;
}

static const httpd_uri_t auv = {
    .uri       = "/auv",
    .method    = HTTP_GET,
    .handler   = AUV_handler,
    /* Let's pass response string in user
     * context to demonstrate it's usage */
    .user_ctx  = NULL
};

static esp_err_t AUV_UPDATE_DATA_handler(httpd_req_t *req)
{
    char resp_str[500];
    sprintf(resp_str, "{\"mass_position\": \"%.2f\",\"pistol_position\": \"%.2f\",\"pitch_value\": \"%.2f\",\"error\": \"%d\",\"errordot\": \"%d\",\"offset\": \"%d\",\"setpoint\": \"%d\",\"mode\": \"%s\"}", Data_Receive.Mass_Position,Data_Receive.Pistol_Position,Data_Receive.Pitch,Data_Receive.Error_max,Data_Receive.Errordot_max,Data_Receive.Offset_max,Data_Receive.Setpoint,Data_Receive.Operation_Mode);
    httpd_resp_send(req, resp_str, strlen(resp_str));
    return ESP_OK;
}

static const httpd_uri_t auv_update_data = {
    .uri       = "/auv_update_data",
    .method    = HTTP_GET,
    .handler   = AUV_UPDATE_DATA_handler,
    /* Let's pass response string in user
     * context to demonstrate it's usage */
    .user_ctx  = NULL
};
/**********************//***********************/

/* An HTTP POST handler */
/**********************//***********************/
static esp_err_t FUZZY_UPDATE_handler(httpd_req_t *req)
{
    char buf[100];
    /* Read the data for the request */
    httpd_req_recv(req, buf,req->content_len);
    /* Send back the same data */
    // printf("%s\n", buf);
    //End respone
    httpd_resp_send_chunk(req, NULL, 0);

    Unpack_data_transmit(buf,strlen(buf));
    Pack_data_transmit();
    enable_transmit = true;
    return ESP_OK;
}
static const httpd_uri_t fuzzy_update = {
    .uri       = "/fuzzy_update",
    .method    = HTTP_POST,
    .handler   = FUZZY_UPDATE_handler,
    .user_ctx  = NULL
};
//--------------------------------------------------

esp_err_t http_404_error_handler(httpd_req_t *req, httpd_err_code_t err)
{
    /* For any other URI send 404 and close socket */
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Some 404 error message");
    return ESP_FAIL;
}

static httpd_handle_t start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;

    // Start the httpd server
    ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK) {
        // Set URI handlers
        ESP_LOGI(TAG, "Registering URI handlers");
        httpd_register_uri_handler(server, &auv);
        httpd_register_uri_handler(server, &auv_update_data);
        httpd_register_uri_handler(server, &fuzzy_update);
        return server;
    }

    ESP_LOGI(TAG, "Error starting server!");
    return NULL;
}

static void stop_webserver(httpd_handle_t server)
{
    // Stop the httpd server
    httpd_stop(server);
}

static void disconnect_handler(void* arg, esp_event_base_t event_base, 
                               int32_t event_id, void* event_data)
{
    httpd_handle_t* server = (httpd_handle_t*) arg;
    if (*server) {
        ESP_LOGI(TAG, "Stopping webserver");
        stop_webserver(*server);
        *server = NULL;
    }
}

static void connect_handler(void* arg, esp_event_base_t event_base, 
                            int32_t event_id, void* event_data)
{
    httpd_handle_t* server = (httpd_handle_t*) arg;
    if (*server == NULL) {
        ESP_LOGI(TAG, "Starting webserver");
        *server = start_webserver();
    }
}

void app_main(void)
{
    // for(uint8_t i=0; i<Wait_Counter; i++)
    // {
    //     vTaskDelay(1000 / portTICK_PERIOD_MS);
    // }
    
    gpio_pad_select_gpio(BLINK_GPIO);
    gpio_set_direction(BLINK_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_pull_mode(BLINK_GPIO, GPIO_PULLDOWN_ONLY);

    gpio_set_level(BLINK_GPIO, 1);
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    gpio_set_level(BLINK_GPIO, 0);

    static httpd_handle_t server = NULL;

    //Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");
    wifi_init_sta();

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &connect_handler, &server));
 //   ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &disconnect_handler, &server));
    start_webserver();

    esp_log_level_set(TAG, ESP_LOG_INFO);

    /* Configure parameters of an UART driver,
     * communication pins and install the driver */
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };
    //Install UART driver, and get the queue.
    uart_driver_install(EX_UART_NUM, BUF_SIZE * 2, BUF_SIZE * 2, 20, &uart0_queue, 0);
    uart_param_config(EX_UART_NUM, &uart_config);

    //Set UART log level
    esp_log_level_set(TAG, ESP_LOG_INFO);
    //Set UART pins (using UART0 default pins ie no changes.)
    uart_set_pin(EX_UART_NUM, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    //Set uart pattern detect function.
    uart_enable_pattern_det_baud_intr(EX_UART_NUM, '+', PATTERN_CHR_NUM, 9, 0, 0);
    //Reset the pattern queue length to record at most 20 pattern positions.
    uart_pattern_queue_reset(EX_UART_NUM, 20);

    //Create a task to handler UART event fromx ISR
    xTaskCreate(uart_event_task, "uart_event_task", 2048, NULL, 12, NULL);

    while(1)
    {
        // if(buf[18] == '1')
        // {
        //     gpio_set_level(BLINK_GPIO, 1);
        // }
        // else
        // {
        //     gpio_set_level(BLINK_GPIO, 0);
        // }
        // printf("%c", buf[5000]);
        if(enable_transmit)
        {
            
            Transmit_data();
            // printf("Hello\n");
            // enable_transmit = false;
        }
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}
