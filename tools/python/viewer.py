#!/usr/bin/env dls-python2.6

'''Form Example with Monitor'''

import os, sys

from pkg_resources import require
require('cothread')
import cothread

# Nasty hack.  Need to integrate this into cothread at some point.
import select
select.select = cothread.select


# from numpy import *
import numpy
from PyQt4 import Qwt5, QtGui, QtCore, uic

cothread.iqt()

# Qt designer form class (widget is actually QtGui.QWidget)
viewer_ui_file = os.path.join(os.path.dirname(__file__), 'viewer.ui')
Ui_Viewer, widget = uic.loadUiType(viewer_ui_file)


sys.path.append(os.path.dirname(__file__))
import falib


class buffer:
    '''Circular buffer.'''
    # Super lazy implementation: we always just copy the data to the bottom!

    def __init__(self, buffer_size):
        self.buffer = numpy.zeros((buffer_size, 2))
        self.buffer_size = buffer_size

    def write(self, block):
        blen = len(block)
        self.buffer[:-blen] = self.buffer[blen:]
        self.buffer[-blen:] = block

    def size(self):
        return self.data_size

    def read(self, size):
        return self.buffer[-size:]

    def reset(self):
        self.buffer[:] = 0


class monitor:
    def __init__(self, on_event, on_eof, buffer_size, read_size):
        self.on_event = on_event
        self.on_eof = on_eof
        self.buffer = buffer(buffer_size)
        self.read_size = read_size
        self.running = False

    def start(self):
        assert not self.running
        self.connection = falib.connection([self.id])
        self.running = True
        self.buffer.reset()
        self.task = cothread.Spawn(self.__monitor)

    def stop(self):
        assert self.running
        self.running = False
        self.task.Wait()
        self.connection.close()

    def set_id(self, id):
        running = self.running
        if running:
            self.stop()
        self.id = id
        if running:
            self.start()

    def resize(self, notify_size, update_size):
        '''The notify_size is the data size delivered in each update, while
        the update_size determines how frequently an update is delivered.'''
        self.notify_size = notify_size
        self.update_size = update_size
        self.data_ready = 0

    def __monitor(self):
        try:
            while self.running:
                self.buffer.write(self.connection.read(self.read_size)[:,0,:])
                self.data_ready += self.read_size
                if self.data_ready >= self.update_size:
                    self.on_event(self.buffer.read(self.notify_size) * 1e-6)
                    self.data_ready -= self.update_size
        except falib.connection.EOF:
            self.on_eof()


BPM_list = [
    'SR%02dC-DI-EBPM-%02d' % (c+1, n+1)
    for c in range(24) for n in range(7)]

Timebase_list = [
    ('100ms', 1000),    ('250ms', 2500),    ('0.5s', 5000),
    ('1s', 10000),      ('2.5s', 25000),    ('5s', 50000),
    ('10s', 100000),    ('25s', 250000),    ('50s', 500000)]

SCROLL_THRESHOLD = 10000


# subclass form to implement buttons
class Viewer(widget, Ui_Viewer):
    '''application class'''
    def __init__(self):
        widget.__init__(self)
        self.setupUi(self)

        # make any contents fill the empty frame
        grid = QtGui.QGridLayout(self.axes)
        self.axes.setLayout(grid)
        self.makeplot()

        # Prepare the selections in the controls
        self.monitor = monitor(
            self.on_event, self.on_eof,
            Timebase_list[-1][1], Timebase_list[0][1])
        self.timebase.addItems([l[0] for l in Timebase_list])
        self.mode.addItems(['Raw Signal', 'FFT', 'Integrated'])
        self.channel.addItems(BPM_list)

        # Select initial state
        self.monitor.set_id(1)
        self.select_timebase(0)
        self.monitor.start()

        # Make the initial connections
        self.connect(self.channel,
            QtCore.SIGNAL('currentIndexChanged(int)'), self.select_channel)
        self.connect(self.rescale,
            QtCore.SIGNAL('clicked()'), self.rescale_graph)
        self.connect(self.mode,
            QtCore.SIGNAL('currentIndexChanged(int)'), self.select_mode)
        self.connect(self.run,
            QtCore.SIGNAL('clicked(bool)'), self.toggle_running)
        self.connect(self.timebase,
            QtCore.SIGNAL('currentIndexChanged(int)'), self.select_timebase)


    def makeplot(self):
        '''set up plotting'''
        # draw a plot in the frame
        p = Qwt5.QwtPlot(self.axes)
        cx = Qwt5.QwtPlotCurve('X')
        cy = Qwt5.QwtPlotCurve('Y')
        cx.setPen(QtGui.QPen(QtCore.Qt.blue))
        cy.setPen(QtGui.QPen(QtCore.Qt.red))
        cy.attach(p)
        cx.attach(p)

        # === Plot Customization ===
        # set background to black
        p.setCanvasBackground(QtCore.Qt.black)
        # stop flickering border
        p.canvas().setFocusIndicator(Qwt5.QwtPlotCanvas.NoFocusIndicator)
        # set fixed scale
        p.setAxisScale(Qwt5.QwtPlot.yLeft, -0.1, 0.1)

        self.p = p
        self.cx = cx
        self.cy = cy
        self.axes.layout().addWidget(self.p)


    # --------------------------------------------------------------------------
    # GUI event handlers

    def select_channel(self, ix):
        self.monitor.set_id(ix + 1)

    def rescale_graph(self):
        self.p.setAxisScale(Qwt5.QwtPlot.yLeft,
            1.2 * numpy.amin(self.value), 1.2 * numpy.amax(self.value))
        self.p.replot()

    def select_timebase(self, ix):
        new_timebase = Timebase_list[ix][1]
        self.p.setAxisScale(Qwt5.QwtPlot.xBottom, 0, new_timebase)
        self.monitor.resize(new_timebase, min(new_timebase, SCROLL_THRESHOLD))

    def select_mode(self, ix):
        print 'new mode', ix

    def toggle_running(self, running):
        if running:
            self.monitor.start()
        else:
            self.monitor.stop()


    # --------------------------------------------------------------------------
    # Data event handlers

    def on_event(self, value):
        self.value = value
        t = numpy.arange(value.shape[0])
        self.cx.setData(t, value[:, 0])
        self.cy.setData(t, value[:, 1])
        self.p.replot()

    def on_eof(self):
        print 'EOF on channel detected'
        self.run.setCheckState(False)
        self.monitor.stop()


# create and show form
s = Viewer()
s.show()

cothread.WaitForQuit()
