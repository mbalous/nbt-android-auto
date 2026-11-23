/* Exercise DsiClient against a local synthetic DSI library; no service is contacted. */
#include "dsi_client.hpp"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready = PTHREAD_COND_INITIALIZER;
static bool received;

static void require(bool ok, const char *what) {
    if (!ok) { fprintf(stderr, "DSI probe: %s\n", what); exit(1); }
}
static void attached(nbt::DsiClient *client, void *) {
    std::vector<uint8_t> payload;
    require(client->attached() && client->setNotification(0x102), "notification");
    nbt::DsiClient::putU32(payload, 0x11223344);
    nbt::DsiClient::putString(payload, "abc");
    require(client->sendRequest(0x1357, payload), "send");
}
static void data(nbt::DsiClient *, uint32_t type, uint32_t id, uint32_t seq,
                 const uint8_t *payload, uint32_t len, void *) {
    const uint8_t expected[] = {0x11, 0x22, 0x33};
    require(type == 0x100 && id == 0x2468 && seq == 9 && len == sizeof(expected) &&
            payload && !memcmp(payload, expected, sizeof(expected)), "data callback");
    pthread_mutex_lock(&lock);
    received = true;
    pthread_cond_signal(&ready);
    pthread_mutex_unlock(&lock);
}
int main(int argc, char **argv) {
    alarm(10);
    require(argc == 2 && setenv("NBT_DSI_LIB", argv[1], 1) == 0, "fixture path");
    nbt::DsiClient client;
    client.setCallbacks(attached, data, NULL);
    require(client.open("TestInterface", "TestRole", 1, 2), "open");
    pthread_mutex_lock(&lock);
    while (!received) pthread_cond_wait(&ready, &lock);
    pthread_mutex_unlock(&lock);
    puts("DSI_FIXTURE_OK notification request callbacks");
    return 0;
}
