/* Interface to archive server. */

bool initialise_server(int port);
void terminate_server(void);

/* Reports error status on the connected socket and calls pop_error_handling().
 * If there is no error to report then a single null byte is written to the
 * socket to signal a valid status. */
bool report_socket_error(int scon, bool ok);
