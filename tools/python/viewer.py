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

def camonitor(name, on_event):
    print 'camonitor', name
    def read():
        c = falib.connection([1])
        while True:
            on_event(c.read(1000)[:,0,0])
    cothread.Spawn(read)


# subclass form to implement buttons
class Viewer(widget, Ui_Viewer):
    '''application class'''
    def __init__(self):
        widget.__init__(self)
        self.setupUi(self)

        self.channel.setText('SR21C-DI-EBPM-01:FR:WFX')
        self.monitor = None
        # make any contents fill the empty frame
        grid = QtGui.QGridLayout(self.axes)
        self.axes.setLayout(grid)
        self.makeplot()

    def bConnect_clicked(self):
        name = str(self.channel.text())
        print 'Connect Clicked', name
        # disconnect old channel if any
        if self.monitor:
            self.monitor.close()
        # connect new channel
        self.monitor = camonitor(name, self.on_event)

    def on_event(self, value):
        '''camonitor callback'''
        x = numpy.arange(value.shape[0])
        self.c.setData(x, value)

    def makeplot(self):
        '''set up plotting'''
        # draw a plot in the frame
        p = Qwt5.QwtPlot(self.axes)
        c = Qwt5.QwtPlotCurve('FR:WFX')
        c.attach(p)
        c.setPen(QtGui.QPen(QtCore.Qt.blue))

        # === Plot Customization ===
        # set background to black
        p.setCanvasBackground(QtCore.Qt.black)
        # stop flickering border
        p.canvas().setFocusIndicator(Qwt5.QwtPlotCanvas.NoFocusIndicator)
        # set zoom colour
#         for z in p.zoomers:
#             z.setRubberBandPen(QtGui.QPen(QtCore.Qt.white))
        # set fixed scale
        p.setAxisScale(Qwt5.QwtPlot.yLeft, -1e7, 1e7)
        p.setAxisScale(Qwt5.QwtPlot.xBottom, 0, 2500)
        # automatically redraw when data changes
        p.setAutoReplot(True)
        # reset plot zoom (the default is 1000 x 1000)
#         for z in p.zoomers:
#             z.setZoomBase()

        self.p = p
        self.c = c
        self.axes.layout().addWidget(self.p)

# create and show form
s = Viewer()
s.show()

cothread.WaitForQuit()
