#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "driver/gpio.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "esp_sleep.h"
#include "lwip/sockets.h"

#include "esp_camera.h"
#include "config.h"
#include "types.h"
#include "camera.h"

/*
Buscar cosas por hacer: TODO
Buscar cosas de las que no estoy seguro: ALERTA



TOCTOU race
*/

// macros
/* esta macro se encarga de notificar el error por la ESP y al servidor */
#define INFORMAR_ERROR(texto) do { \
    ESP_LOGE(TAG, texto); \
    enviar_texto_servidor(texto); \
} while(0)

#define MAXIMUM_RETRY  5 // deshabilitado en el handler
#define TIMER_TIME_PREDETERMINADO 30000000 // tiempo en us

#if CONFIG_ESP_STATION_EXAMPLE_WPA3_SAE_PWE_HUNT_AND_PECK
#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_HUNT_AND_PECK
#define EXAMPLE_H2E_IDENTIFIER ""
#elif CONFIG_ESP_STATION_EXAMPLE_WPA3_SAE_PWE_HASH_TO_ELEMENT
#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_HASH_TO_ELEMENT
#define EXAMPLE_H2E_IDENTIFIER CONFIG_ESP_WIFI_PW_ID
#elif CONFIG_ESP_STATION_EXAMPLE_WPA3_SAE_PWE_BOTH
#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_BOTH
#define EXAMPLE_H2E_IDENTIFIER CONFIG_ESP_WIFI_PW_ID
#endif
#if CONFIG_ESP_WIFI_AUTH_OPEN
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_OPEN
#elif CONFIG_ESP_WIFI_AUTH_WEP
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WEP
#elif CONFIG_ESP_WIFI_AUTH_WPA_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA2_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA_WPA2_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA_WPA2_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA3_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA3_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA2_WPA3_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_WPA3_PSK
#elif CONFIG_ESP_WIFI_AUTH_WAPI_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WAPI_PSK
#endif

// TODO: KIKEN predeterminado (?)
#ifndef ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD
    #define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_PSK
#endif

#ifndef ESP_WIFI_SAE_MODE
    #define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_HUNT_AND_PECK
#endif

#ifndef EXAMPLE_H2E_IDENTIFIER
    #define EXAMPLE_H2E_IDENTIFIER ""
#endif

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

#define HEARTBEAT_TIME 5
#define TIMEOUT_TIME 15
#define CHECK_CONNECTION_TIME 1000
#define BIT_CONEXION BIT0
#define BIT_DESCONEXION BIT1



// variables globales
static const char* TAG = "MAIN";
static EventGroupHandle_t s_wifi_event_group, connection_event_group_handle;
static esp_netif_t* sta_netif = NULL;
static int s_retry_num = 0;
static int server_socket = -1;
static TaskHandle_t heartbeat_ping_handle, execution_thread_handle, send_thread_handle, receive_thread_handle, connection_thread_handle, capture_thread_handle;
static QueueHandle_t send_queue, receive_queue;
static esp_timer_handle_t timer;
static SemaphoreHandle_t server_socket_m;


// cabeceras funciones
void init_gpio(void);
void wifi_init_sta(void);
int init_sockets(void);
int delete_timer(esp_timer_handle_t* p_timer);
int create_timer(esp_timer_handle_t* p_timer, uint64_t time);
static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
void execution_thread(void* args);
void send_thread(void* args);
void heartbeat_ping(void* args);
void receive_thread(void* args); // TODO: limpiar basura que quedo en colas y cosas asi cuando se reconecta, para todos los threads que se paren mientras se reconecta
void connection_thread(void* args);
void capture_thread(void* args);
int recibir_cabecera(tCabecera* c);
int recibir_body(uint32_t size, void* body);
int command_execute(tComando c);
int enviar_texto_servidor(char* msj);
void timer_callback(void* arg);
void flash(camera_fb_t** fb);
int enviar_nodo(int socket_servidor, tNodo* n, int size_cabecera, int size_body);
void informar_error(char* msj);


