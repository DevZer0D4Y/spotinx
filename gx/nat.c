/*
 * nat.c - a network device for LKL made of host sockets.
 *
 * The kernel's network stack is the guest's (sockets, poll and epoll all stay
 * inside LKL), but a homebrew on Horizon has no network card to give it, only
 * BSD sockets. So this device answers the Ethernet frames of the kernel
 * itself: it is the gateway and every remote machine at once. ARP is answered
 * directly, each UDP flow becomes a host UDP socket, and each TCP connection
 * is terminated here and carried on by a host TCP socket.
 *
 * The TCP side is small because the "wire" to the kernel loses nothing and
 * never reorders: no congestion control, a fixed window, and go-back-N from a
 * timer for the rare frame the kernel drops. Only what POSIX sockets offer is
 * used, so the same file serves the PC and the Switch.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <lkl_host.h>
#include "nat.h"
#include "gx.h"

/* Says what went wrong, the first few times of each kind: a page that does
 * not load leaves nothing else to tell why. */
#define COMPLAIN(...)                                \
	do {                                         \
		static int said;                     \
		if (said < 6 && ++said)              \
			host_log("gx: nat: " __VA_ARGS__); \
	} while (0)

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

/* A browser keeps a hundred connections open and asks for names by the dozen,
 * each question from a port of its own. */
#define MAX_TCP 160
#define MAX_UDP 64
#define QUEUE_FRAMES 256
#define FRAME_MAX 1514
#define MSS 1460
#define TCP_BUFFER 65536
#define WINDOW 65535
#define RETRANSMIT_MS 1000
#define CONNECT_MS 30000
#define UDP_IDLE_MS 60000

#define TH_FIN 0x01
#define TH_SYN 0x02
#define TH_RST 0x04
#define TH_PSH 0x08
#define TH_ACK 0x10

static const uint8_t gateway_mac[6] = { 0x52, 0x55, 0x0a, 0x00, 0x02, 0x02 };
static const uint8_t guest_mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };

enum { T_FREE, T_CONNECTING, T_SYNACK, T_OPEN };

/* Addresses and ports are kept in host byte order. */
struct tcp_conn {
	int state, fd;
	uint32_t remote;
	uint16_t gport, rport;
	uint32_t rcv_nxt;                  /* next byte expected from the guest */
	uint32_t iss, snd_una, snd_nxt;    /* our sequence space, towards the guest */
	uint32_t guest_window;
	uint8_t *buf;                      /* from the host, not yet acked: starts at snd_una */
	uint32_t buf_len;
	int host_eof, fin_sent, fin_acked, guest_fin, dup_acks;
	uint64_t timer;                    /* last progress, for retransmission */
};

struct udp_flow {
	int fd;                            /* -1 when free */
	uint16_t gport;
	uint64_t used;
	/* A question to the resolver that has no answer yet: asked again
	 * here, sooner than the guest would, since one lost datagram is a
	 * page that does not load. */
	uint8_t question[512];
	uint16_t question_len, question_port;
	uint8_t asked;                     /* times: 0 when there is none waiting */
	uint64_t asked_at, first_asked;
};

struct frame {
	uint16_t len;
	uint8_t data[FRAME_MAX];
};

struct nat {
	struct lkl_netdev dev;
	pthread_mutex_t lock;
	struct frame *queue;               /* frames for the kernel */
	int head, count;
	int wake_fd;                       /* a UDP socket connected to itself */
	int closing;
	uint32_t dns, dns2;                /* host byte order; dns2 may be 0 */
	uint16_t ip_id;
	struct tcp_conn tcp[MAX_TCP];
	struct udp_flow udp[MAX_UDP];
};

static uint64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void set_nonblocking(int fd)
{
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}

static void wake(struct nat *nat)
{
	char c = 0;

	if (nat->wake_fd >= 0)
		send(nat->wake_fd, &c, 1, MSG_NOSIGNAL);
}

/* ---------- building frames for the kernel ---------- */

