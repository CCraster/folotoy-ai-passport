#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "portal.h"
#include "web_code.h"

int main(void)
{
    uint8_t dns[] = {0x12,0x34,1,0,0,1,0,0,0,0,0,0,3,'w','w','w',7,'e','x','a','m','p','l','e',3,'c','o','m',0,0,1,0,1};
    uint8_t reply[512];
    uint32_t address = 0x0104a8c0;
    size_t size = portal_dns_reply(dns, sizeof dns, reply, sizeof reply, address);
    assert(size == sizeof dns + 16 && reply[7] == 1 && reply[0] == 0x12);
    assert(!memcmp(reply + size - 4, &address, 4));
    for (size_t n = 0; n < sizeof dns; ++n) assert(!portal_dns_reply(dns,n,reply,sizeof reply,address));
    assert(!portal_dns_reply(dns,sizeof dns,reply,size-1,address));
    dns[30] = 28; // AAAA must receive an empty answer, not an invalid IPv4 record.
    assert(portal_dns_reply(dns,sizeof dns,reply,sizeof reply,address) == sizeof dns && reply[7] == 0);
    dns[12] = 0xc0; // Compressed and out-of-bounds names are rejected.
    assert(!portal_dns_reply(dns,sizeof dns,reply,sizeof reply,address));
    uint32_t random = 7;
    for (size_t n = 0; n < 512; ++n) {
        uint8_t packet[512];
        for (size_t i = 0; i < n; ++i) { random = random * 1664525 + 1013904223; packet[i] = random >> 24; }
        portal_dns_reply(packet,n,reply,sizeof reply,address);
    }
    web_code_t code = {.value="012345"};
    assert(web_code_check(&code,"012345",1) == WEB_CODE_OK);
    assert(web_code_check(&code,"012345extra",2) == WEB_CODE_WRONG);
    for (int i=0;i<4;++i) assert(web_code_check(&code,"wrong",10+i) == WEB_CODE_WRONG);
    assert(web_code_check(&code,"012345",100) == WEB_CODE_BLOCKED);
    assert(web_code_check(&code,"012345",60000020) == WEB_CODE_OK);
    memset(&code,0,sizeof code);
    assert(web_code_check(&code,"",70000000) == WEB_CODE_WRONG);
    puts("Portal tests passed: DNS bounds, IPv4/IPv6 queries, exact access code and retry limit.");
}
