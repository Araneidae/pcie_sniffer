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
    class EOF(Exception):
        pass

    def __init__(self, server=DEFAULT_SERVER, port=DEFAULT_PORT):
        self.sock = socket.create_connection((server, port))
        self.sock.setblocking(0)
        self.buffer = ''

    def close(self):
        self.sock.close()

    def recv(self):
        select.select([self.sock.fileno()], [], [])
        chunk = self.sock.recv(4096)
        if not chunk:
            raise self.EOF('Connection closed by server')
        return chunk

    def read_block(self, length):
        while len(self.buffer) < length:
            self.buffer += self.recv()
        result, self.buffer = self.buffer[:length], self.buffer[length:]
        return result


class subscription(connection):
    def __init__(self, mask, **kargs):
        connection.__init__(self, **kargs)
        self.mask = normalise_mask(mask)
        self.count = count_mask(self.mask)

        self.sock.send('SR%s\n' % format_mask(self.mask))

    def read(self, samples):
        raw = self.read_block(8 * samples * self.count)
        array = numpy.frombuffer(raw, dtype = numpy.int32)
        return array.reshape((samples, self.count, 2))


class sample_frequency(connection):
    def __init__(self, **kargs):
        connection.__init__(self, **kargs)
        self.sock.send('CF\n')
        self.frequency = float(self.recv())


def get_sample_frequency(**kargs):
    try:
        return sample_frequency(**kargs).frequency
    except:
        # If get fails fall back to nominal default.
        return 10072.0


if __name__ == '__main__':
    from pkg_resources import require
    require('cothread')
    import cothread
    select.select = cothread.select
    f = sample_frequency()
    import sys
    s = subscription(map(int, sys.argv[1:]))
    while True:
        print numpy.mean(s.read(10000), 0)
