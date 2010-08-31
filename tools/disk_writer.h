/* Interface to archive to disk writer. */

/* First stage of disk writer initialisation: opens the archive file and loads
 * the header into memory.  Can be called before initialising buffers. */
bool initialise_disk_writer(const char *file_name, uint32_t *input_block_size);
/* Starts writing files to disk.  Must be called after initialising the buffer
 * layer. */
bool start_disk_writer(void);
/* Orderly shutdown of the disk writer. */
void terminate_disk_writer(void);
