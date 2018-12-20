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
#include <sys/select.h>
#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include "server.h"
#include "sblist.h"

#ifndef MAX
#define MAX(x, y) ((x) > (y) ? (x) : (y))
#endif

#if !defined(PTHREAD_STACK_MIN) || defined(__APPLE__)
/* MAC says its min is 8KB, but then crashes in our face. thx hunkOLard */
#undef PTHREAD_STACK_MIN
#define PTHREAD_STACK_MIN 64*1024
#elif defined(__GLIBC__)
#undef PTHREAD_STACK_MIN
#define PTHREAD_STACK_MIN 32*1024
#endif

static const struct server* server;
static int bind_mode;
unsigned long timeout;
static sblist* targets;

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

static struct timeval* make_timeval(struct timeval* tv, unsigned long timeout) {
	if(!tv) return NULL;
	tv->tv_sec = timeout / 1000;
	tv->tv_usec = 1000 * (timeout % 1000);
	return tv;
}

static int connect_target(struct client *client) {
	size_t i;
	union sockaddr_union target;
	for(i=0; i<sblist_getsize(targets); i++) {
		target = *(union sockaddr_union*)sblist_get(targets, i);
		int af = target.v4.sin_family,
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
		if(bind_mode && server_bindtoip(server, fd) == -1)
			goto eval_errno;

		int flags = fcntl(fd, F_GETFL);
		if(fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
			err_fcntl:
			close(fd);
			perror("fcntl");
			continue;
		}

		if(connect(fd, (void*)&target, sizeof(target)) == -1) {
			int e = errno;
			if (!(e == EINPROGRESS || e == EWOULDBLOCK))
				goto eval_errno;
		}

		if(fcntl(fd, F_SETFL, flags) == -1)
			goto err_fcntl;

		fd_set wset;
		struct timeval tv;
		int optval, ret;
		socklen_t optlen = sizeof(optval);

		FD_ZERO(&wset);
		FD_SET(fd, &wset);

		ret = select(fd+1, NULL, &wset, NULL, timeout ? make_timeval(&tv, timeout*1000) : NULL);
		if(ret == 1 && FD_ISSET(fd, &wset)) {
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
			af = client->addr.v4.sin_family;
			ipdata = af == AF_INET ? (void*)&client->addr.v4.sin_addr : (void*)&client->addr.v6.sin6_addr;
			inet_ntop(af, ipdata, clientname, sizeof clientname);

			af = target.v4.sin_family;
			ipdata = af == AF_INET ? (void*)&target.v4.sin_addr : (void*)&target.v6.sin6_addr;
			inet_ntop(af, ipdata, servname, sizeof servname);
			dolog("client[%d] %s: connected to %s:%d\n", client->fd, clientname, servname, htons(af == AF_INET ? target.v4.sin_port : target.v6.sin6_port));
		}
		return fd;
	}
	return -1;
}

static void copyloop(int fd1, int fd2) {
	int maxfd = fd2;
	if(fd1 > fd2) maxfd = fd1;
	fd_set fdsc, fds;
	FD_ZERO(&fdsc);
	FD_SET(fd1, &fdsc);
	FD_SET(fd2, &fdsc);

	while(1) {
		memcpy(&fds, &fdsc, sizeof(fds));
		/* inactive connections are reaped after 15 min to free resources.
		   usually programs send keep-alive packets so this should only happen
		   when a connection is really unused. */
		struct timeval timeout = {.tv_sec = 60*15, .tv_usec = 0};
		switch(select(maxfd+1, &fds, 0, 0, &timeout)) {
			case 0:
				return;
			case -1:
				if(errno == EINTR) continue;
				else perror("select");
				return;
		}
		int infd = FD_ISSET(fd1, &fds) ? fd1 : fd2;
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

static int usage(void) {
	dprintf(2,
		"RR Redir - a round-robin port redirector\n"
		"----------------------------------------\n"
		"usage: rrredir [-b -i listenip -p port -t timeout] ip1:port1 ip2:port2 ...\n"
		"all arguments are optional.\n"
		"by default listenip is 0.0.0.0 and port 1080.\n\n"
		"option -b forces outgoing connections to be bound to the ip specified with -i\n"
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
	const char *listenip = "0.0.0.0";
	unsigned port = 1080;
	timeout = 0;
	while((c = getopt(argc, argv, ":bi:p:t:")) != -1) {
		switch(c) {
			case 'b':
				bind_mode = 1;
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
	targets = sblist_new(sizeof(union sockaddr_union), 8);
	while(argv[optind]) {
		char *p = strchr(argv[optind], ':');
		if(!p) {
			dprintf(2, "error: expected ip:port tuple\n");
			return usage();
		}
		*p = 0;
		int tport = atoi(p+1);
		struct addrinfo* remote;
		if(resolve(argv[optind], tport, &remote)) {
			*p = ':';
			dprintf(2, "error: cannot resolve %s\n", argv[optind]);
			return 1;
		}
		union sockaddr_union a;
		memcpy(&a, remote->ai_addr, remote->ai_addrlen);
		sblist_add(targets, &a);
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
	size_t stacksz = MAX(8192, PTHREAD_STACK_MIN);  /* 4KB for us, 4KB for libc */

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
			pthread_attr_setstacksize(a, stacksz);
		}
		if(pthread_create(&curr->pt, a, clientthread, curr) != 0)
			dolog("pthread_create failed. OOM?\n");
		if(a) pthread_attr_destroy(&attr);
	}
}
