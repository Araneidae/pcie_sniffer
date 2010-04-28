/* Simple UNIX socket server for live FA data. */

/* Socket guide nicely given in
 *  http://beej.us/guide/bgipc/output/html/multipage/unixsock.html
 */


#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "error.h"


#define SOCKET_PATH "./mysocket"


bool start_server(int *sock)
{
    struct sockaddr_un sun = { .sun_family = AF_UNIX };
    strcpy(sun.sun_path, SOCKET_PATH);
    int len = sizeof(sun.sun_family) + strlen(sun.sun_path) + 1;
    unlink(sun.sun_path);   // In case the socket already exists...

    return
        TEST_IO(*sock = socket(AF_UNIX, SOCK_STREAM, 0))  &&
        TEST_IO(bind(*sock, (struct sockaddr *) &sun, len))  &&
        TEST_IO(listen(*sock, 5))  &&
        DO_(printf("Server listening on socket %s\n", sun.sun_path));
}


void process_connection(int scon)
{
    printf("process_connection %d\n", scon);
    char buf[4096];
    ssize_t rx;
    if (TEST_IO(rx = read(scon, buf, sizeof(buf)))  &&  rx > 0)
    {
        printf("Read: \"%.*s\"\n", rx, buf);
    }
    printf("Some input read\n");
    sprintf(buf, "Howdy!\n");
    TEST_IO(write(scon, buf, strlen(buf)));
}



void run_server(int sock)
{
    int scon;
    while (TEST_IO(scon = accept(sock, NULL, NULL)))
    {
        process_connection(scon);
        TEST_IO(close(scon));
    }
}


int main(int argc, char **argv)
{
    int sock;
    if (start_server(&sock))
        run_server(sock);
    return 0;
}
