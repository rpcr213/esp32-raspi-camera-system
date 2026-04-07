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

typedef struct tNodo {
    tCabecera cabecera;
    void* body; // CRITICO: LIBERAR BODY AL EXTRAER 
} tNodo; // para un comportamiento correcto, body debe de ser dinamico

#endif
