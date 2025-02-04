#include <zmq.h>
#include <stdio.h>
#include <stdlib.h>

int main() {
    void *context = zmq_ctx_new();
    int io_threads = 4;
    zmq_ctx_set(context, ZMQ_IO_THREADS, io_threads);
    printf("Running with %d I/O threads, %s\n",
      io_threads,
      (zmq_ctx_get(context, ZMQ_IO_THREADS) == io_threads) ? "success" : "fail");
    // XSUB socket for publishers
    void *xsub_socket = zmq_socket(context, ZMQ_XSUB);
    zmq_bind(xsub_socket, "tcp://0.0.0.0:5555");
    
    // XPUB socket for subscribers
    void *xpub_socket = zmq_socket(context, ZMQ_XPUB);
    zmq_bind(xpub_socket, "tcp://0.0.0.0:5556");
    printf("Broker is running using zmq proxy\n");
    // Proxy between XSUB and XPUB sockets
    zmq_proxy(xsub_socket, xpub_socket, NULL);

    // Clean up
    zmq_close(xsub_socket);
    zmq_close(xpub_socket);
    zmq_ctx_destroy(context);

    return 0;
}