#!/usr/bin/env dls-python2.6

'''Form Example with Monitor'''

from pkg_resources import require
require('cothread')

import os, sys

import cothread

import numpy
from PyQt4 import Qwt5, QtGui, QtCore, uic

# Nasty hack.  Need to integrate this into cothread at some point, probably
# better to make falib cothread aware
import select
select.select = cothread.select

sys.path.append(os.path.dirname(__file__))
import falib



# ------------------------------------------------------------------------------
#   Data Acquisition

# A stream of data for one selected BPM is acquired by connecting to the FA
# sniffer server.  This is maintained in a "circular" buffer containing the last
# 50 seconds worth of data (500,000 points) and delivered on demand to the
# display layer.

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
                    self.on_event(self.read())
                    self.data_ready -= self.update_size
        except falib.connection.EOF:
            self.on_eof()

    def read(self):
        '''Can be called at any time to read the most recent buffer.'''
        return 1e-3 * self.buffer.read(self.notify_size)



# ------------------------------------------------------------------------------
#   Mode Specific Functionality

# Three basic display modes are supported: raw data, FFT of data and integrated
# displacement (derived from the FFT).  These modes and their user support
# functionality are implemented by the classes below, one for each display mode.


F_S = 10072.0

# Unicode characters
char_times  = u'\u00D7'             # Multiplication sign
char_mu     = u'\u03BC'             # Greek mu
char_sqrt   = u'\u221A'             # Square root sign
char_cdot   = u'\u22C5'             # Centre dot

micrometre  = char_mu + 'm'


class mode_common:
    def __init__(self, parent):
        pass

    def set_enable(self, enabled):
        pass

    def get_minmax(self, value):
        value = self.compute(value)
        return numpy.nanmin(value), numpy.nanmax(value)

    def linear_rescale(self, value):
        min, max = self.get_minmax(value)
        margin = 0.2 * (max - min)
        self.ymin = min - margin
        self.ymax = max + margin

    def log_rescale(self, value):
        self.ymin, self.ymax = self.get_minmax(value)

    rescale = log_rescale           # Most common default


class mode_raw(mode_common):
    mode_name = 'Raw Signal'
    yname = 'Position'
    yunits = micrometre
    xscale = Qwt5.QwtLinearScaleEngine
    yscale = Qwt5.QwtLinearScaleEngine
    xmin = 0
    ymin = -10
    ymax = 10

    rescale = mode_common.linear_rescale

    def set_timebase(self, timebase):
        if timebase <= 10000:
            self.xname = 'Time'
            self.xunits = 'ms'
            scale = 1e3
        else:
            self.xname = 'Time'
            self.xunits = 's'
            scale = 1.0
        self.xmax = scale / F_S * timebase
        self.xaxis = scale / F_S * numpy.arange(timebase)

    def compute(self, value):
        return value


