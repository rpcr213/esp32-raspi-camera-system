#ifndef QUEUE_H
#define QUEUE_H
#include <pthread.h>
#include <stdio.h>
#include "types.h"

 
void cola_init(tCola* cola);
void cola_destroy(tCola* cola);
void encolar(tCola* cola, tCabecera cabecera, void* body);
tNodo* desencolar(tCola* cola); // bloquea hasta que haya un elemento
tNodo* desencolar_nowait(tCola* cola); // devuelve NULL si esta vacia
void nodo_free(tNodo* nodo);


#endif