void app_main(void) {
    server_socket_m = xSemaphoreCreateMutex();
    connection_event_group_handle = xEventGroupCreate();
    send_queue = xQueueCreate(10, sizeof(tNodo));
    receive_queue = xQueueCreate(10, sizeof(tNodo));
    int num = 0;
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    init_gpio();
    if (init_camera() != CAMERA_OK) esp_restart();
    create_timer(&timer, TIMER_TIME_PREDETERMINADO);
    wifi_init_sta();
    xEventGroupSetBits(connection_event_group_handle, BIT_DESCONEXION); // para conectar el socket

    xTaskCreatePinnedToCore (
        connection_thread,            // función de la tarea
        "Hilo - connection_thread",      // nombre
        8192,                         // stack en words, 1024 -> 4KB
        NULL,                         // parámetros
        1,                            // prioridad menos 1 - 25 mas
        &connection_thread_handle,              // handle para manejar elementos de la tarea
        1                             // APP_CPU
    );

    xTaskCreatePinnedToCore (
        heartbeat_ping,               // función de la tarea
        "Hilo - heartbeat_ping",      // nombre
        8192,                         // stack en words, 1024 -> 4KB
        NULL,                         // parámetros
        1,                            // prioridad menos 1 - 25 mas
        &heartbeat_ping_handle,              // handle para manejar elementos de la tarea
        1                             // APP_CPU
    );

    xTaskCreatePinnedToCore (
        execution_thread,               // función de la tarea
        "Hilo - execution_thread",      // nombre
        8192,                         // stack en words, 1024 -> 4KB
        NULL,                         // parámetros
        1,                            // prioridad menos 1 - 25 mas
        &execution_thread_handle,              // handle para manejar elementos de la tarea
        1                             // APP_CPU
    );

    xTaskCreatePinnedToCore (
        send_thread,               // función de la tarea
        "Hilo - send_thread",      // nombre
        8192,                         // stack en words, 1024 -> 4KB
        NULL,                         // parámetros
        1,                            // prioridad menos 1 - 25 mas
        &send_thread_handle,              // handle para manejar elementos de la tarea
        1                             // APP_CPU
    );

    xTaskCreatePinnedToCore (
        receive_thread,               // función de la tarea
        "Hilo - receive_thread",      // nombre
        8192,                         // stack en words, 1024 -> 4KB
        NULL,                         // parámetros
        1,                            // prioridad menos 1 - 25 mas
        &receive_thread_handle,              // handle para manejar elementos de la tarea
        1                             // APP_CPU
    );

    xTaskCreatePinnedToCore (
        capture_thread,               // función de la tarea
        "Hilo - capture_thread",      // nombre
        8192,                         // stack en words, 1024 -> 4KB
        NULL,                         // parámetros
        1,                            // prioridad menos 1 - 25 mas
        &capture_thread_handle,              // handle para manejar elementos de la tarea
        1                             // APP_CPU
    );

    while (1) {
        vTaskDelay(portMAX_DELAY);
    }
}


void init_gpio(void) {
    gpio_config_t config = {
        .pin_bit_mask = ((1ULL << FLASH_PIN)),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };

    gpio_config(&config);
}

int init_sockets(void) {
    struct sockaddr_in server_socket_addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = inet_addr(SERVER_IP),
        .sin_port = htons(SERVER_PORT),
    };
    struct timeval tv = {
        .tv_sec = TIMEOUT_TIME, // segundos
        .tv_usec = 0 // microsegundos
    };


    xSemaphoreTake(server_socket_m, portMAX_DELAY);
    if (server_socket >= 0) {
        close(server_socket);
    }
    server_socket = socket(AF_INET, SOCK_STREAM, 0); // IPv4 + TPC
    if (server_socket < 0) {
        xSemaphoreGive(server_socket_m);
        ESP_LOGE(TAG, "ERROR AL CREAT SOCKET: socket()");
        return 1;
    }
    if (setsockopt(server_socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv))) {
        close(server_socket);
        xSemaphoreGive(server_socket_m);
        ESP_LOGE(TAG, "init_sockets: setsockopt tv");
        return 1;
    } // introducimos un timeout de TIMEOUT_TIME
    int flag = 1;
    if (setsockopt(server_socket, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag))) {
        close(server_socket);
        xSemaphoreGive(server_socket_m);
        ESP_LOGE(TAG, "init_sockets: setsockopt nagle");
        return 1;
    } // desactivamos el algoritmo de nagle, hacia mas lento el envio de fotos
    if (connect(server_socket, (struct sockaddr*)&server_socket_addr, sizeof(server_socket_addr)) < 0) {
        close(server_socket);
        xSemaphoreGive(server_socket_m);
        ESP_LOGE(TAG, "ERROR AL CONECTAR SOCKET: connect()");
        return 1;
    }
    xSemaphoreGive(server_socket_m);
    return 0;
}

