#ifndef TYPES_H
#define TYPES_H
#include <stdint.h>

// tipos
typedef enum {
    PING,
    PONG,
    TEXTO,
    IMAGEN,
    COMANDO,
} tTipo;

typedef enum {
    FLASH, // activa/desactiva el flash
    FOTO, // toma una foto en ese momento
    TIEMPO_FOTO, // modifica el tiempo entre foto y foto (osea el timer)
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

typedef struct tNodo {
    tCabecera cabecera;
    void* body; // CRITICO: LIBERAR BODY AL EXTRAER 
} tNodo; // para un comportamiento correcto, body debe de ser dinamico

#endif
