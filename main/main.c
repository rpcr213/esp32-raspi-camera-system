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
#include "wifi.h"
#include "network.h"

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

#define TIMER_TIME_PREDETERMINADO 30000000 // tiempo en us


#define HEARTBEAT_TIME 5
#define CHECK_CONNECTION_TIME 1000



// variables globales
static const char* TAG = "MAIN";
static TaskHandle_t heartbeat_ping_handle, execution_thread_handle, send_thread_handle, receive_thread_handle, connection_thread_handle, capture_thread_handle;
static QueueHandle_t send_queue, receive_queue;
static esp_timer_handle_t timer;


// cabeceras funciones
void init_gpio(void);
int delete_timer(esp_timer_handle_t* p_timer);
int create_timer(esp_timer_handle_t* p_timer, uint64_t time);
void execution_thread(void* args);
void send_thread(void* args);
void heartbeat_ping(void* args);
void receive_thread(void* args); // TODO: limpiar basura que quedo en colas y cosas asi cuando se reconecta, para todos los threads que se paren mientras se reconecta
void connection_thread(void* args);
void capture_thread(void* args);
int command_execute(tComando c);
void timer_callback(void* arg);
void flash(camera_fb_t** fb);
int enviar_texto_servidor(char* msj);


void app_main(void) {
    init_network_mutex();
    init_network_event_group();
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
    set_event(BIT_DESCONEXION);

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

// hilo que ejecuta el heartbeat
void heartbeat_ping(void* args) {
    tCabecera cabecera_ping = {
        .tipo = PING,
        .size = 5,
    };

    while (1) {
        EventBits_t bits = wait_event(BIT_CONEXION);
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
        EventBits_t bits = wait_event(BIT_CONEXION);
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
        EventBits_t bits = wait_event(BIT_CONEXION);
        tNodo nodo;
        if (xQueueReceive(send_queue, &nodo, pdMS_TO_TICKS(CHECK_CONNECTION_TIME)) != pdTRUE) {
            continue;
        }
        int64_t t10 = esp_timer_get_time();// BORRAR
        if (enviar_nodo(&nodo, sizeof(tCabecera), nodo.cabecera.size)) {
            clear_event(BIT_CONEXION);
            set_event(BIT_DESCONEXION);
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
        EventBits_t bits = wait_event(BIT_CONEXION);
        tCabecera c;
        if (recibir_cabecera(&c)) {
            clear_event(BIT_CONEXION);
            set_event(BIT_DESCONEXION);
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
            clear_event(BIT_CONEXION);
            set_event(BIT_DESCONEXION);
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
        wait_wifi();
        EventBits_t bits = wait_event(BIT_DESCONEXION);
        clear_event(BIT_DESCONEXION);
        // ALERTA: proteger el soccket aqui?
        if (socket_is_active()) {
            xQueueReset(send_queue);
            xQueueReset(receive_queue);
        }
        while (init_sockets()) {
            vTaskDelay(pdMS_TO_TICKS(3000)); // TODO: quizas esta sincrinizacion con la aceptacion del socket esta rota
        }
        set_event(BIT_CONEXION);
    }
}


void capture_thread(void* args) {
    while (1) {
        EventBits_t bits = wait_event(BIT_CONEXION);
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

