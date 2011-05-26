#
#  The OpenDiamond Platform for Interactive Search
#  Version 6
#
#  Copyright (c) 2011 Carnegie Mellon University
#  All rights reserved.
#
#  This software is distributed under the terms of the Eclipse Public
#  License, Version 1.0 which can be found in the file named LICENSE.
#  ANY USE, REPRODUCTION OR DISTRIBUTION OF THIS SOFTWARE CONSTITUTES
#  RECIPIENT'S ACCEPTANCE OF THIS AGREEMENT
#

'''
diamondd has a long-lived supervisor process responsible for accepting
connections.  The supervisor process forks to produce a child process which
handles a particular search.  This ensures that any resource leaks within
the search logic will not accumulate in a long-running process.

The supervisor diamondd process is single-threaded, and all of its network I/O
is performed non-blockingly.  It is responsible for the following:

1.  Listening for incoming control and blast channel connections and pairing
them via a nonce communicated when the connection is first established.

2.  Establishing a temporary directory and forking a child process for every
connection pair.

3.  Cleaning up after search processes which have exited by deleting their
temporary directories and killing all of their children (filters and helper
processes).

The child is responsible for handling the search.  Initially it has only one
thread, which is responsible for handling the control connection back to the
client.  All client RPCs, including search reexecution, are handled in this
thread.  When the start() RPC is received, the client creates N worker
threads (configurable, defaulting to the number of processors on the
machine) to process objects for the search.

Several pieces of mutable state are shared between threads.  The control
thread configures a ScopeListLoader which iterates over the in-scope Diamond
objects, returning a new object to each worker thread that asks for one.
The blast channel is also shared.  There are also shared objects for logging
and for tracking of statistics and session variables.  All of these objects
have locking to ensure consistency.

Each worker thread maintains a private TCP connection to the Redis server,
which is used for result and attribute caching.  Each worker thread also
maintains one child process for each filter in the filter stack.  These
children are the actual filter code, and communicate with the worker thread
via a pair of pipes.  Because each worker thread has its own set of filter
processes, worker threads can process objects independently.

Each worker thread executes a loop:

1.  Obtain a new object from the ScopeListLoader.

2.  Retrieve result cache entries from Redis.

3.  Walk the result cache entries to determine if a drop decision can be
made.  If so, drop the object.

4.  For each filter in the filter chain, determine whether we received a
valid result cache entry for the filter.  If so, attempt to obtain attribute
cache entries from Redis.  If successful, merge the cached attributes into
the object.  Otherwise, execute the filter.  If the filter produces a drop
decision, break.

5.  Transmit new result cache entries, as well as attribute cache entries
for filters producing less than 2 MB/s of attribute values, to Redis.

6.  If accepting the object, transmit it to the client via the blast
channel.

If a filter crashes while processing an object, the object is dropped and
the filter is restarted.  If a worker thread or the control thread crashes,
the exception is logged and the entire search is terminated.
'''

from datetime import datetime
import logging
from logging.handlers import TimedRotatingFileHandler
import os
import signal
import sys

import opendiamond
from opendiamond.helpers import daemonize
from opendiamond.server.child import ChildManager
from opendiamond.server.listen import ConnListener
from opendiamond.server.rpc import RPCConnection, ConnectionFailure
from opendiamond.server.search import Search

_log = logging.getLogger(__name__)

class _Signalled(BaseException):
    '''Exception indicating that a signal has been received.'''

    def __init__(self, sig):
        self.signal = sig
        # Find the signal name
        for attr in dir(signal):
            if (attr.startswith('SIG') and not attr.startswith('SIG_') and
                    getattr(signal, attr) == sig):
                self.signame = attr
                break
        else:
            self.signame = 'unknown signal'


class _TimestampedLogFormatter(logging.Formatter):
    '''Format a log message with a timestamp including milliseconds delimited
    by a decimal point.'''

    def __init__(self):
        logging.Formatter.__init__(self, '%(asctime)s %(message)s',
                                '%Y-%m-%d %H:%M:%S')

    def formatTime(self, record, datefmt):
        s = datetime.fromtimestamp(record.created).strftime(datefmt)
        return s + '.%03d' % record.msecs