static void put16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
static void put32(uint8_t *p, uint32_t v) { put16(p, v >> 16); put16(p + 2, v); }
static uint16_t get16(const uint8_t *p) { return p[0] << 8 | p[1]; }
static uint32_t get32(const uint8_t *p) { return (uint32_t)get16(p) << 16 | get16(p + 2); }

static uint32_t sum16(const uint8_t *p, size_t len, uint32_t sum)
{
	for (; len > 1; p += 2, len -= 2)
		sum += p[0] << 8 | p[1];
	if (len)
		sum += p[0] << 8;
	return sum;
}

static uint16_t fold(uint32_t sum)
{
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	return ~sum;
}

/* The next free slot of the queue, or NULL if the kernel is not keeping up. */
static struct frame *queue_slot(struct nat *nat)
{
	if (nat->count == QUEUE_FRAMES)
		return NULL;
	return &nat->queue[(nat->head + nat->count) % QUEUE_FRAMES];
}

/* Fills the Ethernet and IPv4 headers of an IPv4 frame whose payload (l4_len
 * bytes at data + 34) is already there, checksums it and queues it. */
static void queue_ip(struct nat *nat, struct frame *f, uint8_t proto, uint32_t src,
		     size_t l4_len)
{
	uint8_t *eth = f->data, *ip = eth + 14, *l4 = ip + 20;
	uint32_t sum;

	memcpy(eth, guest_mac, 6);
	memcpy(eth + 6, gateway_mac, 6);
	put16(eth + 12, 0x0800);
	ip[0] = 0x45;
	ip[1] = 0;
	put16(ip + 2, 20 + l4_len);
	put16(ip + 4, nat->ip_id++);
	put16(ip + 6, 0x4000);             /* don't fragment */
	ip[8] = 64;
	ip[9] = proto;
	put16(ip + 10, 0);
	put32(ip + 12, src);
	put32(ip + 16, NAT_GUEST);
	put16(ip + 10, fold(sum16(ip, 20, 0)));

	sum = sum16(ip + 12, 8, proto + l4_len);
	if (proto == IPPROTO_TCP) {
		put16(l4 + 16, 0);
		put16(l4 + 16, fold(sum16(l4, l4_len, sum)));
	} else {
		uint16_t check;

		put16(l4 + 6, 0);
		check = fold(sum16(l4, l4_len, sum));
		put16(l4 + 6, check ? check : 0xffff);
	}
	f->len = 34 + l4_len;
	nat->count++;
}

/* Queues a TCP segment; with options, a SYN-ACK announcing our MSS. Returns 0
 * if the queue was full. */
static int queue_tcp(struct nat *nat, uint32_t remote, uint16_t rport, uint16_t gport,
		     uint32_t seq, uint32_t ack, uint8_t flags, const uint8_t *data, size_t len)
{
	struct frame *f = queue_slot(nat);
	size_t hdr = (flags & TH_SYN) ? 24 : 20;
	uint8_t *tcp;

	if (!f)
		return 0;
	tcp = f->data + 34;
	put16(tcp, rport);
	put16(tcp + 2, gport);
	put32(tcp + 4, seq);
	put32(tcp + 8, ack);
	tcp[12] = hdr / 4 << 4;
	tcp[13] = flags;
	put16(tcp + 14, WINDOW);
	put16(tcp + 18, 0);
	if (flags & TH_SYN) {
		tcp[20] = 2;               /* maximum segment size */
		tcp[21] = 4;
		put16(tcp + 22, MSS);
	}
	if (len)
		memcpy(tcp + hdr, data, len);
	queue_ip(nat, f, IPPROTO_TCP, remote, hdr + len);
	return 1;
}

/* ---------- TCP ---------- */

static void tcp_free(struct tcp_conn *c)
{
	if (c->fd >= 0)
		close(c->fd);
	free(c->buf);
	memset(c, 0, sizeof(*c));
	c->fd = -1;
}

