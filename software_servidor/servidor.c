#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <sys/time.h>
#include <signal.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>

#include "queue.h"

#define PORT 3939
#define BUFFER_SIZE 1024
#define NAME_BUFFER_SIZE 256
#define HEARTBEAT_TIME 5
#define TIMEOUT_TIME 15
#define MAX_SOCKETS 10
#define MAX_NOMBRE_CARPETA 16
#define MAX_HISTORIAL_DISPOSITIVOS 30

/*
Servidor que maneja peticiones propias que se realizan desde una ESP32cam
*/

// variables globales
pthread_t thread_ping, thread_execution, thread_send, thread_receive, thread_main;
pthread_mutex_t destructor_mutex;
pthread_cond_t destructor_c;
tDispositivo dispositivo[MAX_SOCKETS];
int dispositivo_a_destruir = -1; // TODO: es solo un int, asi que solo sirve para uno, arreglar para cuando haya 2

// cabeceras de funciones
int recibir_cabecera(int socket_cliente, tCabecera* c);
int recibir_body(int socket_cliente, uint32_t size, void* body);
void *heartbeat_ping(void* arg);
void *execution_thread(void* arg);
void *send_thread(void* arg);
void *receive_thread(void* arg);
void *destructor_thread(void* arg);
void *command_thread(void* arg);
void handler(int sig);
int leer_comando(tComando *com);
int enviar_nodo(int socket_cliente, tNodo* n, int size_cabecera, int size_body);
int guardar_imagen(uint8_t* img, uint32_t size, int id);



