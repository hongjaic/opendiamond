#
#  The OpenDiamond Platform for Interactive Search
#  Version 6
#
#  Copyright (c) 2009-2011 Carnegie Mellon University
#  All rights reserved.
#
#  This software is distributed under the terms of the Eclipse Public
#  License, Version 1.0 which can be found in the file named LICENSE.
#  ANY USE, REPRODUCTION OR DISTRIBUTION OF THIS SOFTWARE CONSTITUTES
#  RECIPIENT'S ACCEPTANCE OF THIS AGREEMENT
#

import os
import resource

def daemonize():
    # Double-fork
    if os.fork():
        os._exit(0)
    os.setsid()
    if os.fork():
        os._exit(0)
    # Close open fds
    maxfd = resource.getrlimit(resource.RLIMIT_NOFILE)[1]
    for fd in xrange(maxfd):
        try:
            os.close(fd)
        except OSError:
            pass
    # Open new fds
    os.open("/dev/null", os.O_RDWR)
    os.dup2(0, 1)
    os.dup2(0, 2)