/* Tells the guest the connection is gone, and forgets it. */
static void tcp_reset(struct nat *nat, struct tcp_conn *c)
{
	queue_tcp(nat, c->remote, c->rport, c->gport, c->snd_nxt, c->rcv_nxt, TH_RST | TH_ACK,
		  NULL, 0);
	tcp_free(c);
}

static void tcp_ack(struct nat *nat, struct tcp_conn *c)
{
	queue_tcp(nat, c->remote, c->rport, c->gport, c->snd_nxt, c->rcv_nxt, TH_ACK, NULL, 0);
}

/* Sends what the guest's window allows of the buffered data, then our FIN. */
static void tcp_output(struct nat *nat, struct tcp_conn *c)
{
	if (c->state != T_OPEN)
		return;
	for (;;) {
		uint32_t off = c->snd_nxt - c->snd_una, len;

		if (off >= c->buf_len || off >= c->guest_window)
			break;
		len = c->buf_len - off;
		if (len > MSS)
			len = MSS;
		if (len > c->guest_window - off)
			len = c->guest_window - off;
		if (!queue_tcp(nat, c->remote, c->rport, c->gport, c->snd_nxt, c->rcv_nxt,
			       TH_ACK | TH_PSH, c->buf + off, len))
			return;
		c->snd_nxt += len;
	}
	if (c->host_eof && !c->fin_sent && c->snd_nxt - c->snd_una == c->buf_len &&
	    queue_tcp(nat, c->remote, c->rport, c->gport, c->snd_nxt, c->rcv_nxt,
		      TH_FIN | TH_ACK, NULL, 0)) {
		c->fin_sent = 1;
		c->snd_nxt++;
	}
}

static struct tcp_conn *tcp_find(struct nat *nat, uint16_t gport, uint32_t remote, uint16_t rport)
{
	for (int i = 0; i < MAX_TCP; i++) {
		struct tcp_conn *c = &nat->tcp[i];

		if (c->state != T_FREE && c->gport == gport && c->remote == remote &&
		    c->rport == rport)
			return c;
	}
	return NULL;
}

static void tcp_open(struct nat *nat, uint16_t gport, uint32_t remote, uint16_t rport,
		     uint32_t seq, uint32_t window)
{
	struct sockaddr_in sa = { 0 };
	struct tcp_conn *c = NULL;

	for (int i = 0; i < MAX_TCP && !c; i++)
		if (nat->tcp[i].state == T_FREE)
			c = &nat->tcp[i];
	if (c) {
		c->buf = malloc(TCP_BUFFER);
		c->fd = c->buf ? socket(AF_INET, SOCK_STREAM, 0) : -1;
	}
	if (!c || c->fd < 0) {
		/* No room for it now: nothing is answered, and the guest asks
		 * again in a second, when there may be. Refusing would be an
		 * error on the page. */
		if (!c)
			COMPLAIN("%d connections at once, one more waits\n", MAX_TCP);
		else
			COMPLAIN("no socket for a connection (error %d)\n", errno);
		if (c)
			tcp_free(c);
		return;
	}
	set_nonblocking(c->fd);
	sa.sin_family = AF_INET;
	sa.sin_port = htons(rport);
	sa.sin_addr.s_addr = htonl(remote == NAT_GATEWAY ? INADDR_LOOPBACK : remote);
	c->state = T_CONNECTING;
	c->gport = gport;
	c->remote = remote;
	c->rport = rport;
	c->rcv_nxt = seq + 1;
	c->guest_window = window;
	c->timer = now_ms();
	if (connect(c->fd, (struct sockaddr *)&sa, sizeof(sa)) && errno != EINPROGRESS) {
		COMPLAIN("cannot connect to %u.%u.%u.%u:%u (error %d)\n", remote >> 24, remote >> 16 & 255,
			 remote >> 8 & 255, remote & 255, rport, errno);
		tcp_reset(nat, c);
	}
}

