#ifndef ASCENT_DNS_RESOLVER_H
#define ASCENT_DNS_RESOLVER_H
#include <stddef.h>
#include <stdint.h>

#define DNS_MAX_SERVERS 4
struct dns_resolver {
    uint32_t servers[DNS_MAX_SERVERS]; /* network byte order */
    size_t server_count;
    int timeout_ms;
    int attempts;
};

int dns_build_query(uint8_t *out, size_t capacity, uint16_t id,
                    const char *name);
int dns_parse_a_reply(const uint8_t *packet, size_t length, uint16_t id,
                      uint32_t *address);
int dns_resolve_a(const struct dns_resolver *resolver, const char *name,
                  uint32_t *address);
int dns_build_query_type(uint8_t *out, size_t capacity, uint16_t id,
                         const char *name, uint16_t type);
int dns_parse_aaaa_reply(const uint8_t *packet, size_t length, uint16_t id,
                         uint8_t address[16]);
int dns_resolve_aaaa(const struct dns_resolver *resolver, const char *name,
                     uint8_t address[16]);
#endif
