#include <stdlib.h>
#include "queue.h"
 
void cola_init(tCola* cola) {
    cola->head = NULL;
    cola->tail = NULL;
    cola->size = 0;
    pthread_mutex_init(&cola->mutex, NULL);
    pthread_cond_init(&cola->cond, NULL);
}
 
void cola_destroy(tCola* cola) {
    tNodo* nodo = cola->head;
    while (nodo) {
        tNodo* siguiente = nodo->next;
        nodo_free(nodo);
        nodo = siguiente;
    }
    // printf("Destruccion de nodos completada\n");
    pthread_mutex_destroy(&cola->mutex);
    pthread_cond_destroy(&cola->cond);
    // printf("Destruccion de mutex y varcond completadas\n");
}
 
void encolar(tCola* cola, tCabecera cabecera, void* body) {
    tNodo* nodo = malloc(sizeof(tNodo));
    nodo->cabecera = cabecera;
    nodo->body = body;
    nodo->next = NULL;
 
    pthread_mutex_lock(&cola->mutex);
 
    if (cola->tail) cola->tail->next = nodo;
    else cola->head = nodo;
    cola->tail = nodo;
    cola->size++;
 
    pthread_cond_signal(&cola->cond);
    pthread_mutex_unlock(&cola->mutex);
}
 
tNodo* desencolar(tCola* cola) {
    pthread_mutex_lock(&cola->mutex);
 
    while (!cola->head) {
        pthread_cond_wait(&cola->cond, &cola->mutex);
    }

    tNodo* nodo = cola->head;
    cola->head = nodo->next;
    if (!cola->head) cola->tail = NULL;
    cola->size--;
 
    pthread_mutex_unlock(&cola->mutex);
 
    nodo->next = NULL;
    return nodo;
}
 
tNodo* desencolar_nowait(tCola* cola) {
    pthread_mutex_lock(&cola->mutex);
 
    if (!cola->head) {
        pthread_mutex_unlock(&cola->mutex);
        return NULL;
    }
 
    tNodo* nodo = cola->head;
    cola->head = nodo->next;
    if (!cola->head) cola->tail = NULL;
    cola->size--;
 
    pthread_mutex_unlock(&cola->mutex);
 
    nodo->next = NULL;
    return nodo;
}
 
void nodo_free(tNodo* nodo) {
    free(nodo->body);
    free(nodo);
}