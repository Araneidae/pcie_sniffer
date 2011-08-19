from pkg_resources import require
require('cothread')
import cothread
import falib
import numpy

s = falib.subscription(range(1,169))

while True:
    r = s.read(10000)
    print r.shape
