/*
 * net - the network, one layer at a time: a UDP exchange with the resolver of
 * /etc/resolv.conf, getaddrinfo(), then an HTTP request over TCP. Each step
 * says what it saw, so a failure points at a layer.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int failures;

static void fail(const char *what)
{
	printf("FAIL: %s (errno %d: %s)\n", what, errno, strerror(errno));
	failures++;
}

static int resolver(struct sockaddr_in *sa)
{
	char line[256], addr[64];
	FILE *f = fopen("/etc/resolv.conf", "r");
	int found = 0;

	if (!f)
		return 0;
	while (!found && fgets(line, sizeof(line), f))
		found = sscanf(line, "nameserver %63s", addr) == 1 &&
			inet_pton(AF_INET, addr, &sa->sin_addr) == 1;
	fclose(f);
	sa->sin_family = AF_INET;
	sa->sin_port = htons(53);
	if (found)
		printf("resolver: %s\n", addr);
	return found;
}

static void udp_dns(void)
{
	/* A query for the A records of example.com. */
	static const unsigned char query[] = {
		0x12, 0x34, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		7, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 3, 'c', 'o', 'm', 0,
		0x00, 0x01, 0x00, 0x01,
	};
	unsigned char answer[512];
	struct sockaddr_in sa = { 0 };
	struct pollfd pfd;
	int fd, n;

	if (!resolver(&sa)) {
		fail("no nameserver in /etc/resolv.conf");
		return;
	}
	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof(sa))) {
		fail("UDP socket or connect");
		return;
	}
	if (send(fd, query, sizeof(query), 0) != sizeof(query))
		fail("UDP send");
	pfd.fd = fd;
	pfd.events = POLLIN;
	pfd.revents = 0;
	n = poll(&pfd, 1, 5000);
	printf("udp: poll returned %d, revents %#x\n", n, pfd.revents);
	if (n != 1) {
		fail("no answer from the resolver");
	} else {
		n = recv(fd, answer, sizeof(answer), 0);
		printf("udp: %d bytes, id %02x%02x, %d answers\n", n, answer[0], answer[1],
		       n >= 8 ? answer[7] : -1);
		if (n < 12 || answer[0] != 0x12 || answer[1] != 0x34)
			fail("UDP answer");
	}
	close(fd);
}

static void http_get(void)
{
	static const char request[] =
		"GET / HTTP/1.0\r\nHost: example.com\r\nConnection: close\r\n\r\n";
	struct addrinfo hints = { 0 }, *res = NULL;
	char buf[4096], addr[64];
	int ret, fd, n, total = 0, ok = 0;

	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	ret = getaddrinfo("example.com", "80", &hints, &res);
	if (ret) {
		printf("getaddrinfo: %s\n", gai_strerror(ret));
		fail("getaddrinfo");
		return;
	}
	inet_ntop(AF_INET, &((struct sockaddr_in *)res->ai_addr)->sin_addr, addr, sizeof(addr));
	printf("tcp: example.com is %s\n", addr);

	fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if (fd < 0 || connect(fd, res->ai_addr, res->ai_addrlen)) {
		fail("TCP connect");
		freeaddrinfo(res);
		return;
	}
	freeaddrinfo(res);
	if (write(fd, request, sizeof(request) - 1) != sizeof(request) - 1)
		fail("TCP write");
	while ((n = read(fd, buf, sizeof(buf) - 1)) > 0) {
		buf[n] = 0;
		if (!total)
			printf("tcp: %.15s\n", buf);
		if (strstr(buf, "Example Domain"))
			ok = 1;
		total += n;
	}
	if (n < 0)
		fail("TCP read");
	printf("tcp: %d bytes\n", total);
	if (!ok)
		fail("the page is not the one expected");
	close(fd);
}

int main(void)
{
	udp_dns();
	http_get();
	printf("%s\n", failures ? "FAIL" : "PASS");
	return failures ? 1 : 0;
}
