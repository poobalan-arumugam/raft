#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>

#include "tcp.h"

void test_tcp_setup(const MunitParameter params[], struct test_tcp *t)
{
    struct sockaddr_in addr;
    socklen_t size = sizeof addr;
    int rv;

    (void)params;

    /* Initialize the socket address structure. */
    memset(&addr, 0, size);

    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = 0; /* Get a random free port */

    /* Create the server socket. */
    t->server.socket = socket(AF_INET, SOCK_STREAM, 0);
    if (t->server.socket == -1) {
        munit_errorf("tcp: socket(): %s", strerror(errno));
    }

    /* Bind the socket. */
    rv = bind(t->server.socket, (struct sockaddr *)&addr, size);
    if (rv == -1) {
        munit_errorf("tcp: bind(): %s", strerror(errno));
    }

    /* Start listening. */
    rv = listen(t->server.socket, 1);
    if (rv == -1) {
        munit_errorf("tcp: listen(): %s", strerror(errno));
    }

    /* Get the actual addressed assigned by the kernel and save it back in
     * the relevant test_socket__server field (pointed to by address). */
    rv = getsockname(t->server.socket, (struct sockaddr *)&addr, &size);
    if (rv != 0) {
        munit_errorf("tcp: getsockname(): %s", strerror(errno));
    }

    sprintf(t->server.address, "127.0.0.1:%d", htons(addr.sin_port));

    t->client.socket = -1;
}

void test_tcp_tear_down(struct test_tcp *t)
{
    int rv;

    rv = close(t->server.socket);
    if (rv == -1) {
        munit_errorf("tcp: close(): %s", strerror(errno));
    }

    if (t->client.socket != -1) {
        rv = close(t->client.socket);
        if (rv == -1) {
            munit_errorf("tcp: close(): %s", strerror(errno));
        }
    }
}

void test_tcp_connect(struct test_tcp *t, int port)
{
    struct sockaddr_in addr;
    int rv;

    /* Create the client socket. */
    t->client.socket = socket(AF_INET, SOCK_STREAM, 0);
    if (t->client.socket == -1) {
        munit_errorf("tcp: socket(): %s", strerror(errno));
    }

    /* Initialize the socket address structure. */
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons(port);

    /* Connect */
    rv = connect(t->client.socket, (struct sockaddr *)&addr, sizeof addr);
    if (rv == -1) {
        munit_errorf("tcp: connect(): %s", strerror(errno));
    }
}

void test_tcp_send(struct test_tcp *t, const void *buf, int len)
{
    int rv;

    rv = write(t->client.socket, buf, len);
    if (rv == -1) {
        munit_errorf("tcp: write(): %s", strerror(errno));
    }
    if (rv != len) {
        munit_errorf("tcp: write(): only %d bytes written", rv);
    }
}