int main(void) {
    printf("Iniciando Servidor...\n");
    tIP ip[MAX_HISTORIAL_DISPOSITIVOS];
    pthread_t thread_destructor, thread_command;
    pthread_mutex_init(&destructor_mutex, NULL);
    pthread_cond_init(&destructor_c, NULL);
    int recieve_socket_fd;
    for (int i = 0; i < MAX_SOCKETS; i++) {
        dispositivo[i].id = i;
        dispositivo[i].client_socket = -1; // marcamos cada socket como no inicializado
        dispositivo[i].client_len = sizeof(dispositivo[i].client_addr); // sus...
    }
    for (int i = 0; i < MAX_HISTORIAL_DISPOSITIVOS; i++) {
        ip[i].id = -1;
        ip[i].tiempo = -1; 
    }
    struct sockaddr_in recieve_socket_addr;
    struct timeval tv = {
        .tv_sec = TIMEOUT_TIME, // segundos
        .tv_usec = 0 // microsegundos
    };

    recieve_socket_fd = socket(AF_INET, SOCK_STREAM, 0); // IPv4 + TCP

    if (recieve_socket_fd < 0) {
        perror("Socket - recieve_socket_creation");
        return 1;
    }

    int opt = 1;
    if (setsockopt(recieve_socket_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("Socket - setsockopt");
        return 1;
    }

    printf("Socket de recepcion abierto\n");
    
    recieve_socket_addr.sin_family = AF_INET; // usamos ipv4
    recieve_socket_addr.sin_addr.s_addr = INADDR_ANY; // la direccion ip en la que vamos a escuchar
    recieve_socket_addr.sin_port = htons(PORT); // cada ip tiene varios puertos, ip -> calle, puerto -> numero de casa
    
    if (bind(recieve_socket_fd, (struct sockaddr*) &recieve_socket_addr, sizeof(recieve_socket_addr)) < 0) {
        perror("Bind - recieve_socket");
        return 1;
    }

    if (listen(recieve_socket_fd, MAX_SOCKETS)) {
        perror("Listen - recieve_socket");
        return 1;
    }

    printf("Socket configurado como escucha, escucuchando en puerto: %d\n", PORT);

    if (pthread_create(&thread_destructor, NULL, destructor_thread, NULL) != 0) {
        perror("Error al crear hilo ping");
        return 1;
    }

    if (pthread_create(&thread_command, NULL, command_thread, NULL) != 0) {
        perror("Error al crear hilo ping");
        return 1;
    }

    while (1) {
        // aceptamos un socket auxiliar y luego lo almacenamos como un dispositivo nuevo
        struct sockaddr_in aux_client_addr;
        socklen_t aux_client_len = sizeof(aux_client_addr);
        int aux_client_socket = accept(recieve_socket_fd, (struct sockaddr*)&aux_client_addr, &aux_client_len);
        // almacenamos la ip del cliente
        char ip_cliente[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(aux_client_addr.sin_addr), ip_cliente, INET_ADDRSTRLEN);
        // verificamos en el historial
        int existe = 0;
        for (int i = 0; i < MAX_HISTORIAL_DISPOSITIVOS; i++) {
            if (strcmp(ip_cliente, ip[i].ip_addr) == 0) {
                existe = 1;
                break;
            }
        }
        if (existe) { // es una reconexion
            // TODO: meter pausa o algo, es una reconexion
            printf("Reconectando el dispositivo %s...\n", ip_cliente);
            sleep(3); // ALERTA: realmente necesario
        }


        if (aux_client_socket < 0) {
            perror("Accept - aux_client_socket");
            return 1;
        }
        
        int i = 0;
        while (i < MAX_SOCKETS) {
            if (dispositivo[i].client_socket == -1) {
                dispositivo[i].id = i;
                dispositivo[i].client_socket = aux_client_socket;
                dispositivo[i].client_addr = aux_client_addr;
                dispositivo[i].client_len = aux_client_len;
                snprintf(dispositivo[i].ip_cliente, INET_ADDRSTRLEN, "%s", ip_cliente);
                setsockopt(dispositivo[i].client_socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                printf("Conexion establecida con: %s\n", dispositivo[i].ip_cliente);
                cola_init(&dispositivo[i].cola_recieve);
                cola_init(&dispositivo[i].cola_send);
                if (!existe) {
                    int max = -1, idx = -1, encontrado = 0; // en principio no deberia de haber problemas con idx valiendo -1, siempre habra algun dispositivo que sea mas viejo que otro
                    for (int j = 0; j < MAX_HISTORIAL_DISPOSITIVOS; j++) {
                        if (ip[j].id == -1) {
                            ip[j].tiempo = 0;
                            ip[j].id = i; // le asignamos el dispositivo
                            snprintf(ip[j].ip_addr, INET_ADDRSTRLEN, "%s", dispositivo[i].ip_cliente); // copiamos la ip al historial
                            encontrado = 1;
                            break;
                        }
                        else { // si entra aqui es que ya se asigno en algun momento un dispositivo
                            if (dispositivo[ip[j].id].client_socket == -1) { // el dispositivo no esta conectado
                                ip[j].tiempo++;
                                if (max < ip[j].tiempo) {
                                    max = ip[j].tiempo;
                                    idx = j;
                                }
                            }
                        }
                    }
                    if (!encontrado) {
                        if (idx == -1) {
                            printf("Error inesperado, todos los dispositivos son igual de viejos? Esta todo lleno?\n");
                            return 1;
                        }
                        else {
                            ip[idx].tiempo = 0;
                            ip[idx].id = i;
                            snprintf(ip[idx].ip_addr, IPV4_LEN, "%s", dispositivo[i].ip_cliente);
                        }
                    }
                }
                char nombre_carpeta[MAX_NOMBRE_CARPETA];
                snprintf(nombre_carpeta, MAX_NOMBRE_CARPETA, "img%d", dispositivo[i].id);
                if (mkdir(nombre_carpeta, 0755) == -1) {
                    if (errno == EEXIST) { // la carpeta existe
                        
                    }
                    else {
                        perror("main: mkdir, error al crear la carpeta");
                        return 1;
                    }
                }
                // tDispositivo* d = malloc(sizeof(tDispositivo));
                // *d = dispositivo[i]; // la cola se comparte entre todos!

                // TODO: merece la pena estar creando y destruyendo threads cada vez que se conecta un dispositivo nuevo?
                if (pthread_create(&dispositivo[i].thread_ping, NULL, heartbeat_ping, (void*) &dispositivo[i]) != 0) {
                    perror("Error al crear hilo ping");
                    return 1;
                }

                if (pthread_create(&dispositivo[i].thread_execution, NULL, execution_thread, (void*) &dispositivo[i]) != 0) {
                    perror("Error al crear execution_thread");
                    return 1;
                }

                if (pthread_create(&dispositivo[i].thread_send, NULL, send_thread, (void*) &dispositivo[i]) != 0) {
                    perror("Error al crear send_thread");
                    return 1;
                }

                if (pthread_create(&dispositivo[i].thread_receive, NULL, receive_thread, (void*) &dispositivo[i]) != 0) {
                    perror("Error al crear receive_thread");
                    return 1;
                }

                break;
            }
            i++;
        }
        if (i == MAX_SOCKETS) {
            printf("No se aceptan mas conexiones, maximo de dispositivos alcanzado\n");
            close(aux_client_socket);
        }
    }

    pthread_cond_destroy(&destructor_c);
    pthread_mutex_destroy(&destructor_mutex);
    return 0;
}


// recibe una cabecera vacia, la procesa y la devuelve 1 si fue mal 0 si fue bien
int recibir_cabecera(int socket_cliente, tCabecera* c) {
    int total = 0;
    void* cabecera = malloc(sizeof(tCabecera));

    while (total < sizeof(tCabecera)) {
        ssize_t r = recv(socket_cliente, (char*)cabecera + total, sizeof(tCabecera) - total, 0); // hacemos un parse a char para sumar correctamente los bytes
        if (r > 0) {
            total += r;
        }
        else if (r == 0) {
            printf("Conexion interrumpida mientras se recibe la cabecera\n");
            free(cabecera);
            return 1;
        }
        else if (r == -1) {
            printf("Error de conexion mientras se recibe la cabecera\n");
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                printf("Tiempo de respuesta agotado\n");
            }
            free(cabecera);
            return 1;
        }
    }

    *c = *((tCabecera*) cabecera);
    free(cabecera);
    return 0;
}


int recibir_body(int socket_cliente, uint32_t size, void* body) {
    uint32_t total = 0;
    while (total < size) {
        ssize_t r = recv(socket_cliente, (char*)body + total, size - total, 0);
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

// TODO: quizas los struct por cosas del compilador no lleguen iguales, se hacen optimizaciones diferentes -> potencial crash
// funcion que recibe un nodo y lo envia
int enviar_nodo(int socket_cliente, tNodo* n, int size_cabecera, int size_body) {
    uint32_t total = 0;
    while (total < size_cabecera) {
        ssize_t r = send(socket_cliente, ((char*) &n->cabecera) + total, size_cabecera - total, 0);
        if (r > 0) {
            total += r;
        }
        else if (r == 0) {
            printf("Conexion interrumpida mientras se envia la cabecera\n");
            return 1;
        }
        else if (r == -1) {
            printf("Error de conexion mientras se envia la cabecera\n");
            return 1;
        }
    }
    total = 0;
    while (total < size_body) {
        ssize_t r = send(socket_cliente, ((char*) n->body) + total, size_body - total, 0);
        if (r > 0) {
            total += r;
        }
        else if (r == 0) {
            printf("Conexion interrumpida mientras se envia el body\n");
            return 1;
        }
        else if (r == -1) {
            printf("Error de conexion mientras se envia el body\n");
            return 1;
        }
    }
    return 0;
}

// thread que envia un PING cada HEARTBEAT_TIME segundos
void *heartbeat_ping(void* arg) {
    tDispositivo* d = (tDispositivo*) arg;
    tCabecera cabecera_ping = {
        .tipo = PING,
        .size = 5,
    };

    while (1) {
        char* ping = malloc(5);
        memcpy(ping, "PING", 5); // <-- ya que nodo_free libera el body, hay que hacerlo asi
        encolar(&d->cola_send, cabecera_ping, (void*) ping);
        sleep(HEARTBEAT_TIME);
    }

    return NULL;
}


void *execution_thread(void* arg) {
    tDispositivo* d = (tDispositivo*) arg;
    tCabecera cabecera_pong = {
        .tipo = PONG,
        .size = 5,
    };
    while (1) {
        tNodo* nodo = desencolar(&d->cola_recieve); // funcion bloqueante
        switch (nodo->cabecera.tipo) {
            case PING: { // se ponen llaves para diferenciarlo del resto de codigo y que no se malloc cuando no es necesario
                char* pong = malloc(5);
                memcpy(pong, "PONG", 5);
                encolar(&d->cola_send, cabecera_pong, (void*) pong);
                break;
            }
            case PONG: // TODO: cambiar comportamiento
                printf("PONG!\n");
                break;
            case IMAGEN: // TODO: no se guarda la imagen
                printf("Imagen recibida, procesando...\n");
                guardar_imagen((uint8_t*) nodo->body, nodo->cabecera.size, d->id);
                break;
            case TEXTO:
                printf("Mensaje Servidor: %s\n", (char*) nodo->body);
                break;
            case COMANDO: // TODO: procesar comandos, quizas no haga falta, de momento la esp no envia comandos al servidor
                printf("Mensaje Recibido: %s\n", (char*) nodo->body);
                break;
            default:
                break;
        }
        nodo_free(nodo);
    }
    return NULL;
}

void *send_thread(void* arg) {
    tDispositivo* d = (tDispositivo*) arg;
    while (1) {
        tNodo* nodo = desencolar(&d->cola_send);
        enviar_nodo(d->client_socket, nodo, sizeof(tCabecera), nodo->cabecera.size);
        nodo_free(nodo);
    }
    return NULL;
}


void *receive_thread(void* arg) {
    tDispositivo* d = (tDispositivo*) arg;
    while (1) {
        tCabecera c;
        // TODO: no cerrar siempre que recibir_cabecera de 1, evaluar el tipo de problema
        if (recibir_cabecera(d->client_socket, &c)) { // TODO: posible race condition si hay varios dispositivos y se intercalan las cabeceras con los body (cabecera A + body B), (cabecera B + body A), por ejemplo
            pthread_mutex_lock(&destructor_mutex);
            dispositivo_a_destruir = d->id;
            pthread_cond_signal(&destructor_c); // llamamos al hilo destructor
            pthread_mutex_unlock(&destructor_mutex);
            break;
        }
        void* body = malloc(c.size);
        if (body == NULL) {
            perror("Error malloc - main code");
            break;
        }
        if (recibir_body(d->client_socket, c.size, body)) {
            free(body);
            pthread_mutex_lock(&destructor_mutex);
            dispositivo_a_destruir = d->id;
            pthread_cond_signal(&destructor_c); // llamamos al hilo destructor
            pthread_mutex_unlock(&destructor_mutex);
            break;
        }
        encolar(&d->cola_recieve, c, body);
    }
    return NULL;
}

// espera mientras no haya nada que destruir
void *destructor_thread(void* arg) {
    while (1) {
        pthread_mutex_lock(&destructor_mutex);
        while (dispositivo_a_destruir == -1) {
            pthread_cond_wait(&destructor_c, &destructor_mutex);
        }
        int dad = dispositivo_a_destruir;
        dispositivo_a_destruir = -1;
        pthread_mutex_unlock(&destructor_mutex);
        printf("El dispositivo con IP: %s no responde, preparando desconexion...\n", dispositivo[dad].ip_cliente);
        printf("Liberando los hilos...\n");
        // TODO: cancelar es chungo por si pilla a medio malloc o algo
        pthread_cancel(dispositivo[dad].thread_ping); // es como un break de un hilo pero con limpieza extra... 
        pthread_cancel(dispositivo[dad].thread_execution);
        pthread_cancel(dispositivo[dad].thread_send);
        pthread_cancel(dispositivo[dad].thread_receive);
        pthread_join(dispositivo[dad].thread_ping, NULL);
        pthread_join(dispositivo[dad].thread_execution, NULL);
        pthread_join(dispositivo[dad].thread_send, NULL);
        pthread_join(dispositivo[dad].thread_receive, NULL);
        printf("Liberando colas...\n");
        cola_destroy(&dispositivo[dad].cola_send);
        cola_destroy(&dispositivo[dad].cola_recieve);
        printf("Liberando socket...\n");
        close(dispositivo[dad].client_socket);
        dispositivo[dad].client_socket = -1;
        dispositivo[dad].client_len = sizeof(dispositivo[dad].client_addr);
        // TODO: quizas falte algo por eliminar por aqui
        printf("El dispotivo con IP: %s se ha eliminado\n", dispositivo[dad].ip_cliente);
    }
}

// formato: NUM_DISPOSITIVO,COMANDO,PARAM
void *command_thread(void* arg) {
    char comando[16];
    tCabecera c = {
        .tipo = COMANDO,
        .size = sizeof(tComando),
    };
    while (1) {
        tComando *com = malloc(c.size);
        if (leer_comando(com)) {
            free(com);
            continue;
        }
        encolar(&dispositivo[com->dispositivo].cola_send, c, com);
    }
}

int leer_comando(tComando *com) {
    char linea[256];

    if (fgets(linea, sizeof(linea), stdin) == NULL) {
        printf("Error: fallo al leer stdin\n");
        return 1;
    }

    linea[strcspn(linea, "\n")] = '\0';

    int  num_dispositivo;
    char comando_str[64];
    int  parametro;

    if (sscanf(linea, "%d,%63[^,],%d", &num_dispositivo, comando_str, &parametro) != 3) {
        printf("Error: formato incorrecto -> '%s'\n", linea);
        return 1;
    }


    if (num_dispositivo < 0 || num_dispositivo >= MAX_SOCKETS) {
        printf("Error: num_dispositivo %d fuera de rango [0, %d)\n", num_dispositivo, MAX_SOCKETS);
        return 1;
    }

    if (dispositivo[num_dispositivo].client_socket == -1) {
        printf("Error: dispositivo %d no esta conectado\n", num_dispositivo);
        return 1;
    }

    // TODO: aniadir comandos
    tTiposComandos tipo;
    if      (strcmp(comando_str, "flash")       == 0) tipo = FLASH;
    else if (strcmp(comando_str, "foto")        == 0) tipo = FOTO;
    else if (strcmp(comando_str, "tiempo_foto") == 0) tipo = TIEMPO_FOTO;
    else {
        printf("Error: comando desconocido -> '%s'\n", comando_str);
        return 1;
    }

    com->dispositivo = num_dispositivo;
    com->comando     = tipo;
    com->parametro   = parametro;
    return 0;
}

// debe de existir la carpeta img(n dispositivo)
int guardar_imagen(uint8_t* img, uint32_t size, int id) {
    char name[NAME_BUFFER_SIZE];
    time_t t = time(NULL);
    struct tm* st_tiempo = localtime(&t);
    char tiempo[80];
    strftime(tiempo, 80, "%H-%M-%S_%Y-%m-%d", st_tiempo);
    snprintf(name, NAME_BUFFER_SIZE, "img%d/img_%s.jpg", id, tiempo); // TODO: poner dispositivo

    int fd = open(name, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd == -1) {
        perror("guardar_imagen: el archivo ya existe o error");
        return 1;
    }

    uint8_t *ptr = img;
    size_t restantes = size;

    while (restantes > 0) {
        ssize_t escritos = write(fd, ptr, restantes);
        if (escritos == -1) {
            perror("guardar_imagen: error al escribir");
            close(fd);
            return 1;
        }
        ptr += escritos;
        restantes -= escritos;
    }

    close(fd);
    printf("Imagen: %s guardada correctamente\n", name);

    return 0;
}

/*
void handler(int sig) {
    printf("Preparando para finalizar...\n");
    printf("Liberando los hilos...\n");
    pthread_cancel(thread_ping); // es como un break de un hilo pero con limpieza extra... 
    pthread_cancel(thread_execution);
    pthread_cancel(thread_send);
    pthread_cancel(thread_receive);
    pthread_join(thread_ping, NULL);
    pthread_join(thread_execution, NULL);
    pthread_join(thread_send, NULL);
    pthread_join(thread_receive, NULL);

    printf("Liberando colas...\n");
    cola_destroy(&cola_send);
    cola_destroy(&cola_recieve);
    printf("Liberando sockets...\n");

    close(client_socket_fd);
    close(recieve_socket_fd);
    printf("Fin del programa\n");
    exit(0);
}
*/
