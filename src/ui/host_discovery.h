#ifndef HOST_DISCOVERY_H
#define HOST_DISCOVERY_H

#define DISCOVERY_MAX_HOSTS 12

typedef struct {
    char name[64];
    char address[16];
} discovered_host_t;

typedef struct {
    int fd;
    int sent;
    double started;
    int count;
    discovered_host_t hosts[DISCOVERY_MAX_HOSTS];
} host_discovery_t;

void discovery_start(host_discovery_t *d);
void discovery_stop(host_discovery_t *d);
int discovery_poll(host_discovery_t *d);

#endif