/* A segment from the guest. */
static void tcp_input(struct nat *nat, uint32_t remote, const uint8_t *tcp, size_t len)
{
	uint16_t gport = get16(tcp), rport = get16(tcp + 2);
	uint32_t seq = get32(tcp + 4), ack = get32(tcp + 8);
	size_t hdr = (tcp[12] >> 4) * 4;
	uint8_t flags = tcp[13];
	uint32_t window = get16(tcp + 14);
	struct tcp_conn *c = tcp_find(nat, gport, remote, rport);
	size_t data_len;

	if (hdr < 20 || hdr > len)
		return;
	data_len = len - hdr;

	if (!c) {
		if ((flags & (TH_SYN | TH_ACK | TH_RST)) == TH_SYN)
			tcp_open(nat, gport, remote, rport, seq, window);
		else if (!(flags & TH_RST))
			queue_tcp(nat, remote, rport, gport, (flags & TH_ACK) ? ack : 0,
				  seq + data_len + !!(flags & (TH_SYN | TH_FIN)), TH_RST | TH_ACK,
				  NULL, 0);
		return;
	}
	if (flags & TH_RST) {
		tcp_free(c);
		return;
	}
	if (flags & TH_SYN) {
		/* Its SYN again: ours got lost, or the host is still connecting. */
		if (c->state == T_SYNACK)
			queue_tcp(nat, remote, rport, gport, c->iss, c->rcv_nxt, TH_SYN | TH_ACK,
				  NULL, 0);
		return;
	}
	if (c->state == T_CONNECTING || !(flags & TH_ACK))
		return;

	if (c->state == T_SYNACK) {
		if (ack != c->iss + 1)
			return;
		c->state = T_OPEN;
		c->snd_una = c->snd_nxt = ack;
		c->timer = now_ms();
	} else if (ack - c->snd_una > 0 && ack - c->snd_una <= c->snd_nxt - c->snd_una) {
		uint32_t acked = ack - c->snd_una;

		if (c->fin_sent && acked > c->buf_len) {
			c->fin_acked = 1;
			acked = c->buf_len;
		}
		memmove(c->buf, c->buf + acked, c->buf_len - acked);
		c->buf_len -= acked;
		c->snd_una = ack;
		c->dup_acks = 0;
		c->timer = now_ms();
	} else if (ack == c->snd_una && !data_len && window == c->guest_window &&
		   c->snd_nxt != c->snd_una && ++c->dup_acks == 3) {
		/* It is missing something: send again from what it acked. */
		c->snd_nxt = c->snd_una;
		c->fin_sent = c->fin_acked;
		c->dup_acks = 0;
	}
	c->guest_window = window;

	if (data_len || (flags & TH_FIN)) {
		size_t taken = 0;

		if (seq == c->rcv_nxt && data_len && !c->guest_fin) {
			ssize_t n = send(c->fd, tcp + hdr, data_len, MSG_NOSIGNAL);

			if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
				tcp_reset(nat, c);
				return;
			}
			/* What the host does not take is not acked: the guest
			 * will send it again. */
			taken = n > 0 ? n : 0;
			c->rcv_nxt += taken;
		}
		if ((flags & TH_FIN) && seq + data_len == c->rcv_nxt && taken == data_len &&
		    !c->guest_fin) {
			c->rcv_nxt++;
			c->guest_fin = 1;
			shutdown(c->fd, SHUT_WR);
		}
		tcp_ack(nat, c);
	}
	if (c->guest_fin && c->fin_acked) {
		tcp_free(c);
		return;
	}
	tcp_output(nat, c);
}

