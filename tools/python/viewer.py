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

def camonitor(on_event):
    def read():
        c = falib.connection([1])
        while True:
            on_event(c.read(2500)[:,0,0] * 1e-6)
    cothread.Spawn(read)


class monitor:
    def __init__(self, on_event, id, size):
        self.on_event = on_event
        self.size = size
        self.connection = falib.connection([id])
        self.running = True
        self.task = cothread.Spawn(self.monitor)

    def resize(self, size):
        self.size = size

    def close(self):
        self.running = False
        self.task.Wait()
        self.connection.close()

    def monitor(self):
        while self.running:
            data = self.connection.read(self.size)
            self.on_event(data[:, 0, :] * 1e-6)


BPM_list = [
    'SR%02dC-DI-EBPM-%02d' % (c+1, n+1)
    for c in range(24) for n in range(7)]

Timebase_list = [
    ('100ms', 1000),    ('250ms', 2500),    ('0.5s', 5000),
    ('1s', 10000),      ('2.5s', 25000),    ('5s', 50000),
    ('10s', 100000),    ('25s', 250000),    ('50s', 500000)]


# subclass form to implement buttons
class Viewer(widget, Ui_Viewer):
    '''application class'''
    def __init__(self):
        widget.__init__(self)
        self.setupUi(self)

        self.monitor = None
        self.channel.addItems(BPM_list)
        self.timebase.addItems([l[0] for l in Timebase_list])
        self.mode.addItems(['Raw Signal', 'FFT', 'Integrated'])
        # make any contents fill the empty frame
        grid = QtGui.QGridLayout(self.axes)
        self.axes.setLayout(grid)
        self.makeplot()

    def channel_currentIndexChanged(self, ix):
        # disconnect old channel if any
        if self.monitor:
            self.monitor.close()
        # connect new channel
        self.monitor = monitor(self.on_event, ix + 1, 1000)

    def rescale_clicked(self):
        min = numpy.amin(self.value)
        max = numpy.amax(self.value)
        self.p.setAxisScale(Qwt5.QwtPlot.yLeft, 1.2*min, 1.2*max)

    def timebase_currentIndexChanged(self, ix):
        new_timebase = Timebase_list[ix][1]
        self.p.setAxisScale(Qwt5.QwtPlot.xBottom, 0, new_timebase)
        self.monitor.resize(new_timebase)

    def mode_currentIndexChanged(self, ix):
        print 'new mode', ix

    def run_clicked(self, running):
        print 'running', running

    def on_event(self, value):
        '''camonitor callback'''
        self.value = value
        t = numpy.arange(value.shape[0])
        self.c1.setData(t, value[:, 0])
        self.c2.setData(t, value[:, 1])

    def makeplot(self):
        '''set up plotting'''
        # draw a plot in the frame
        p = Qwt5.QwtPlot(self.axes)
        c1 = Qwt5.QwtPlotCurve('X')
        c2 = Qwt5.QwtPlotCurve('Y')
        c1.attach(p)
        c1.setPen(QtGui.QPen(QtCore.Qt.blue))
        c2.attach(p)
        c2.setPen(QtGui.QPen(QtCore.Qt.red))

        # === Plot Customization ===
        # set background to black
        p.setCanvasBackground(QtCore.Qt.black)
        # stop flickering border
        p.canvas().setFocusIndicator(Qwt5.QwtPlotCanvas.NoFocusIndicator)
        # set fixed scale
        p.setAxisScale(Qwt5.QwtPlot.yLeft, -0.1, 0.1)
        p.setAxisScale(Qwt5.QwtPlot.xBottom, 0, 1000)
        # Set up manual zooming
        z = Qwt5.QwtPlotZoomer(p.canvas())
        z.setRubberBandPen(QtGui.QPen(QtCore.Qt.white))
        # automatically redraw when data changes
        p.setAutoReplot(True)
        # reset plot zoom (the default is 1000 x 1000)
#         for z in p.zoomers:
#             z.setZoomBase()

        self.p = p
        self.c1 = c1
        self.c2 = c2
        self.axes.layout().addWidget(self.p)

# create and show form
s = Viewer()
s.show()

cothread.WaitForQuit()
