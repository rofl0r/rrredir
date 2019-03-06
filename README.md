RR Redir - Round-Robin Port Redirector.
=======================================

This is a port redirector program like the well-known `redir`, but with the
ability to specify more than one target ip/port which will be tried one after
the other in round-robin fashion, until the connection succeeds.

It's very lightweight, and very light on resources too:

For every client, a thread with a stack size of 8KB is spawned.
the main process basically doesn't consume any resources at all.

The only limits are the amount of file descriptors and the RAM.

It's also designed to be robust: it handles resource exhaustion
gracefully by simply denying new connections, instead of calling abort()
as most other programs do these days.

Another plus is ease-of-use: no config file necessary, everything can be
done from the command line and doesn't even need any parameters for quick
setup.

command line options
------------------------

    rrredir [-i listenip -p port -t timeout -b bindaddr] ip1:port1 ip2:port2...

all arguments are optional.
by default listenip is 0.0.0.0 and port 1080.

option -b specifies the default ip outgoing connections are bound to.
it can be overruled per-target by appending @bindip to the target addr
e.g. ip1:port1@bindip1

the -t timeout is specified in seconds, default: 0
if timeout is set to 0, block until the OS cancels conn. attempt

all incoming connections will be redirected to ip1:port1, followed
by ip2:port2 if the former host is unreachable, etc.