void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();


    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    sta_netif = esp_netif_create_default_wifi_sta();
    // esp_netif_set_ip_info(sta_netif, &ipInfo); // se puede quitar

    // TODO: KIKEN revisar (seguridad WiFi)
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

    // TODO: KIKEN revisar (seguridad WiFi)
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            /* Authmode threshold resets to WPA2 as default if password matches WPA2 standards (password len => 8).
             * If you want to connect the device to deprecated WEP/WPA networks, Please set the threshold value
             * to WIFI_AUTH_WEP/WIFI_AUTH_WPA_PSK and set the password with length and format matching to
             * WIFI_AUTH_WEP/WIFI_AUTH_WPA_PSK standards.
             */
            .threshold.authmode = ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD,
            .sae_pwe_h2e = ESP_WIFI_SAE_MODE,
            .sae_h2e_identifier = EXAMPLE_H2E_IDENTIFIER,
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
    // EventBits_t devuelve los bits (flags) que se levantaron
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Conectado correctamente a la SSID:%s", WIFI_SSID);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Error de conexion a la SSID:%s", WIFI_SSID);
    } else {
        ESP_LOGE(TAG, "Evento inesperado");
    }
}

// Trata de conectarse indefinidamente a una red WiFi
static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect(); // con la configuracion wifi definida nos intentamos conectar a la red wifi
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < MAXIMUM_RETRY) {
            esp_wifi_connect();
            // s_retry_num++; se intenta conectar indefinidamente
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


// hilo que ejecuta el heartbeat
void heartbeat_ping(void* args) {
    tCabecera cabecera_ping = {
        .tipo = PING,
        .size = 5,
    };

    while (1) {
        EventBits_t bits = xEventGroupWaitBits(connection_event_group_handle, BIT_CONEXION, pdFALSE, pdTRUE, portMAX_DELAY);
        char* ping = malloc(5);
        if (ping == NULL) { // TODO: quizas empiece como loco a ejecutarse porque no hay delays
            ESP_LOGE(TAG, "heartbeat_ping: error malloc");
            continue;
        }
        memcpy(ping, "PING", 5); // <-- ya que nodo_free libera el body, hay que hacerlo asi
        tNodo nodo = {
            .cabecera = cabecera_ping,
            .body = (void*) ping,
        };
        xQueueSend(send_queue, &nodo, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_TIME * 1000)); // TODO: chungo si el BIT_CONEXION se baja mientras espera
    }
    vTaskDelete(NULL);
}


void execution_thread(void* args) {
    tCabecera cabecera_pong = {
        .tipo = PONG,
        .size = 5,
    };
    while (1) {
        EventBits_t bits = xEventGroupWaitBits(connection_event_group_handle, BIT_CONEXION, pdFALSE, pdTRUE, portMAX_DELAY);
        tNodo nodo;
        if (xQueueReceive(receive_queue, &nodo, pdMS_TO_TICKS(CHECK_CONNECTION_TIME)) != pdTRUE) { // comprobamos cada segundo que seguimos teniendo conexion
            continue;
        }
        switch (nodo.cabecera.tipo) {
            case PING: { // se ponen llaves para diferenciarlo del resto de codigo y que no se malloc cuando no es necesario
                char* pong = malloc(5);
                memcpy(pong, "PONG", 5);
                tNodo n = {
                    .body = (void*) pong,
                    .cabecera = cabecera_pong,
                };
                xQueueSend(send_queue, &n, portMAX_DELAY);
                break;
            }
            case PONG: // 
                // printf("PONG!\n");
                break;
            case IMAGEN: // guardar imagen
                printf("Se ha recibido una imagen\n");
                break;
            case TEXTO:
                printf("Mensaje Recibido: %s\n", (char*) nodo.body);
                break;
            case COMANDO: // TODO: CAMBIAR, tener en cuenta que el contenido del body vendra en tComando
                if (nodo.body != NULL) {
                    if (command_execute(*((tComando*) nodo.body))) {
                        // TODO: que pongo ?
                    }
                }
                break;
            default:
                break;
        }
        if (nodo.body != NULL) {
            free(nodo.body);
        }
    }
    vTaskDelete(NULL);
}

