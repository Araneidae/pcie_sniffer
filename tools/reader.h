/* Implements reading from disk. */

/* scon is the connected socket and buf is the command read from the user: rx
 * bytes have already been received and the buffer is buf_size bytes long.
 * The first character in the buffer is R. */
void process_read(int scon, char *buf, ssize_t rx, size_t buf_size);
