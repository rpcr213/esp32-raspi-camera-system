#ifndef NETWORK_H
#define NETWORK_H

#include "types.h"
#include "lwip/sockets.h"
#include "config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_log.h"

#define TIMEOUT_TIME 15
#define BIT_CONEXION BIT0
#define BIT_DESCONEXION BIT1




// cabeceras funciones
void init_network_mutex(void);
void init_network_event_group(void);
int init_sockets(void);
int recibir_cabecera(tCabecera* c);
int recibir_body(uint32_t size, void* body);
int enviar_nodo(tNodo* n, int size_cabecera, int size_body);
void set_event(uint32_t bit);
void clear_event(uint32_t bit);
EventBits_t wait_event(uint32_t bit);
int socket_is_active();




#endif /* NETWORK_H */