void send_thread(void* args) {
    while (1) {
        EventBits_t bits = xEventGroupWaitBits(connection_event_group_handle, BIT_CONEXION, pdFALSE, pdTRUE, portMAX_DELAY);
        tNodo nodo;
        if (xQueueReceive(send_queue, &nodo, pdMS_TO_TICKS(CHECK_CONNECTION_TIME)) != pdTRUE) {
            continue;
        }
        int64_t t10 = esp_timer_get_time();// BORRAR
        if (enviar_nodo(server_socket, &nodo, sizeof(tCabecera), nodo.cabecera.size)) {
            xEventGroupClearBits(connection_event_group_handle, BIT_CONEXION);
            xEventGroupSetBits(connection_event_group_handle, BIT_DESCONEXION);
            ESP_LOGE(TAG, "send_thread: error de conexion, descartando nodo");
            continue;
        }
        int64_t t1f = esp_timer_get_time();// BORRAR
        ESP_LOGI(TAG, "TIEMPO ENVIO: %lld PARA %lu BYTES", t1f - t10, nodo.cabecera.size);// BORRAR
        if (nodo.cabecera.tipo == IMAGEN) {
            heap_caps_free(nodo.body);
        }
        else if (nodo.body != NULL) {
            free(nodo.body);
        }
        int64_t t2f = esp_timer_get_time();// BORRAR
        
        ESP_LOGI(TAG, "TIEMPO FREE: %lld", t2f - t1f); // BORRAR
    }
    vTaskDelete(NULL);
}

void receive_thread(void* args) {
    while (1) {
        EventBits_t bits = xEventGroupWaitBits(connection_event_group_handle, BIT_CONEXION, pdFALSE, pdTRUE, portMAX_DELAY);
        tCabecera c;
        if (recibir_cabecera(&c)) {
            xEventGroupClearBits(connection_event_group_handle, BIT_CONEXION);
            xEventGroupSetBits(connection_event_group_handle, BIT_DESCONEXION);
            ESP_LOGE(TAG, "receive_thread: error recibiendo cabecera entrando en modo reconexion...");
            continue;
        }
        void* body = malloc(c.size);
        if (body == NULL) {
            ESP_LOGE(TAG, "Error malloc - main code");
            continue;
        }
        if (recibir_body(c.size, body)) {
            free(body);
            xEventGroupClearBits(connection_event_group_handle, BIT_CONEXION);
            xEventGroupSetBits(connection_event_group_handle, BIT_DESCONEXION);
            ESP_LOGE(TAG, "receive_thread: error recibiendo body entrando en modo reconexion...");
            continue;
        }
        tNodo n = {
            .cabecera = c,
            .body = body,
        };
        xQueueSend(receive_queue, &n, portMAX_DELAY);
    }
    vTaskDelete(NULL);
}


void connection_thread(void* args) {
    while (1) {
        xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY); // espera wifi
        EventBits_t bits = xEventGroupWaitBits(connection_event_group_handle, BIT_DESCONEXION, pdTRUE, pdTRUE, portMAX_DELAY); // espera socket desconectado
        // ALERTA: proteger el soccket aqui?
        if (server_socket >= 0) {
            xQueueReset(send_queue);
            xQueueReset(receive_queue);
        }
        while (init_sockets()) {
            vTaskDelay(pdMS_TO_TICKS(3000)); // TODO: quizas esta sincrinizacion con la aceptacion del socket esta rota
        }
        xEventGroupSetBits(connection_event_group_handle, BIT_CONEXION);
    }
}


