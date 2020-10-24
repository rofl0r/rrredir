/*
   RR Redir - a round-robin port redirector.

   Copyright (C) 2018 rofl0r.

*/

#define _GNU_SOURCE
#include <unistd.h>
#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <signal.h>
#include <poll.h>
#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include "server.h"
#include "sblist.h"

#ifndef MAX
#define MAX(x, y) ((x) > (y) ? (x) : (y))
#endif

#ifdef PTHREAD_STACK_MIN
#define THREAD_STACK_SIZE MAX(8*1024, PTHREAD_STACK_MIN)
#else
#define THREAD_STACK_SIZE 64*1024
#endif

#if defined(__APPLE__)
#undef THREAD_STACK_SIZE
#define THREAD_STACK_SIZE 64*1024
#elif defined(__GLIBC__) || defined(__FreeBSD__)
#undef THREAD_STACK_SIZE
#define THREAD_STACK_SIZE 32*1024
#endif

static const struct server* server;
unsigned long timeout;
static sblist* targets;

struct target {
	union sockaddr_union addr;
	union sockaddr_union bind_addr;
};
struct thread {
	pthread_t pt;
	struct client client;
	volatile int  done;
};

#ifndef CONFIG_LOG
#define CONFIG_LOG 1
#endif
#if CONFIG_LOG
/* we log to stderr because it's not using line buffering, i.e. malloc which would need
   locking when called from different threads. for the same reason we use dprintf,
   which writes directly to an fd. */
#define dolog(...) dprintf(2, __VA_ARGS__)
#else
static void dolog(const char* fmt, ...) { }
#endif

static int connect_target(struct client *client) {
	size_t i;
	struct target target;
	for(i=0; i<sblist_getsize(targets); i++) {
		target = *(struct target*)sblist_get(targets, i);
		int af = SOCKADDR_UNION_AF(&target.addr),
		fd = socket(af, SOCK_STREAM, 0);
		if(fd == -1) {
			eval_errno: ;
			if(fd != -1) close(fd);
			int e = errno;
			switch(e) {
				case EPROTOTYPE:
				case EPROTONOSUPPORT:
				case EAFNOSUPPORT:
				case ECONNREFUSED:
				case ENETDOWN:
				case ENETUNREACH:
				case EHOSTUNREACH:
				case ETIMEDOUT:
					continue;
				case EBADF:
				default:
				perror("socket/connect");
				return -1;
			}
		}
		if(SOCKADDR_UNION_AF(&target.bind_addr) != AF_UNSPEC &&
		   bindtoip(fd, &target.bind_addr) == -1)
			goto eval_errno;

		int flags = fcntl(fd, F_GETFL);
		if(fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
			err_fcntl:
			close(fd);
			perror("fcntl");
			continue;
		}

		if(connect(fd, (void*)&target.addr, sizeof(target.addr)) == -1) {
			int e = errno;
			if (!(e == EINPROGRESS || e == EWOULDBLOCK))
				goto eval_errno;
		}

		if(fcntl(fd, F_SETFL, flags) == -1)
			goto err_fcntl;

		struct pollfd fds = {.fd = fd, .events = POLLOUT };
		int optval, ret;
		socklen_t optlen = sizeof(optval);

		ret = poll(&fds, 1, timeout ? timeout*1000 : -1);
		if(ret == 1 && (fds.revents & POLLOUT)) {
			ret = getsockopt(fd, SOL_SOCKET, SO_ERROR, &optval, &optlen);
			if(ret == -1) goto eval_errno;
			else if(optval) {
				errno = optval;
				goto eval_errno;
			}
		} else if(ret == 0) {
			errno = ETIMEDOUT;
			goto eval_errno;
		}

		if(CONFIG_LOG) {
			char clientname[256], servname[256];
			int af;
			void *ipdata;
			af = SOCKADDR_UNION_AF(&client->addr);
			ipdata = SOCKADDR_UNION_ADDRESS(&client->addr);
			inet_ntop(af, ipdata, clientname, sizeof clientname);

			af = SOCKADDR_UNION_AF(&target.addr);
			ipdata = SOCKADDR_UNION_ADDRESS(&target.addr);
			inet_ntop(af, ipdata, servname, sizeof servname);
			dolog("client[%d] %s: connected to %s:%d\n", client->fd, clientname, servname, htons(SOCKADDR_UNION_PORT(&target.addr)));
		}
		return fd;
	}
	return -1;
}

static void copyloop(int fd1, int fd2) {
	struct pollfd fds[2] = {
		[0] = {.fd = fd1, .events = POLLIN},
		[1] = {.fd = fd2, .events = POLLIN},
	};

	while(1) {
		/* inactive connections are reaped after 15 min to free resources.
		   usually programs send keep-alive packets so this should only happen
		   when a connection is really unused. */
		switch(poll(fds, 2, 60*15*1000)) {
			case 0:
				return;
			case -1:
				if(errno == EINTR || errno == EAGAIN) continue;
				else perror("poll");
				return;
		}
		int infd = (fds[0].revents & POLLIN) ? fd1 : fd2;
		int outfd = infd == fd2 ? fd1 : fd2;
		char buf[1024];
		ssize_t sent = 0, n = read(infd, buf, sizeof buf);
		if(n <= 0) return;
		while(sent < n) {
			ssize_t m = write(outfd, buf+sent, n-sent);
			if(m < 0) return;
			sent += m;
		}
	}
}