/* The host side of a connection has something to say. */
static void tcp_host_event(struct nat *nat, struct tcp_conn *c, int revents)
{
	if (c->state == T_CONNECTING) {
		int error = 0;
		socklen_t len = sizeof(error);

		if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &error, &len) || error ||
		    (revents & (POLLERR | POLLHUP | POLLNVAL))) {
			COMPLAIN("connection to %u.%u.%u.%u:%u failed (error %d, events %#x)\n",
				 c->remote >> 24, c->remote >> 16 & 255, c->remote >> 8 & 255,
				 c->remote & 255, c->rport, error, revents);
			tcp_reset(nat, c);
			return;
		}
		/* Any initial sequence number will do on a private wire. */
		c->iss = (uint32_t)now_ms() * 2654435761u + c->gport;
		c->snd_una = c->iss;
		c->snd_nxt = c->iss + 1;
		c->state = T_SYNACK;
		c->timer = now_ms();
		queue_tcp(nat, c->remote, c->rport, c->gport, c->iss, c->rcv_nxt, TH_SYN | TH_ACK,
			  NULL, 0);
		return;
	}
	if (c->state == T_OPEN && !c->host_eof && c->buf_len < TCP_BUFFER) {
		ssize_t n = recv(c->fd, c->buf + c->buf_len, TCP_BUFFER - c->buf_len, 0);

		if (n > 0) {
			if (!c->buf_len)
				c->timer = now_ms();
			c->buf_len += n;
		} else if (n == 0) {
			c->host_eof = 1;
		} else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
			tcp_reset(nat, c);
			return;
		}
		tcp_output(nat, c);
	}
}

/* Questions to the resolver that have waited a while are asked again. */
static void udp_timers(struct nat *nat, uint64_t now)
{
	for (int i = 0; i < MAX_UDP; i++) {
		struct udp_flow *f = &nat->udp[i];
		struct sockaddr_in sa = { 0 };

		if (f->fd < 0 || !f->asked || now - f->asked_at < (f->asked == 1 ? 300 : 1000))
			continue;
		if (f->asked >= 5) {
			COMPLAIN("a name got no answer in %u ms\n", (unsigned)(now - f->first_asked));
			f->asked = 0;
			continue;
		}
		f->asked++;
		f->asked_at = now;
		sa.sin_family = AF_INET;
		sa.sin_port = htons(f->question_port);
		sa.sin_addr.s_addr = htonl(nat->dns);
		sendto(f->fd, f->question, f->question_len, MSG_NOSIGNAL, (struct sockaddr *)&sa, sizeof(sa));
		if (nat->dns2) {
			sa.sin_addr.s_addr = htonl(nat->dns2);
			sendto(f->fd, f->question, f->question_len, MSG_NOSIGNAL, (struct sockaddr *)&sa,
			       sizeof(sa));
		}
	}
}

static void tcp_timers(struct nat *nat, uint64_t now)
{
	for (int i = 0; i < MAX_TCP; i++) {
		struct tcp_conn *c = &nat->tcp[i];

		if (c->state == T_CONNECTING && now - c->timer > CONNECT_MS) {
			tcp_reset(nat, c);
		} else if (c->state == T_SYNACK && now - c->timer > RETRANSMIT_MS) {
			c->timer = now;
			queue_tcp(nat, c->remote, c->rport, c->gport, c->iss, c->rcv_nxt,
				  TH_SYN | TH_ACK, NULL, 0);
		} else if (c->state == T_OPEN) {
			if (c->snd_nxt != c->snd_una && now - c->timer > RETRANSMIT_MS) {
				c->timer = now;
				c->snd_nxt = c->snd_una;
				c->fin_sent = c->fin_acked;
			}
			tcp_output(nat, c);
		}
	}
}

/* ---------- UDP ---------- */

static struct udp_flow *udp_flow(struct nat *nat, uint16_t gport)
{
	struct udp_flow *f = NULL, *oldest = &nat->udp[0];

	for (int i = 0; i < MAX_UDP; i++) {
		if (nat->udp[i].fd >= 0 && nat->udp[i].gport == gport)
			return &nat->udp[i];
		if (nat->udp[i].fd < 0 && !f)
			f = &nat->udp[i];
		if (nat->udp[i].used < oldest->used)
			oldest = &nat->udp[i];
	}
	if (!f) {
		if (now_ms() - oldest->used < 5000)
			COMPLAIN("%d UDP exchanges at once, the oldest is dropped\n", MAX_UDP);
		f = oldest;
		close(f->fd);
	}
	f->fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (f->fd < 0) {
		COMPLAIN("no socket for UDP (error %d)\n", errno);
		return NULL;
	}
	set_nonblocking(f->fd);
	f->gport = gport;
	return f;
}