class DiamondServer(object):
    caught_signals = (signal.SIGINT, signal.SIGTERM)

    def __init__(self, config):
        # Daemonize before doing any other setup
        if config.daemonize:
            daemonize()

        self.config = config
        self._children = ChildManager(config.cgroupdir, not config.oneshot)
        self._listener = ConnListener(config.localhost_only)

        # Configure signals
        for sig in self.caught_signals:
            signal.signal(sig, self._handle_signal)

        # Configure logging
        baselog = logging.getLogger()
        baselog.setLevel(logging.DEBUG)
        if not config.daemonize:
            # In daemon mode, stderr goes to /dev/null, so don't bother
            # logging there.
            handler = logging.StreamHandler()
            baselog.addHandler(handler)
        self._logfile_handler = TimedRotatingFileHandler(
                                os.path.join(config.logdir, 'diamondd.log'),
                                when='midnight', backupCount=14)
        self._logfile_handler.setFormatter(_TimestampedLogFormatter())
        baselog.addHandler(self._logfile_handler)

    def run(self):
        try:
            # Log startup of parent
            _log.info('Starting supervisor %s, pid %d',
                                        opendiamond.__version__, os.getpid())
            _log.info('Server IDs: %s', ', '.join(self.config.serverids))
            if self.config.cache_server:
                _log.info('Cache: %s:%d' % self.config.cache_server)
            while True:
                # Accept a new connection pair
                control, data = self._listener.accept()
                # Fork a child for this connection pair.  In the child, this
                # does not return.
                self._children.start(self._child, control, data)
                # Close the connection pair in the parent
                control.close()
                data.close()
        except _Signalled, s:
            _log.info('Supervisor exiting on %s', s.signame)
            # Stop listening for incoming connections
            self._listener.shutdown()
            # Kill our children and clean up after them
            self._children.kill_all()
            # Shut down logging
            logging.shutdown()
            # Ensure our exit status reflects that we died on the signal
            signal.signal(s.signal, signal.SIG_DFL)
            os.kill(os.getpid(), s.signal)
        except Exception:
            _log.exception('Supervisor exception')
            # Don't attempt to shut down cleanly; just flush logging buffers
            logging.shutdown()
            sys.exit(1)

    def _child(self, control, data):
        '''Main function for child process.'''
        # Close supervisor log, open child log
        baselog = logging.getLogger()
        baselog.removeHandler(self._logfile_handler)
        del self._logfile_handler
        now = datetime.now().strftime('%Y-%m-%d-%H:%M:%S')
        logname = 'search-%s-%d.log' % (now, os.getpid())
        logpath = os.path.join(self.config.logdir, logname)
        handler = logging.FileHandler(logpath)
        handler.setFormatter(_TimestampedLogFormatter())
        baselog.addHandler(handler)

        # Okay, now we have logging
        try:
            # Close listening socket and half-open connections
            self._listener.shutdown()
            # Log startup of child
            _log.info('Starting search %s, pid %d', opendiamond.__version__,
                                    os.getpid())
            _log.info('Peer: %s', control.getpeername()[0])
            _log.info('Worker threads: %d', self.config.threads)
            # Set up connection wrappers and search object
            control = RPCConnection(control)
            search = Search(self.config, RPCConnection(data))
            # Dispatch RPCs on the control connection until we die
            while True:
                control.dispatch(search)
        except ConnectionFailure, e:
            # Client closed connection
            _log.info('Search exiting: %s', str(e))
        except _Signalled, s:
            # Worker threads raise SIGUSR1 when they've encountered a
            # fatal error
            if s.signal != signal.SIGUSR1:
                _log.info('Search exiting on %s', s.signame)
        except Exception:
            _log.exception('Control thread exception')
        finally:
            logging.shutdown()

    def _handle_signal(self, sig, frame):
        '''Signal handler in the supervisor.'''
        raise _Signalled(sig)