def scaled_abs_fft(value, axis=0):
    '''Returns the fft of value (along axis 0) scaled so that values are in
    units per sqrt(Hz).  The magnitude of the first half of the spectrum is
    returned.'''
    fft = numpy.fft.fft(value, axis=axis)

    # This trickery below is simply implementing fft[:N//2] where the slicing is
    # along the specified axis rather than axis 0.  It does seem a bit
    # complicated...
    N = value.shape[axis]
    slice = [numpy.s_[:] for s in fft.shape]
    slice[axis] = numpy.s_[:N//2]
    fft = fft[slice]

    # Finally scale the result into units per sqrt(Hz)
    return numpy.abs(fft) * numpy.sqrt(2.0 / (F_S * N))

def fft_timebase(timebase, scale=1.0):
    '''Returns a waveform suitable for an FFT timebase with the given number of
    points.'''
    return scale * F_S * numpy.arange(timebase // 2) / timebase


class mode_fft(mode_common):
    mode_name = 'FFT'
    xname = 'Frequency'
    yname = 'Amplitude'
    xunits = 'kHz'
    yunits = '%s/%sHz' % (micrometre, char_sqrt)
    xscale = Qwt5.QwtLinearScaleEngine
    yscale = Qwt5.QwtLog10ScaleEngine
    xmin = 0
    xmax = 1e-3 * F_S / 2
    ymin = 1e-4
    ymax = 1

    Decimations = [1, 10, 100]

    def __init__(self, parent):
        # Create the GUI components for managing the decimation selection
        self.parent = parent
        self.label = QtGui.QLabel('Decimation', parent.ui)
        self.selector = QtGui.QComboBox(parent.ui)
        parent.ui.bottom_row.addWidget(self.label)
        parent.ui.bottom_row.addWidget(self.selector)
        parent.connect(self.selector,
            'currentIndexChanged(int)', self.set_decimation)
        self.set_enable(False)
        self.decimation = 1

    def set_timebase(self, timebase):
        self.xaxis = fft_timebase(timebase, 1e-3)

        # Manage the set of available decimations
        self.timebase = timebase

        # Update selection of decimations to match those available for the
        # selected decimation
        self.selector.blockSignals(True)
        self.selector.clear()
        valid_items = [n for n in self.Decimations if 1000 * n <= timebase]
        self.selector.addItems(['%d:1' % n for n in valid_items])
        self.selector.blockSignals(False)

        # Restore the current selection, as far as possible.
        if self.decimation not in valid_items:
            self.decimation = valid_items[-1]
        self.selector.setCurrentIndex(valid_items.index(self.decimation))

    def set_enable(self, enabled):
        # Simply show or hide the decimation GUI
        self.label.setVisible(enabled)
        self.selector.setVisible(enabled)

    def set_decimation(self, ix):
        self.decimation = self.Decimations[ix]
        self.xaxis = fft_timebase(self.timebase // self.decimation, 1e-3)

    def compute(self, value):
        if self.decimation == 1:
            return scaled_abs_fft(value)
        else:
            # Compute a decimated fft by segmenting the waveform (by reshaping),
            # computing the fft of each segment, and computing the mean power of
            # all the resulting transforms.
            N = len(value)
            value = value.reshape((self.decimation, N//self.decimation, 2))
            fft = scaled_abs_fft(value, axis=1)
            return numpy.sqrt(numpy.mean(fft**2, axis=0))


def compute_gaps(l, N):
    '''This computes a series of logarithmically spaced indexes into an array
    of length l.  N is a hint for the number of indexes, but the result may
    be somewhat shorter.'''
    gaps = numpy.int_(numpy.logspace(0, numpy.log10(l), N))
    counts = numpy.diff(gaps)
    return counts[counts > 0]

def condense(value, counts):
    '''The given waveform is condensed in logarithmic intervals so that the same
    number of points are generated in each decade.  The accumulation and number
    of accumulated points are returned as separate waveforms.'''

    # The result is the same shape as the value in all axes except the first.
    shape = list(value.shape)
    shape[0] = len(counts)
    sums = numpy.empty(shape)

    left = 0
    for i, step in enumerate(counts):
        sums[i] = numpy.sum(value[left:left + step], axis=0)
        left += step
    return sums


class mode_fft_logf(mode_common):
    mode_name = 'FFT (log f)'
    xname = 'Frequency'
    yname = 'Amplitude %s freq' % char_times
    xunits = 'Hz'
    yunits = '%s%s%sHz' % (micrometre, char_cdot, char_sqrt)
    xscale = Qwt5.QwtLog10ScaleEngine
    yscale = Qwt5.QwtLog10ScaleEngine
    xmax = F_S / 2
    ymin = 1e-3
    ymax = 100

    def set_timebase(self, timebase):
        self.counts = compute_gaps(timebase//2 - 1, 1000)
        self.xaxis = F_S * numpy.cumsum(self.counts) / timebase
        self.xmin = self.xaxis[0]

    def compute(self, value):
        fft = scaled_abs_fft(value)[1:]
        return self.xaxis[:, None] * numpy.sqrt(
            condense(fft**2, self.counts) / self.counts[:,None])


class mode_integrated(mode_common):
    mode_name = 'Integrated'
    xname = 'Frequency'
    yname = 'Cumulative amplitude'
    xunits = 'Hz'
    yunits = micrometre
    xscale = Qwt5.QwtLog10ScaleEngine
    yscale = Qwt5.QwtLog10ScaleEngine
    xmax = F_S / 2
    ymin = 1e-3
    ymax = 10

    def set_timebase(self, timebase):
        self.counts = compute_gaps(timebase//2 - 1, 5000)
        self.xaxis = F_S * numpy.cumsum(self.counts) / timebase
        self.xmin = self.xaxis[0]

    def compute(self, value):
        N = len(value)
        fft2 = condense(scaled_abs_fft(value)[1:]**2, self.counts)
        return numpy.sqrt(F_S / N * numpy.cumsum(fft2, axis=0))

    def __init__(self, parent):
        self.parent = parent
        self.button = QtGui.QPushButton('Background', parent.ui)
        parent.ui.bottom_row.addWidget(self.button)
        parent.connect(self.button, 'clicked()', self.set_background)
        self.cxb = parent.makecurve(QtCore.Qt.blue, True)
        self.cyb = parent.makecurve(QtCore.Qt.red,  True)
        self.set_enable(False)

    def set_enable(self, enabled):
        self.button.setVisible(enabled)
        self.cxb.setVisible(enabled)
        self.cyb.setVisible(enabled)

    def set_background(self):
        v = self.compute(self.parent.monitor.read())
        self.cxb.setData(self.xaxis, v[:, 0])
        self.cyb.setData(self.xaxis, v[:, 1])
        self.parent.plot.replot()


Display_modes = [mode_raw, mode_fft, mode_fft_logf, mode_integrated]

# Start up in raw display mode
INITIAL_MODE = 0


# ------------------------------------------------------------------------------
#   FA Sniffer Viewer

# This is the implementation of the viewer as a Qt display application.


BPM_list = [('Other', [])] + [
    ('Cell %d' % (c+1),
     [('SR%02dC-DI-EBPM-%02d' % (c+1, n+1), 7*c+n+1) for n in range(7)])
    for c in range(24)]
BPM_list[21][1].append(('SR21C-DI-EBPM-08', 169))


# Start on BPM #1 -- as sensible a default as any
INITIAL_BPM = 0


Timebase_list = [
    ('100ms', 1000),    ('250ms', 2500),    ('0.5s',  5000),
    ('1s',   10000),    ('2.5s', 25000),    ('5s',   50000),
    ('10s', 100000),    ('25s', 250000),    ('50s', 500000)]

# Start up with 1 second window
INITIAL_TIMEBASE = 3

SCROLL_THRESHOLD = 10000

class SpyMouse(QtCore.QObject):
    def __init__(self, parent):
        QtCore.QObject.__init__(self, parent)
        parent.setMouseTracking(True)
        parent.installEventFilter(self)

    def eventFilter(self, object, event):
        if event.type() == QtCore.QEvent.MouseMove:
            self.emit(QtCore.SIGNAL('MouseMove'), event.pos())
        return QtCore.QObject.eventFilter(self, object, event)


class Viewer:
    '''application class'''
    def __init__(self, ui):
        self.ui = ui

        # make any contents fill the empty frame
        self.makeplot()

        self.monitor = monitor(self.on_data_update, self.on_eof, 500000, 1000)

        # Prepare the selections in the controls
        ui.timebase.addItems([l[0] for l in Timebase_list])
        ui.mode.addItems([l.mode_name for l in Display_modes])
        ui.channel_group.addItems([l[0] for l in BPM_list])
        ui.show_curves.addItems(['Show X&Y', 'Show X', 'Show Y'])

        ui.channel_id.setValidator(QtGui.QIntValidator(0, 255, ui))

        ui.position_x = QtGui.QLabel('X:     ', ui.statusbar)
        ui.position_y = QtGui.QLabel('Y:     ', ui.statusbar)
        ui.statusbar.addPermanentWidget(ui.position_x)
        ui.statusbar.addPermanentWidget(ui.position_y)

        # For each possible display mode create the initial state used to manage
        # that display mode and set up the initial display mode.
        self.mode_list = [l(self) for l in Display_modes]
        self.mode = self.mode_list[INITIAL_MODE]
        self.mode.set_enable(True)
        self.ui.mode.setCurrentIndex(INITIAL_MODE)

        # Make the initial GUI connections
        self.connect(ui.channel_group,
            'currentIndexChanged(int)', self.set_group)
        self.connect(ui.channel,
            'currentIndexChanged(int)', self.set_channel)
        self.connect(ui.channel_id, 'editingFinished()', self.set_channel_id)
        self.connect(ui.timebase,
            'currentIndexChanged(int)', self.set_timebase)
        self.connect(ui.rescale, 'clicked()', self.rescale_graph)
        self.connect(ui.mode, 'currentIndexChanged(int)', self.set_mode)
        self.connect(ui.run, 'clicked(bool)', self.toggle_running)
        self.connect(ui.show_curves,
            'currentIndexChanged(int)', self.show_curves)

        # Initial control settings: these all trigger GUI related actions.
        ui.channel_group.setCurrentIndex(1)
        ui.channel.setCurrentIndex(0)
        ui.timebase.setCurrentIndex(INITIAL_TIMEBASE)
        # Go!
        self.try_start()

    def connect(self, control, signal, action):
        '''Connects a Qt signal from a control to the selected action.'''
        self.ui.connect(control, QtCore.SIGNAL(signal), action)

    def makecurve(self, colour, dotted=False):
        c = Qwt5.QwtPlotCurve()
        pen = QtGui.QPen(colour)
        if dotted:
            pen.setStyle(QtCore.Qt.DotLine)
        c.setPen(pen)
        c.attach(self.plot)
        return c

    def makeplot(self):
        '''set up plotting'''
        # Draw a plot in the frame.  We do this, rather than defining the
        # QwtPlot object in Qt designer because loadUi then fails!
        plot = self.ui.plot

        self.plot = plot
        self.cx = self.makecurve(QtCore.Qt.blue)
        self.cy = self.makecurve(QtCore.Qt.red)

        # set background to black
        plot.setCanvasBackground(QtCore.Qt.black)

        # Enable zooming
        zoom = Qwt5.QwtPlotZoomer(plot.canvas())
        zoom.setRubberBandPen(QtGui.QPen(QtCore.Qt.white))
        zoom.setTrackerPen(QtGui.QPen(QtCore.Qt.white))
        # This is a poorly documented trick to disable the use of the right
        # button for cancelling zoom, so we can use it for panning instead.  The
        # first argument of setMousePattern() selects the zooming action, and is
        # one of the following with the given default assignment:
        #
        #   Index   Button          Action
        #   0       Left Mouse      Start and stop rubber band selection
        #   1       Right Mouse     Restore to original unzoomed axes
        #   2       Middle Mouse    Zoom out one level
        #   3       Shift Left      ?
        #   4       Shift Right     ?
        #   5       Shift Middle    Zoom back in one level
        zoom.setMousePattern(1, QtCore.Qt.NoButton)
        self.zoom = zoom

        # Enable panning.  We reconfigure the active mouse to use the right
        # button so that panning and zooming can coexist.
        pan = Qwt5.QwtPlotPanner(plot.canvas())
        pan.setMouseButton(QtCore.Qt.RightButton)

        # Monitor mouse movements over the plot area so we can show the position
        # in coordinates.
        self.connect(SpyMouse(plot.canvas()), 'MouseMove', self.mouse_move)



    # --------------------------------------------------------------------------
    # GUI event handlers

    def set_group(self, ix):
        self.group_index = ix
        self.ui.channel_id.setVisible(ix == 0)
        self.ui.channel.setVisible(ix > 0)

        if ix == 0:
            self.ui.channel_id.setText(str(self.channel))
        else:
            self.ui.channel.blockSignals(True)
            self.ui.channel.clear()
            self.ui.channel.addItems([l[0] for l in BPM_list[ix][1]])
            self.ui.channel.setCurrentIndex(-1)
            self.ui.channel.blockSignals(False)

    def set_channel(self, ix):
        bpm = BPM_list[self.group_index][1][ix]
        self.channel = bpm[1]
        self.monitor.set_id(self.channel)
        self.ui.statusbar.showMessage(
            'BPM: %s (id %d)' % (bpm[0], self.channel))

    def set_channel_id(self):
        self.channel = int(self.ui.channel_id.text())
        self.monitor.set_id(self.channel)
        self.ui.statusbar.showMessage('BPM id %d' % self.channel)

    def rescale_graph(self):
        self.mode.rescale(self.monitor.read())
        self.plot.setAxisScale(
            Qwt5.QwtPlot.xBottom, self.mode.xmin, self.mode.xmax)
        self.plot.setAxisScale(
            Qwt5.QwtPlot.yLeft, self.mode.ymin, self.mode.ymax)
        self.zoom.setZoomBase()
        self.plot.replot()

    def set_timebase(self, ix):
        new_timebase = Timebase_list[ix][1]
        self.timebase = new_timebase
        self.monitor.resize(new_timebase, min(new_timebase, SCROLL_THRESHOLD))
        self.reset_mode()

    def set_mode(self, ix):
        self.mode.set_enable(False)
        self.mode = self.mode_list[ix]
        self.mode.set_enable(True)
        self.reset_mode()

    def toggle_running(self, running):
        if running:
            self.try_start()
        else:
            self.monitor.stop()

    def show_curves(self, ix):
        show_x = ix in [0, 1]
        show_y = ix in [0, 2]
        self.cx.setVisible(show_x)
        self.cy.setVisible(show_y)
        self.plot.replot()

    def mouse_move(self, pos):
        x = self.plot.invTransform(Qwt5.QwtPlot.xBottom, pos.x())
        y = self.plot.invTransform(Qwt5.QwtPlot.yLeft, pos.y())
        self.ui.position_x.setText('X: %8.4g %s' % (x, self.mode.xunits))
        self.ui.position_y.setText('Y: %8.4g %s' % (y, self.mode.yunits))


    # --------------------------------------------------------------------------
    # Data event handlers

    def on_data_update(self, value):
        v = self.mode.compute(value)
        self.cx.setData(self.mode.xaxis, v[:, 0])
        self.cy.setData(self.mode.xaxis, v[:, 1])
        if self.ui.autoscale.isChecked() and self.zoom.zoomRectIndex() == 0:
            self.mode.rescale(value)
            self.plot.setAxisScale(
                Qwt5.QwtPlot.yLeft, self.mode.ymin, self.mode.ymax)
        self.plot.replot()

    def on_eof(self):
        self.ui.run.setCheckState(False)
        self.monitor.stop()
        self.ui.statusbar.showMessage('FA server disconnected')


    # --------------------------------------------------------------------------
    # Handling

    def try_start(self):
        try:
            self.monitor.start()
        except Exception, message:
            self.ui.run.setCheckState(False)
            self.ui.statusbar.showMessage(
                'Unable to connect to server: %s' % message)


    def reset_mode(self):
        self.mode.set_timebase(self.timebase)

        x = Qwt5.QwtPlot.xBottom
        y = Qwt5.QwtPlot.yLeft
        self.plot.setAxisTitle(
            x, '%s (%s)' % (self.mode.xname, self.mode.xunits))
        self.plot.setAxisTitle(
            y, '%s (%s)' % (self.mode.yname, self.mode.yunits))
        self.plot.setAxisScaleEngine(x, self.mode.xscale())
        self.plot.setAxisScaleEngine(y, self.mode.yscale())
        self.plot.setAxisScale(x, self.mode.xmin, self.mode.xmax)
        self.plot.setAxisScale(y, self.mode.ymin, self.mode.ymax)
        self.zoom.setZoomBase()

        # Force a redraw right away with the data we have in hand
        self.on_data_update(self.monitor.read())


cothread.iqt()

# Create and show form
# Would use loadUi, but unfortunately it doesn't work when one of the widgets is
# a QwtPlot widget, so we end up with this rather complicated approach.
ui_form, ui_base = uic.loadUiType(
    os.path.join(os.path.dirname(__file__), 'viewer.ui'))

class UI(ui_form, ui_base):
    pass

ui = UI()
ui.setupUi(ui)

s = Viewer(ui)

ui.show()
cothread.WaitForQuit()