static void* clientthread(void *data) {
	struct thread *t = data;
	int fd = connect_target(&t->client);
	if(fd != -1) {
		copyloop(t->client.fd, fd);
		close(fd);
	}
	close(t->client.fd);
	t->done = 1;
	return 0;
}

static void collect(sblist *threads) {
	size_t i;
	for(i=0;i<sblist_getsize(threads);) {
		struct thread* thread = *((struct thread**)sblist_get(threads, i));
		if(thread->done) {
			pthread_join(thread->pt, 0);
			sblist_delete(threads, i);
			free(thread);
		} else
			i++;
	}
}

static int complain_bind(const char* addr) {
	dprintf(2, "error: the supplied bind address %s could not be resolved\n", addr);
	return 1;
}

static int usage(void) {
	dprintf(2,
		"RR Redir - a round-robin port redirector\n"
		"----------------------------------------\n"
		"usage: rrredir [-i listenip -p port -t timeout -b bindaddr] ip1:port1 ip2:port2 ...\n"
		"all arguments are optional.\n"
		"by default listenip is 0.0.0.0 and port 1080.\n\n"
		"option -b specifies the default ip outgoing connections are bound to\n"
		"it can be overruled per-target by appending @bindip to the target addr\n"
		"e.g. ip1:port1@bindip1\n"
		"the -t timeout is specified in seconds, default: %lu\n"
		"if timeout is set to 0, block until the OS cancels conn. attempt\n"
		"\n"
		"all incoming connections will be redirected to ip1:port1, followed\n"
		"by ip2:port2 if the former host is unreachable, etc.\n"
		, timeout
	);
	return 1;
}

int main(int argc, char** argv) {
	int c;
	const char *listenip = "0.0.0.0", *bind_arg = 0;
	unsigned port = 1080;
	timeout = 0;
	while((c = getopt(argc, argv, ":b:i:p:t:")) != -1) {
		switch(c) {
			case 'b':
				bind_arg = optarg;
				break;
			case 'i':
				listenip = optarg;
				break;
			case 'p':
				port = atoi(optarg);
				break;
			case 't':
				timeout = atoi(optarg);
				break;
			case ':':
				dprintf(2, "error: option -%c requires an operand\n", optopt);
			case '?':
				return usage();
		}
	}
	targets = sblist_new(sizeof(struct target), 8);
	while(argv[optind]) {
		char *p = strchr(argv[optind], ':'), *q;
		if(!p) {
			dprintf(2, "error: expected ip:port tuple\n");
			return usage();
		}
		*p = 0;
		q = strchr(p+1, '@');
		if(q) *q = 0;
		int tport = atoi(p+1);
		struct addrinfo* remote;
		if(resolve(argv[optind], tport, &remote)) {
			*p = ':';
			dprintf(2, "error: cannot resolve %s\n", argv[optind]);
			return 1;
		}
		struct target t = {0};
		if(!q) {
			if(bind_arg) {
				if(resolve_sa(bind_arg, 0, &t.bind_addr))
					return complain_bind(bind_arg);
			} else
				SOCKADDR_UNION_AF(&t.bind_addr) = AF_UNSPEC;
		} else {
			if(resolve_sa(q+1, 0, &t.bind_addr))
				return complain_bind(q+1);
		}
		memcpy(&t.addr, remote->ai_addr, remote->ai_addrlen);
		sblist_add(targets, &t);
		optind++;
	};
	if(!sblist_getsize(targets)) {
		dprintf(2, "error: need at least one redirect target\n");
		return usage();
	}
	signal(SIGPIPE, SIG_IGN);
	struct server s;
	sblist *threads = sblist_new(sizeof (struct thread*), 8);
	if(server_setup(&s, listenip, port)) {
		perror("server_setup");
		return 1;
	}
	server = &s;

	while(1) {
		collect(threads);
		struct client c;
		struct thread *curr = malloc(sizeof (struct thread));
		if(!curr) goto oom;
		curr->done = 0;
		if(server_waitclient(&s, &c)) continue;
		curr->client = c;
		if(!sblist_add(threads, &curr)) {
			close(curr->client.fd);
			free(curr);
			oom:
			dolog("rejecting connection due to OOM\n");
			usleep(16); /* prevent 100% CPU usage in OOM situation */
			continue;
		}
		pthread_attr_t *a = 0, attr;
		if(pthread_attr_init(&attr) == 0) {
			a = &attr;
			pthread_attr_setstacksize(a, THREAD_STACK_SIZE);
		}
		if(pthread_create(&curr->pt, a, clientthread, curr) != 0)
			dolog("pthread_create failed. OOM?\n");
		if(a) pthread_attr_destroy(&attr);
	}
}
