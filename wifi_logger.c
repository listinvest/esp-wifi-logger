#include "wifi_logger.h"

static const char* tag_wifi_logger = "wifi_logger";

/**
 * @brief Initialises message queue
 * 
 * @return esp_err_t ESP_OK - if queue init sucessfully, ESP_FAIL - if queue init failed
**/
esp_err_t init_queue(void)
{
    wifi_logger_queue = xQueueCreate(MESSAGE_QUEUE_SIZE, sizeof(char*));
    
    if (wifi_logger_queue == NULL)
    {
        ESP_LOGE(tag_wifi_logger, "%s", "Queue creation failed");
        return ESP_FAIL;
    }
    else
    {
        ESP_LOGI(tag_wifi_logger, "%s", "Queue created");
        return ESP_OK;
    }
}

/**
 * @brief Initialises and connects to wifi
**/
void init_wifi(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    ESP_ERROR_CHECK(example_connect());
}

/**
 * @brief Sends log message to message queue
 * 
 * @param log_message log message to be sent to the queue
 * @return esp_err_t ESP_OK - if queue init sucessfully, ESP_FAIL - if queue init failed
**/
esp_err_t send_to_queue(char* log_message)
{
    BaseType_t qerror = xQueueSendToBack(wifi_logger_queue, (void*)&log_message, (TickType_t) 0/portTICK_PERIOD_MS);
    
    if(qerror == pdPASS)
    {
        ESP_LOGD(tag_wifi_logger, "%s", "Data sent to Queue");
        return ESP_OK;
    }
    else if(qerror == errQUEUE_FULL)
    {
        ESP_LOGE(tag_wifi_logger, "%s", "Data not sent to Queue, Queue full");
        return ESP_FAIL;
    }
    else
    {
        ESP_LOGE(tag_wifi_logger, "%s", "Unknown error");
        return ESP_FAIL;
    }
}

/**
 * @brief Receive data from queue. Timeout is set to portMAX_DELAY, which is around 50 days (confirm from esp32 specs)
 * 
 * @return char* - returns log message received from the queue, returns NULL if error
**/
char* receive_from_queue(void)
{
    char* data;
    // ************************* IMPORTANT *******************************************************************
    // Timeout period is set to portMAX_DELAY, so if it doesnot receive a log message for ~50 days, config assert will fail and program will crash
    //
    BaseType_t qerror = xQueueReceive(wifi_logger_queue, &data, (TickType_t) portMAX_DELAY);
    configASSERT(qerror);
    //
    // *******************************************************************************************************
    
    if(qerror == pdPASS)
    {
        ESP_LOGD(tag_wifi_logger, "%s", "Data received from Queue");
    }
    else if(qerror == pdFALSE)
    {
        free((void*)data);

        ESP_LOGW(tag_wifi_logger, "%s", "Data not received from Queue");
        data = NULL;
    }
    else
    {
        free((void*)data);
        
        ESP_LOGE(tag_wifi_logger, "%s", "Unknown error");
        data = NULL;
    }

    return data;
}

/**
 * @brief generates log message, of the format generated by ESP_LOG function
 * 
 * @param level set ESP LOG level {E, W, I, D, V}
 * @param TAG Tag for the log message
 * @param line line
 * @param func func
 * @param fmt fmt
 */
void generate_log_message(esp_log_level_t level, const char *TAG, int line, const char *func, const char *fmt, ...)
{
    char log_print_buffer[BUFFER_SIZE];

    memset(log_print_buffer, '\0', BUFFER_SIZE);
    sprintf(log_print_buffer, "%s (%s:%d) ", TAG, func, line);
    va_list args;
    va_start(args, fmt);

    int len = strlen(log_print_buffer);
    vsprintf(&log_print_buffer[len], fmt, args);
    va_end(args);
    
    uint log_level_opt = 2;

    switch (level)
    {
    case ESP_LOG_ERROR:
        log_level_opt = 0;
        break;
    case ESP_LOG_WARN:
        log_level_opt = 1;
        break;
    case ESP_LOG_INFO:
        log_level_opt = 2;
        break;
    case ESP_LOG_DEBUG:
        log_level_opt = 3;
        break;
    case ESP_LOG_VERBOSE:
        log_level_opt = 4;
        break;
    default:
        log_level_opt = 2;
        break;
    }

    // ************************* IMPORTANT *******************************************************************
    // I am mallocing a char* inside generate_log_timestamp() function situated inside util.cpp, log_print_buffer is not being pushed to queue
    // The function returns the malloc'd char* and is passed to the queue
    //
    send_to_queue(generate_log_message_timestamp(log_level_opt, esp_log_timestamp(), log_print_buffer));
    //
    //********************************************************************************************************
}

