/* posix */
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

/* bsd extensions */
#include <sys/uio.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "priv.h"

ssize_t sendto (int fd, __const void *a, size_t n,
			int flags, __CONST_SOCKADDR_ARG to,
			socklen_t tolen)
{
	/* actually, should do connect if not done already */
	return send(fd, a, n, flags);
}


ssize_t recvfrom (int fd, void *__restrict a, size_t n,
			  int flags, __SOCKADDR_ARG from,
			  socklen_t *__restrict fromlen)
{
	if(getsockname(fd, from, fromlen) < 0)
		return -1;
	return recv(fd, a, n, flags);
}
