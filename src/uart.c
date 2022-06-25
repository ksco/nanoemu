#include "nanoemu.h"

static void*
uart_thread(void* opaque) {
    struct uart* uart = opaque;
    while (1) {
        char c;
        read(STDIN_FILENO, &c, 1);
        pthread_mutex_lock(&uart->lock);
        while ((uart->data[UART_LSR - UART_BASE] & UART_LSR_RX) == 1) {
            pthread_cond_wait(&uart->cond, &uart->lock);
        }
        uart->data[0] = c;
        uart->interrupting = true;
        uart->data[UART_LSR - UART_BASE] |= UART_LSR_RX;
        pthread_mutex_unlock(&uart->lock);
    }

    /* Unreachable. */
    return NULL;
}

struct uart*
uart_new() {
    struct uart* uart = calloc(1, sizeof *uart);
    uart->data[UART_LSR - UART_BASE] |= UART_LSR_TX;
    pthread_mutex_init(&uart->lock, NULL);
    pthread_cond_init(&uart->cond, NULL);

    pthread_create(&uart->tid, NULL, uart_thread, (void*)uart);
    return uart;
}

enum exception
uart_load(struct uart* uart, uint64_t addr, uint64_t size, uint64_t *result) {
    switch (size) {
    case 8:
        pthread_mutex_lock(&uart->lock);
        switch (addr) {
        case UART_RHR:
            pthread_cond_broadcast(&uart->cond);
            uart->data[UART_LSR - UART_BASE] &= ~UART_LSR_RX;
        default:
            *result = uart->data[addr - UART_BASE];
        }
        pthread_mutex_unlock(&uart->lock);
        return OK;
    default:
        return LOAD_ACCESS_FAULT;
    }
}

enum exception
uart_store(struct uart* uart, uint64_t addr, uint64_t size, uint64_t value) {
    switch (size) {
    case 8:
        pthread_mutex_lock(&uart->lock);
        switch (addr) {
        case UART_THR:
            printf("%c", (char)(value & 0xff));
            fflush(stdout);
            break;
        default:
            uart->data[addr - UART_BASE] = value & 0xff;
        }
        pthread_mutex_unlock(&uart->lock);
        return OK;
    default:
        return STORE_AMO_ACCESS_FAULT;
    }
}

bool
uart_interrupting(struct uart* uart) {
    pthread_mutex_lock(&uart->lock);
    bool interrupting = uart->interrupting;
    uart->interrupting = false;
    pthread_mutex_unlock(&uart->lock);
    return interrupting;
}