static void udp_input(struct nat *nat, uint32_t remote, const uint8_t *udp, size_t len)
{
	struct sockaddr_in sa = { 0 };
	struct udp_flow *f;

	if (len < 8 || get16(udp + 4) < 8 || get16(udp + 4) > len)
		return;
	f = udp_flow(nat, get16(udp));
	if (!f)
		return;
	f->used = now_ms();
	sa.sin_family = AF_INET;
	sa.sin_port = htons(get16(udp + 2));
	if (remote == NAT_DNS) {
		size_t size = get16(udp + 4) - 8;

		/* To both resolvers of the host, and kept to ask again. */
		f->asked = 0;
		if (size <= sizeof(f->question)) {
			memcpy(f->question, udp + 8, size);
			f->question_len = size;
			f->question_port = get16(udp + 2);
			f->asked = 1;
			f->asked_at = f->first_asked = f->used;
		}
		if (nat->dns2) {
			sa.sin_addr.s_addr = htonl(nat->dns2);
			sendto(f->fd, udp + 8, size, MSG_NOSIGNAL, (struct sockaddr *)&sa, sizeof(sa));
		}
		remote = nat->dns;
	} else if (remote == NAT_GATEWAY) {
		remote = INADDR_LOOPBACK;
	}
	sa.sin_addr.s_addr = htonl(remote);
	sendto(f->fd, udp + 8, get16(udp + 4) - 8, MSG_NOSIGNAL, (struct sockaddr *)&sa, sizeof(sa));
}

static void udp_host_event(struct nat *nat, struct udp_flow *f)
{
	for (;;) {
		struct frame *slot = queue_slot(nat);
		struct sockaddr_in sa;
		socklen_t sa_len = sizeof(sa);
		uint32_t remote;
		uint8_t *udp;
		ssize_t n;

		if (!slot)
			return;
		udp = slot->data + 34;
		n = recvfrom(f->fd, udp + 8, FRAME_MAX - 42, 0, (struct sockaddr *)&sa, &sa_len);
		if (n < 0)
			return;
		remote = ntohl(sa.sin_addr.s_addr);
		if ((remote == nat->dns || (nat->dns2 && remote == nat->dns2)) && ntohs(sa.sin_port) == 53) {
			remote = NAT_DNS;
			if (f->asked) {
				uint64_t took = now_ms() - f->first_asked;

				if (took > 700)
					COMPLAIN("a name took %u ms to resolve (asked %d times)\n",
						 (unsigned)took, f->asked);
				f->asked = 0;
			} else if (ntohs(sa.sin_port) == 53) {
				continue;   /* the other resolver's answer to the same question */
			}
		}
		else if (remote == INADDR_LOOPBACK)
			remote = NAT_GATEWAY;
		put16(udp, ntohs(sa.sin_port));
		put16(udp + 2, f->gport);
		put16(udp + 4, 8 + n);
		queue_ip(nat, slot, IPPROTO_UDP, remote, 8 + n);
		f->used = now_ms();
	}
}

/* ---------- the device ---------- */

static void arp_input(struct nat *nat, const uint8_t *frame, size_t len)
{
	struct frame *f;
	uint8_t *arp;

	/* A request for anyone but the guest itself: we are everyone. */
	if (len < 42 || get16(frame + 20) != 1 || get32(frame + 38) == NAT_GUEST ||
	    !(f = queue_slot(nat)))
		return;
	memcpy(f->data, frame + 6, 6);
	memcpy(f->data + 6, gateway_mac, 6);
	put16(f->data + 12, 0x0806);
	arp = f->data + 14;
	memcpy(arp, frame + 14, 6);        /* hardware and protocol types, sizes */
	put16(arp + 6, 2);                 /* reply */
	memcpy(arp + 8, gateway_mac, 6);
	memcpy(arp + 14, frame + 38, 4);   /* the address asked for */
	memcpy(arp + 18, frame + 22, 10);  /* back to the asker */
	f->len = 42;
	nat->count++;
}

