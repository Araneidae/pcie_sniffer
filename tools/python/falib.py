'''Simple FA capture library for reading data from the FA sniffer.'''

DEFAULT_SERVER = 'pc0062'
DEFAULT_PORT = 8888

import socket
import numpy
import select


MASK_SIZE = 256         # Number of possible bits in a mask

def normalise_mask(mask):
    nmask = numpy.zeros(MASK_SIZE / 8, dtype=numpy.uint8)
    for i in mask:
        nmask[i // 8] |= 1 << (i % 8)
    return nmask

def format_mask(mask):
    '''Converts a mask (a set of integers in the range 0 to 255) into a 64
    character hexadecimal coded string suitable for passing to the server.'''
    return ''.join(['%02X' % x for x in reversed(mask)])

def count_mask(mask):
    n = 0
    for i in range(MASK_SIZE):
        if mask[i // 8] & (1 << (i % 8)):
            n += 1
    return n


class connection:
    EOF = Exception()

    def __init__(self, mask, server=DEFAULT_SERVER, port=DEFAULT_PORT):
        self.mask = normalise_mask(mask)
        self.count = count_mask(self.mask)
        self.server = server
        self.port = port

        self.sock = socket.create_connection((server, port))
        self.sock.send('SR%s' % format_mask(self.mask))
        self.sock.setblocking(0)
        self.buffer = ''

    def read_raw(self, length):
        while len(self.buffer) < length:
            select.select([self.sock.fileno()], [], [])
            chunk = self.sock.recv(4096)
            if not chunk:
                raise self.EOF
            self.buffer += chunk
        result, self.buffer = self.buffer[:length], self.buffer[length:]
        return result

    def read(self, samples):
        raw = self.read_raw(8 * samples * self.count)
        array = numpy.frombuffer(raw, dtype = numpy.int32)
        return array.reshape((samples, self.count, 2))

    def close(self):
        self.sock.close()


if __name__ == '__main__':
    from pkg_resources import require
    require('cothread')
    import cothread
    select.select = cothread.select
    import sys
    c = connection(map(int, sys.argv[1:]))
    while True:
        print numpy.mean(c.read(10000), 0)