void capture_thread(void* args) {
    while (1) {
        EventBits_t bits = xEventGroupWaitBits(connection_event_group_handle, BIT_CONEXION, pdFALSE, pdTRUE, portMAX_DELAY);
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        // xTaskNotifyStateClear(NULL); // descartamos las notificaciones que no ha dado tiempo a procesar
        camera_fb_t* fb = NULL;
        // int64_t t10 = esp_timer_get_time();
        if (capture_photo(&fb) != CAMERA_OK) {
            INFORMAR_ERROR("Error al capturar la imagen");
            continue;
        }
        // int64_t t1f = esp_timer_get_time();
        // ESP_LOGI(TAG, "TIEMPO CAMARA SHOT: %lld us", t1f - t10);
        // int64_t t20 = esp_timer_get_time();
        uint8_t* img = (uint8_t*) heap_caps_malloc(fb->len, MALLOC_CAP_SPIRAM);
        if (img == NULL) {
            esp_camera_fb_return(fb);
            INFORMAR_ERROR("capture_thread: error al reservar psram para enviar imagen");
            continue;
        }
        memcpy(img, fb->buf, fb->len);
        // int64_t t2f = esp_timer_get_time();
        // ESP_LOGI(TAG, "TIEMPO BUFFER COPY: %lld us", t2f - t20);

        tNodo n = {
            .cabecera = {
                .tipo = IMAGEN,
                .size = fb->len,
            },
            .body = (void*) img,
        };

        if (xQueueSend(send_queue, &n, portMAX_DELAY) != pdTRUE) {
            INFORMAR_ERROR("capture_thread: error al enviar la imagen por send_queue");
            free(img);
        }

        esp_camera_fb_return(fb); // marcamos el espacio capturado como libre (podemos sobreescribirlo)
    }
}

// TODO: malloc innecesario
int recibir_cabecera(tCabecera* c) {
    int total = 0;
    void* cabecera = malloc(sizeof(tCabecera));
    xSemaphoreTake(server_socket_m, portMAX_DELAY);
    int server_socket_protegido = server_socket;
    xSemaphoreGive(server_socket_m);

    while (total < sizeof(tCabecera)) {
        ssize_t r = recv(server_socket_protegido, (char*)cabecera + total, sizeof(tCabecera) - total, 0); // hacemos un parse a char para sumar correctamente los bytes
        if (r > 0) {
            total += r;
        }
        else if (r == 0) {
            ESP_LOGE(TAG, "recibir_cabecera: conexion interrumpida mientras se recibe la cabecera");
            free(cabecera);
            return 1;
        }
        else if (r == -1) {
            ESP_LOGE(TAG, "recibir_cabecera: error de conexion mientras se recibe la cabecera\n");
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                ESP_LOGE(TAG, "TIEMPO DE RESPUESTA AGOTADO");
            }
            free(cabecera);
            return 1;
        }
    }

    *c = *((tCabecera*) cabecera);
    free(cabecera);
    return 0;
}


int recibir_body(uint32_t size, void* body) {
    uint32_t total = 0;
    xSemaphoreTake(server_socket_m, portMAX_DELAY);
    int server_socket_protegido = server_socket;
    xSemaphoreGive(server_socket_m);
    while (total < size) {
        ssize_t r = recv(server_socket_protegido, (char*)body + total, size - total, 0);
        if (r > 0) {
            total += r;
        }
        else if (r == 0) {
            printf("Conexion interrumpida mientras se recibe el body\n");
            return 1;
        }
        else if (r == -1) {
            printf("Error de conexion mientras se recibe el body\n");
            return 1;
        }
    }
    return 0;
}


