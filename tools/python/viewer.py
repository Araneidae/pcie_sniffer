#!/usr/bin/env dls-python2.6

'''Form Example with Monitor'''

import os, sys

from pkg_resources import require
require('cothread')
import cothread

# Nasty hack.  Need to integrate this into cothread at some point.
import select
select.select = cothread.select

sys.path.append(os.path.dirname(__file__))
import falib

import numpy
from PyQt4 import Qwt5, QtGui, QtCore, uic


cothread.iqt()


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
        self.update_size = read_size
        self.notify_size = read_size
        self.data_ready = 0
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
                    self.on_event(self.buffer.read(self.notify_size))
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

F_S = 10072

char_mu     = u'\u03BC'             # Greek mu Unicode character
char_sqrt   = u'\u221A'             # Mathematical square root sign


class mode_raw:
    mode_name = 'Raw Signal'
    yname = 'Position (%sm)' % char_mu
    xscale = Qwt5.QwtLinearScaleEngine
    yscale = Qwt5.QwtLinearScaleEngine
    xmin = 0

    def __init__(self, timebase):
        if timebase <= 10000:
            self.xname = 'Time (ms)'
            scale = 1e3
        else:
            self.xname = 'Time (s)'
            scale = 1.0
        self.xmax = scale / F_S * timebase
        self.xaxis = scale / F_S * numpy.arange(timebase)

    def compute(self, value):
        return 1e-3 * value


class mode_fft:
    mode_name = 'FFT'
    xname = 'Frequency (kHz)'
    yname = 'Amplitude (%sm/%sHz)' % (char_mu, char_sqrt)
    xscale = Qwt5.QwtLinearScaleEngine
    yscale = Qwt5.QwtLog10ScaleEngine
    xmin = 0

    def __init__(self, timebase):
        self.xmax = 1e-3 * F_S / 2
        N2 = timebase // 2
        self.xaxis = self.xmax * numpy.arange(N2) / N2

    def compute(self, value):
        N = len(value)
        fft = numpy.fft.fft(1e-3 * value, axis=0)[:N//2]
        return numpy.abs(fft) * numpy.sqrt(2.0 / (F_S * N))

class mode_fft_logt(mode_fft):
    mode_name = 'FFT (log t)'
    xname = 'Frequency (Hz)'
    xscale = Qwt5.QwtLog10ScaleEngine

    def __init__(self, timebase):
        mode_fft.__init__(self, timebase)
        self.xaxis = 1e3 * self.xaxis[1:]
        self.xmin = self.xaxis[0]
        self.xmax = self.xaxis[-1]

    def compute(self, value):
        return mode_fft.compute(self, value)[1:]


class mode_integrated:
    mode_name = 'Integrated'
    xname = 'Frequency (Hz)'
    yname = 'Cumulative amplitude (%sm)' % char_mu
    xscale = Qwt5.QwtLog10ScaleEngine
    yscale = Qwt5.QwtLog10ScaleEngine

    def __init__(self, timebase):
        self.len = timebase / 2
        self.xmax = 0.5 * F_S
        self.xaxis = self.xmax * numpy.arange(self.len)[1:] / self.len
        self.xmin = self.xaxis[0]

    def compute(self, value):
        N = len(value)
        fft = numpy.fft.fft(1e-3 * value, axis=0)[1 : N//2]
        return numpy.sqrt(2 * numpy.cumsum(numpy.abs(fft**2), axis=0)) / N



Display_modes = [mode_raw, mode_fft, mode_fft_logt, mode_integrated]


class Viewer:
    '''application class'''
    def __init__(self, ui):
        self.ui = ui

        # make any contents fill the empty frame
        grid = QtGui.QGridLayout(ui.axes)
        ui.axes.setLayout(grid)
        self.makeplot()

        self.monitor = monitor(self.on_event, self.on_eof, 500000, 1000)

        # Prepare the selections in the controls
        ui.timebase.addItems([l[0] for l in Timebase_list])
        ui.mode.addItems([l.mode_name for l in Display_modes])
        ui.channel.addItems(BPM_list)

        # Select initial state
        self.monitor.set_id(1)
        self.current_timebase = Timebase_list[0][1]
        self.mode_class = Display_modes[0]
        self.reset_mode()
        self.monitor.start()

        # Make the initial connections
        self.connect(ui.channel,
            'currentIndexChanged(int)', self.select_channel)
        self.connect(ui.timebase,
            'currentIndexChanged(int)', self.select_timebase)
        self.connect(ui.rescale, 'clicked()', self.rescale_graph)
        self.connect(ui.mode, 'currentIndexChanged(int)', self.select_mode)
        self.connect(ui.run, 'clicked(bool)', self.toggle_running)

    def connect(self, control, signal, action):
        self.ui.connect(control, QtCore.SIGNAL(signal), action)

    def makeplot(self):
        '''set up plotting'''
        # draw a plot in the frame
        p = Qwt5.QwtPlot(self.ui.axes)
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
        self.ui.axes.layout().addWidget(p)


    # --------------------------------------------------------------------------
    # GUI event handlers

    def select_channel(self, ix):
        self.monitor.set_id(ix + 1)

    def rescale_graph(self):
        self.p.setAxisScale(Qwt5.QwtPlot.yLeft,
            1.2 * numpy.nanmin(self.value), 1.2 * numpy.nanmax(self.value))
        self.p.replot()

    def select_timebase(self, ix):
        new_timebase = Timebase_list[ix][1]
        self.current_timebase = new_timebase
        self.monitor.resize(new_timebase, min(new_timebase, SCROLL_THRESHOLD))
        self.reset_mode()

    def select_mode(self, ix):
        self.mode_class = Display_modes[ix]
        self.reset_mode()

    def toggle_running(self, running):
        if running:
            self.monitor.start()
        else:
            self.monitor.stop()


    # --------------------------------------------------------------------------
    # Data event handlers

    def reset_mode(self):
        mode = self.mode_class(self.current_timebase)
        self.compute = mode.compute
        self.xaxis = mode.xaxis

        x = Qwt5.QwtPlot.xBottom
        y = Qwt5.QwtPlot.yLeft
        self.p.setAxisTitle(x, mode.xname)
        self.p.setAxisTitle(y, mode.yname)
        self.p.setAxisScale(x, mode.xmin, mode.xmax)
        self.p.setAxisScaleEngine(x, mode.xscale())
        self.p.setAxisScaleEngine(y, mode.yscale())

    def on_event(self, value):
        v = self.compute(value)
        self.value = v
        self.cx.setData(self.xaxis, v[:, 0])
        self.cy.setData(self.xaxis, v[:, 1])
        self.p.replot()

    def on_eof(self):
        print 'EOF on channel detected'
        self.ui.run.setCheckState(False)
        self.monitor.stop()


# create and show form
ui_viewer = uic.loadUi(os.path.join(os.path.dirname(__file__), 'viewer.ui'))
ui_viewer.show()
# Bind code to form
s = Viewer(ui_viewer)


cothread.WaitForQuit()