/**
 * @brief route log messages generated by ESP_LOGX to the wifi logger
 * 
 * @param fmt logger string format
 * @param tag arguments
 * @return int return value of vprintf
 */
int system_log_message_route(const char* fmt, va_list tag)
{
    char *log_print_buffer = (char*) malloc(sizeof(char) * BUFFER_SIZE);
    vsprintf(log_print_buffer, fmt, tag);

    send_to_queue(log_print_buffer);

    return vprintf(fmt, tag);
}

/**
 * @brief wrapper function to start wifi logger
 * 
 */
void start_wifi_logger(void)
{
    init_wifi();
    ESP_ERROR_CHECK(init_queue());

    #ifdef CONFIG_ROUTE_ESP_IDF_API_LOGS_TO_WIFI
    esp_log_set_vprintf(system_log_message_route);
    #endif
    
    xTaskCreatePinnedToCore(&wifi_logger, "wifi_logger", 4096, NULL, 2, NULL, 1);
    ESP_LOGI(tag_wifi_logger, "WiFi logger initialised");
}

/**
 * @brief function which handles sending of log messages to server by UDP
 * 
 */
#ifdef CONFIG_TRANSPORT_PROTOCOL_UDP
void wifi_logger()
{
    struct network_data* handle = malloc(sizeof(struct network_data));
    network_manager(handle);

    while (true)
    {
        char* log_message = receive_from_queue();
 
        if (log_message != NULL)
        {
            int len = send_data(handle, log_message);
            ESP_LOGD(tag_wifi_logger, "%d %s", len, "bytes of data sent");
            
            free((void*)log_message);
        }
        else
        {
            log_message = "Unknown error - receiving log message";
            int len = send_data(handle, log_message);
            ESP_LOGE(tag_wifi_logger, "%d %s", len, "Unknown error");
        }
    }

    close_network_manager(handle);
}
#endif

/**
 * @brief function which handles sending of log messages to server by TCP
 * 
 */
#ifdef CONFIG_TRANSPORT_PROTOCOL_TCP
void wifi_logger()
{
    struct tcp_network_data* handle = malloc(sizeof(struct network_data));
    tcp_network_manager(handle);

    while (true)
    {
        char* log_message = receive_from_queue();

        if (log_message != NULL)
        {
            int len = tcp_send_data(handle, log_message);
            ESP_LOGD(tag_wifi_logger, "%d %s", len, "bytes of data sent");
        
            free((void*)log_message);
        }
        else
        {
            log_message = "Unknown error - receiving log message";
            int len = tcp_send_data(handle, log_message);
            ESP_LOGE(tag_wifi_logger, "%d %s", len, "Unknown error");
        }
    }

    tcp_close_network_manager(handle);
}
#endif

#ifdef CONFIG_TRANSPORT_PROTOCOL_WEBSOCKET
void wifi_logger()
{
    esp_websocket_client_handle_t handle = websocket_network_manager();

    while (true)
    {
        char* log_message = receive_from_queue();

        if (log_message != NULL)
        {
            int len = websocket_send_data(handle, log_message);
            ESP_LOGD(tag_wifi_logger, "%d %s", len, "bytes of data sent");
        
            free((void*)log_message);
        }
        else
        {
            log_message = "Unknown error - log message corrupt";
            int len = websocket_send_data(handle, log_message);
            ESP_LOGE(tag_wifi_logger, "%d %s", len, "Unknown error");
        }
    }

    websocket_close_network_manager(handle);
}
#endif