static int nat_tx(struct lkl_netdev *nd, struct iovec *iov, int cnt)
{
	struct nat *nat = (struct nat *)nd;
	uint8_t frame[FRAME_MAX], *ip = frame + 14;
	size_t len = 0, total = 0, ihl, ip_len;

	for (int i = 0; i < cnt; i++) {
		size_t n = iov[i].iov_len;

		total += n;
		if (n > sizeof(frame) - len)
			n = sizeof(frame) - len;
		memcpy(frame + len, iov[i].iov_base, n);
		len += n;
	}
	if (len < 34 || total > sizeof(frame))
		return total;

	pthread_mutex_lock(&nat->lock);
	if (get16(frame + 12) == 0x0806) {
		arp_input(nat, frame, len);
	} else if (get16(frame + 12) == 0x0800 && (ip[0] >> 4) == 4) {
		ihl = (ip[0] & 15) * 4;
		ip_len = get16(ip + 2);
		/* Whole datagrams only: nothing the guest sends needs fragments. */
		if (ihl >= 20 && ip_len >= ihl && ip_len <= len - 14 && !(get16(ip + 6) & 0x3fff)) {
			if (ip[9] == IPPROTO_TCP && ip_len - ihl >= 20)
				tcp_input(nat, get32(ip + 16), ip + ihl, ip_len - ihl);
			else if (ip[9] == IPPROTO_UDP)
				udp_input(nat, get32(ip + 16), ip + ihl, ip_len - ihl);
		}
	}
	if (nat->count)
		wake(nat);
	pthread_mutex_unlock(&nat->lock);
	return total;
}

static int nat_rx(struct lkl_netdev *nd, struct iovec *iov, int cnt)
{
	struct nat *nat = (struct nat *)nd;
	struct frame *f;
	size_t done = 0;
	int len = -1;

	pthread_mutex_lock(&nat->lock);
	if (nat->count) {
		f = &nat->queue[nat->head];
		len = f->len;
		for (int i = 0; i < cnt && done < f->len; i++) {
			size_t n = f->len - done < iov[i].iov_len ? f->len - done : iov[i].iov_len;

			memcpy(iov[i].iov_base, f->data + done, n);
			done += n;
		}
		nat->head = (nat->head + 1) % QUEUE_FRAMES;
		nat->count--;
	}
	pthread_mutex_unlock(&nat->lock);
	return len;
}

/* Waits for the host sockets and turns what they bring into frames. */
static int nat_poll(struct lkl_netdev *nd)
{
	struct nat *nat = (struct nat *)nd;
	struct pollfd fds[1 + MAX_TCP + MAX_UDP];
	/* What each entry was when the lock was dropped: it may be gone after. */
	struct { int index, fd; } who[1 + MAX_TCP + MAX_UDP];

	pthread_mutex_lock(&nat->lock);
	for (;;) {
		uint64_t now = now_ms();
		int n = 0, ready;

		if (nat->closing) {
			pthread_mutex_unlock(&nat->lock);
			return LKL_DEV_NET_POLL_HUP;
		}
		tcp_timers(nat, now);
		udp_timers(nat, now);
		if (nat->count) {
			pthread_mutex_unlock(&nat->lock);
			return LKL_DEV_NET_POLL_RX;
		}

		fds[n].fd = nat->wake_fd;
		fds[n].events = POLLIN;
		who[n++].index = -1;
		for (int i = 0; i < MAX_TCP; i++) {
			struct tcp_conn *c = &nat->tcp[i];
			short events = c->state == T_CONNECTING ? POLLOUT :
				       (c->state == T_OPEN && !c->host_eof &&
					c->buf_len < TCP_BUFFER) ? POLLIN : 0;

			if (!events)
				continue;
			fds[n].fd = c->fd;
			fds[n].events = events;
			who[n].index = i;
			who[n++].fd = c->fd;
		}
		for (int i = 0; i < MAX_UDP; i++) {
			struct udp_flow *f = &nat->udp[i];

			if (f->fd >= 0 && now - f->used > UDP_IDLE_MS) {
				close(f->fd);
				f->fd = -1;
			}
			if (f->fd < 0)
				continue;
			fds[n].fd = f->fd;
			fds[n].events = POLLIN;
			who[n].index = MAX_TCP + i;
			who[n++].fd = f->fd;
		}

		pthread_mutex_unlock(&nat->lock);
		/* Short, in case the socket that wakes us does not: frames then
		 * wait for the next look. */
		ready = poll(nat->wake_fd >= 0 ? fds : fds + 1, nat->wake_fd >= 0 ? n : n - 1,
			     nat->wake_fd >= 0 ? 20 : 5);
		pthread_mutex_lock(&nat->lock);
		if (ready <= 0)
			continue;

		if (nat->wake_fd >= 0 && fds[0].revents) {
			char drain[64];

			while (recv(nat->wake_fd, drain, sizeof(drain), 0) > 0)
				;
		}
		for (int k = 1; k < n; k++) {
			int i = who[k].index;

			if (!fds[k].revents)
				continue;
			if (i < MAX_TCP) {
				if (nat->tcp[i].state != T_FREE && nat->tcp[i].fd == who[k].fd)
					tcp_host_event(nat, &nat->tcp[i], fds[k].revents);
			} else if (nat->udp[i - MAX_TCP].fd == who[k].fd) {
				udp_host_event(nat, &nat->udp[i - MAX_TCP]);
			}
		}
	}
}

