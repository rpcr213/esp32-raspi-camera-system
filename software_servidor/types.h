#ifndef TYPES_H
#define TYPES_H
#include <stdint.h>
#include <arpa/inet.h>
#include <pthread.h>

#define IPV4_LEN 16

typedef enum {
    PING,
    PONG,
    TEXTO,
    IMAGEN,
    COMANDO,
} tTipo;

typedef enum {
    FLASH,
    FOTO,
    TIEMPO_FOTO,
} tTiposComandos;

typedef struct {
    int dispositivo;
    tTiposComandos comando;
    int parametro;
} tComando;

typedef struct {
    tTipo tipo; // tipo de mensaje: ping, texto, imagen...
    uint32_t size; // tamanio del mensaje
} tCabecera;

// de tCola
typedef struct tNodo {
    tCabecera cabecera;
    void* body;
    struct tNodo* next;
} tNodo;
 
typedef struct {
    tNodo* head;
    tNodo* tail;
    int size;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} tCola;

typedef struct {
    int id; // el id del dispositivo
    char ip_addr[INET_ADDRSTRLEN];
    int tiempo; // la tIP con mas tiempo sera sustituida, -1 es que no se esta usando ese addr
} tIP;


typedef struct {
    int id;
    int client_socket;
    struct sockaddr_in client_addr;
    socklen_t client_len;
    char ip_cliente[INET_ADDRSTRLEN];
    pthread_t thread_ping, thread_execution, thread_send, thread_receive;
    tCola cola_send, cola_recieve;
} tDispositivo;

#endif
