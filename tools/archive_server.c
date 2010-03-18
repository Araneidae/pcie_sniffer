/* Simple server for archive data.
 *
 */

#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <signal.h>

#include "error.h"


bool start_server(int port, int *sock)
{
    struct sockaddr_in sin = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(port)
    };
    
    return
        TEST_IO(*sock = socket(AF_INET, SOCK_STREAM, 0))  &&
        TEST_IO(bind(*sock, (struct sockaddr *) &sin, sizeof(sin)))  &&
        TEST_IO(listen(*sock, 5))  &&
        DO_(printf("Server listening on port %d\n", port));
}


void process_connection(int scon)
{
    char buf[4096];
    ssize_t rx;
    if (TEST_IO(rx = read(scon, buf, sizeof(buf)))  &&  rx > 0)
    {
        printf("Read: \"%.*s\"\n", rx, buf);
    }
    printf("Some input read\n");
    sprintf(buf, "HTTP/1.0 200 OK\r\n\r\n<HTML><BODY>Ok!</BODY></HTML>\r\n");
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
    if (start_server(8888, &sock))
        run_server(sock);
    return 0;
}