static void nat_poll_hup(struct lkl_netdev *nd)
{
	struct nat *nat = (struct nat *)nd;

	pthread_mutex_lock(&nat->lock);
	nat->closing = 1;
	wake(nat);
	pthread_mutex_unlock(&nat->lock);
}

static void nat_free(struct lkl_netdev *nd)
{
	struct nat *nat = (struct nat *)nd;

	for (int i = 0; i < MAX_TCP; i++)
		if (nat->tcp[i].state != T_FREE)
			tcp_free(&nat->tcp[i]);
	for (int i = 0; i < MAX_UDP; i++)
		if (nat->udp[i].fd >= 0)
			close(nat->udp[i].fd);
	if (nat->wake_fd >= 0)
		close(nat->wake_fd);
	free(nat->queue);
	free(nat);
}

static struct lkl_dev_net_ops nat_ops = {
	.tx = nat_tx,
	.rx = nat_rx,
	.poll = nat_poll,
	.poll_hup = nat_poll_hup,
	.free = nat_free,
};

/* A UDP socket on the loopback connected to itself: what poll() can wait on
 * together with the other sockets, where there are no pipes. */
static int wake_socket(void)
{
	struct sockaddr_in sa = { 0 };
	socklen_t len = sizeof(sa);
	int fd = socket(AF_INET, SOCK_DGRAM, 0);

	if (fd < 0)
		return -1;
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) ||
	    getsockname(fd, (struct sockaddr *)&sa, &len) ||
	    connect(fd, (struct sockaddr *)&sa, sizeof(sa))) {
		close(fd);
		return -1;
	}
	set_nonblocking(fd);
	return fd;
}

struct lkl_netdev *nat_create(uint32_t dns, uint32_t dns2, uint8_t mac[6])
{
	struct nat *nat = calloc(1, sizeof(*nat));

	if (!nat)
		return NULL;
	nat->queue = malloc(QUEUE_FRAMES * sizeof(*nat->queue));
	if (!nat->queue) {
		free(nat);
		return NULL;
	}
	pthread_mutex_init(&nat->lock, NULL);
	nat->dev.ops = &nat_ops;
	nat->dns = ntohl(dns);
	nat->dns2 = dns2 == dns ? 0 : ntohl(dns2);
	nat->wake_fd = wake_socket();
	for (int i = 0; i < MAX_TCP; i++)
		nat->tcp[i].fd = -1;
	for (int i = 0; i < MAX_UDP; i++)
		nat->udp[i].fd = -1;
	memcpy(mac, guest_mac, 6);
	return &nat->dev;
}
