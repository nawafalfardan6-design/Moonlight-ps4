#include "host_discovery.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static const char service[] = "_nvstream._tcp.local";
static const uint8_t query[] = {
    0x4d, 0x4c, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0,
    9, '_','n','v','s','t','r','e','a','m',
    4, '_','t','c','p', 5, 'l','o','c','a','l', 0,
    0, 12, 0, 1
};

static unsigned u16(const uint8_t *p) {
    return (unsigned)p[0] * 256 + p[1];
}

/* Decode a DNS name, following compression pointers with a bounded hop count. */
static int read_name(const uint8_t *p, size_t len, size_t *off, char out[256]) {
    size_t pos = *off, used = 0;
    int jumped = 0;
    for (int hops = 0; hops < 128 && pos < len; hops++) {
        unsigned n = p[pos++];
        if ((n & 0xc0) == 0xc0) {
            if (pos >= len)
                return 0;
            size_t target = (size_t)(n & 0x3f) * 256 + p[pos++];
            if (!jumped)
                *off = pos;
            jumped = 1;
            pos = target;
            continue;
        }
        if (!n) {
            if (!jumped)
                *off = pos;
            out[used] = 0;
            return 1;
        }
        if (n > 63 || pos + n > len || used + n + (used != 0) >= 256 ||
            memchr(p + pos, 0, n))
            return 0;
        if (used)
            out[used++] = '.';
        memcpy(out + used, p + pos, n);
        pos += n;
        used += n;
    }
    return 0;
}

typedef struct {
    char owner[256];
    unsigned type, class, size;
    size_t data;
} dns_record_t;

static int read_record(const uint8_t *p, size_t len, size_t *off, dns_record_t *r) {
    if (!read_name(p, len, off, r->owner) || len - *off < 10)
        return 0;
    r->type = u16(p + *off);
    r->class = u16(p + *off + 2) & 0x7fff;
    r->size = u16(p + *off + 8);
    r->data = *off + 10;
    *off = r->data + r->size;
    return *off <= len;
}

static void add_host(host_discovery_t *d, const char *name, const uint8_t *ip) {
    char address[16];
    if (!inet_ntop(AF_INET, ip, address, sizeof(address)))
        return;
    for (int i = 0; i < d->count; i++) {
        if (!strcmp(d->hosts[i].address, address))
            return;
    }
    if (d->count == DISCOVERY_MAX_HOSTS)
        return;

    discovered_host_t *host = &d->hosts[d->count++];
    snprintf(host->address, sizeof(host->address), "%s", address);
    size_t name_len = strlen(name) - sizeof(service);
    snprintf(host->name, sizeof(host->name), "%.*s", (int)name_len, name);
}

/* A legacy unicast browse reply carries PTR, SRV and IPv4 records together. */
static void read_response(host_discovery_t *d, const uint8_t *p, size_t len) {
    if (len < 12 || u16(p) != u16(query) || (p[2] & 0xfa) != 0x80 ||
        (p[3] & 0x0f))
        return;

    size_t first = 12;
    char name[256];
    for (unsigned i = 0; i < u16(p + 4); i++) {
        if (!read_name(p, len, &first, name) || len - first < 4)
            return;
        first += 4;
    }

    unsigned records = u16(p + 6) + u16(p + 8) + u16(p + 10);
    size_t off = first;
    dns_record_t srv;
    for (unsigned i = 0; i < records; i++) {
        if (!read_record(p, len, &off, &srv))
            return;
        size_t owner_len = strlen(srv.owner);
        if (srv.type != 33 || srv.class != 1 || srv.size < 7 ||
            u16(p + srv.data + 4) != 47989 || owner_len <= sizeof(service) ||
            srv.owner[owner_len - sizeof(service)] != '.' ||
            strcasecmp(srv.owner + owner_len - sizeof(service) + 1, service))
            continue;

        size_t target = srv.data + 6;
        if (!read_name(p, len, &target, name) || target != off || !name[0])
            continue;

        size_t addr_off = first;
        dns_record_t addr;
        for (unsigned j = 0; j < records; j++) {
            if (!read_record(p, len, &addr_off, &addr))
                return;
            if (addr.type == 1 && addr.class == 1 && addr.size == 4 &&
                !strcasecmp(addr.owner, name))
                add_host(d, srv.owner, p + addr.data);
        }
    }
}

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1000000000.0;
}

void discovery_stop(host_discovery_t *d) {
    if (d->fd >= 0)
        close(d->fd);
    d->fd = -1;
}

void discovery_start(host_discovery_t *d) {
    discovery_stop(d);
    d->count = 0;
    d->sent = 0;
    d->started = now();
    d->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (d->fd >= 0 && fcntl(d->fd, F_SETFL, O_NONBLOCK) < 0)
        discovery_stop(d);
}

int discovery_poll(host_discovery_t *d) {
    if (d->fd < 0)
        return 0;

    double elapsed = now() - d->started;
    if (elapsed >= 4.0) {
        discovery_stop(d);
        return 1;
    }

    if (d->sent < 3 && elapsed >= d->sent) {
        struct sockaddr_in multicast = {0};
        multicast.sin_family = AF_INET;
        multicast.sin_port = htons(5353);
        multicast.sin_addr.s_addr = inet_addr("224.0.0.251");
        if (sendto(d->fd, query, sizeof(query), 0,
                   (struct sockaddr *)&multicast, sizeof(multicast)) < 0) {
            discovery_stop(d);
            return 1;
        }
        d->sent++;
    }

    int previous = d->count;
    for (int i = 0; i < 8; i++) {
        uint8_t packet[2048];
        struct sockaddr_in sender = {0};
        socklen_t sender_len = sizeof(sender);
        ssize_t size = recvfrom(d->fd, packet, sizeof(packet), 0,
                                (struct sockaddr *)&sender, &sender_len);
        if (size <= 0)
            break;
        if (sender.sin_port == htons(5353))
            read_response(d, packet, (size_t)size);
    }
    return previous != d->count;
}