int command_execute(tComando c) {
    switch (c.comando) {
        case FLASH:
            if (change_flash_mode(c.parametro) != CAMERA_OK) {
                INFORMAR_ERROR("command_execute: parametro invalido para comando FLASH");
                return 1;
            }
            char msj[32];
            snprintf(msj, sizeof(msj), "flash %s", ((get_flash_mode() == 0) ? "desactivado" : "activado"));
            enviar_texto_servidor(msj);
            break;
        case FOTO: // se asume que no hay parametro, se toma una foto
            xTaskNotifyGive(capture_thread_handle);
            /*            
            if (c.parametro >= 0) {
                // TODO: quizas haya que cambiar mucha logica, osea hacer una especie de cola de eventos con tiempos que cuando lleguen a 0 se ejecuten esos eventos, no se si merece la pena
            }
            else {
                INFORMAR_ERROR("command_execute: parametro invalido para comando FOTO");
                return 1;
            }
            */
            break;
        case TIEMPO_FOTO:
            if (c.parametro > 0) { 
                // el c.parametro viene en segundos !!
                delete_timer(&timer);
                create_timer(&timer, ((uint64_t) c.parametro) * 1000000);
            }
            else {
                INFORMAR_ERROR("command_execute: parametro invalido para comando TIEMPO_FOTO");
                return 1;
            }
            break;
        default:
            ESP_LOGE(TAG, "command_execute: comando no reconocido");
            return 1;
    }
    return 0;
}

int enviar_texto_servidor(char* msj) {
    if (msj == NULL) {
        ESP_LOGE(TAG, "enviar_texto_servidor: msj es NULL");
        return 1;
    }
    int len = strlen(msj) + 1;
    tNodo n = {
        .cabecera = {
            .tipo = TEXTO,
            .size = len,
        },
        .body = malloc(len),
    };
    if (n.body == NULL) {
        ESP_LOGE(TAG, "enviar_texto_servidor: error malloc");
        return 1;
    }
    memcpy(n.body, msj, len);
    if (xQueueSend(send_queue, &n, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "enviar_texto_servidor: error al enviar por send_queue");
        return 1;
    }
    return 0;
}

int delete_timer(esp_timer_handle_t* p_timer) {
    esp_err_t ret;
    ret = esp_timer_stop(*p_timer);
    if (ret != ESP_OK) { // PENDIENTE
        INFORMAR_ERROR("delete_timer: error al parar el timer");
        return 1;
    }
    ret = esp_timer_delete(*p_timer);
    *p_timer = NULL;
    return 0;
}

int create_timer(esp_timer_handle_t* p_timer, uint64_t time) {
    if (*p_timer != NULL) return 1;
    esp_timer_create_args_t timer_args = {
        .callback = timer_callback,
        .arg = NULL,
        .name = "timer"
    };
    
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, p_timer));
    esp_timer_start_periodic(*p_timer, time);
    return 0;
}


void timer_callback(void* arg) {
    xTaskNotifyGive(capture_thread_handle); // notificamos al hilo con su handler que la tarea debe de ejecutarse
}


int enviar_nodo(int socket_servidor, tNodo* n, int size_cabecera, int size_body) {
    uint32_t total = 0;
    xSemaphoreTake(server_socket_m, portMAX_DELAY);
    int socket_servidor_protegido = server_socket;
    xSemaphoreGive(server_socket_m);
    while (total < size_cabecera) {
        ssize_t r = send(socket_servidor_protegido, ((char*) &n->cabecera) + total, size_cabecera - total, 0);
        if (r > 0) {
            total += r;
        }
        else if (r == 0) {
            ESP_LOGE(TAG, "Conexion interrumpida mientras se envia la cabecera");
            return 1;
        }
        else if (r == -1) {
            ESP_LOGE(TAG, "Error de conexion mientras se envia la cabecera\n");
            return 1;
        }
    }
    total = 0;
    int num_envios = 0;// BORRAR
    while (total < size_body) {
        ssize_t r = send(socket_servidor_protegido, ((char*) n->body) + total, size_body - total, 0);
        if (r > 0) {
            total += r;
        }
        else if (r == 0) {
            ESP_LOGE(TAG, "Conexion interrumpida mientras se envia el body\n");
            return 1;
        }
        else if (r == -1) {
            ESP_LOGE(TAG, "Error de conexion mientras se envia el body\n");
            return 1;
        }
        num_envios++;// BORRAR
    }
    ESP_LOGI(TAG, "NUM ENVIOS: %d", num_envios);// BORRAR
    return 0;
}



