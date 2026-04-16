#include "network.h"


static const char* TAG = "NETWORK";
static EventGroupHandle_t connection_event_group_handle;
static int server_socket = -1;
static SemaphoreHandle_t server_socket_m;




void init_network_mutex(void) {
    server_socket_m = xSemaphoreCreateMutex();
}

void init_network_event_group(void) {
    connection_event_group_handle = xEventGroupCreate();
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



int enviar_nodo(tNodo* n, int size_cabecera, int size_body) {
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


void set_event(uint32_t bit) {
    xEventGroupSetBits(connection_event_group_handle, bit);
}

// 
void clear_event(uint32_t bit) {
    xEventGroupClearBits(connection_event_group_handle, bit);
}

// Funcion que espera a que se levante un bit y tras salir NO limpia el bit levantado automaticamente
EventBits_t wait_event(uint32_t bit) {
    EventBits_t ret = xEventGroupWaitBits(connection_event_group_handle, BIT_CONEXION, pdFALSE, pdTRUE, portMAX_DELAY);
    return ret;
}

int socket_is_active() {
    return (server_socket >= 0);
}

