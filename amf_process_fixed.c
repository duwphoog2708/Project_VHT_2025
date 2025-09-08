#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <time.h>
#include <netinet/sctp.h>

#define AMF_BASE_PORT 9101
#define GNB_PAGING_PORT 9200
#define NUM_UE 200

#define MSG_RRC_NGAP_REQ 0x12
#define MSG_NGAP_RESP     0x13
#define BM_RANDOM_VALUE 0x01
#define BM_5G_STMSI     0x02

typedef struct {
    uint8_t msgid;
    uint8_t bitmask;
    uint16_t ue_id;
    uint64_t tmsi;
    uint64_t s_tmsi;
} Message;

int amf_index_global;
int capacity_global;
int current_load = 0;
uint16_t registered_ues[NUM_UE] = {0};

void send_paging_to_gnb(Message *paging) {
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_SCTP);
    if (sock < 0) { perror("AMF SCTP socket"); return; }
    struct sockaddr_in serv = {0};
    serv.sin_family = AF_INET;
    serv.sin_port = htons(GNB_PAGING_PORT);
    serv.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (connect(sock, (struct sockaddr*)&serv, sizeof(serv)) < 0) { close(sock); return; }
    sctp_sendmsg(sock, paging, sizeof(*paging), NULL, 0, 0, 0, 0, 0, 0);
    close(sock);
}

void *send_paging_thread(void *arg) {
    Message *p = (Message *)arg;
    int rnds[6] = {500, 1000, 1500, 2000, 2500, 3000};
    int y = rnds[rand() % 6];
    usleep(y * 1000);
    send_paging_to_gnb(p);
    free(p);
    return NULL;
}

void *handle_connection(void *arg) {
    int fd = *(int*)arg;
    free(arg);
    Message req;
    int r = sctp_recvmsg(fd, &req, sizeof(req), NULL, 0, NULL, NULL);
    if (r <= 0) { close(fd); return NULL; }

    if (req.msgid == MSG_RRC_NGAP_REQ) {
        if (req.bitmask & BM_RANDOM_VALUE) {  // Removed capacity check
            uint64_t s = ((uint64_t)(amf_index_global & 0x3FF) << 30) | // Set ID (10 bits)
                         ((uint64_t)(amf_index_global & 0x3F) << 24) | // Pointer (6 bits)
                         (req.tmsi & 0xFFFFFF); // TMSI (24 bits)
            Message resp = {0};
            resp.msgid = MSG_NGAP_RESP;
            resp.bitmask = BM_RANDOM_VALUE;
            resp.ue_id = req.ue_id;
            resp.tmsi = req.tmsi;
            resp.s_tmsi = s;
            sctp_sendmsg(fd, &resp, sizeof(resp), NULL, 0, 0, 0, 0, 0, 0);
            if (!registered_ues[req.ue_id]) {
                registered_ues[req.ue_id] = 1;
                current_load++;
                printf("AMF%d: current load = %d (%.2f%%)\n", amf_index_global+1, current_load, (float)current_load/NUM_UE*100.0f);
            }
            Message paging = {0};
            paging.msgid = MSG_NGAP_RESP;
            paging.bitmask = BM_5G_STMSI;
            paging.ue_id = req.ue_id;
            paging.s_tmsi = s;
            pthread_t t;
            Message *mp = malloc(sizeof(Message));
            *mp = paging;
            pthread_create(&t, NULL, send_paging_thread, mp);
            pthread_detach(t);
        } else if (req.bitmask & BM_5G_STMSI) {
            // Check ownership
            int extracted_amf = (req.s_tmsi >> 30) & 0x3FF;
            if (extracted_amf != amf_index_global) { close(fd); return NULL; }
            Message resp = {0};
            resp.msgid = MSG_NGAP_RESP;
            resp.bitmask = BM_5G_STMSI;
            resp.ue_id = req.ue_id;
            sctp_sendmsg(fd, &resp, sizeof(resp), NULL, 0, 0, 0, 0, 0, 0);
            // Schedule next paging
            Message paging = {0};
            paging.msgid = MSG_NGAP_RESP;
            paging.bitmask = BM_5G_STMSI;
            paging.ue_id = req.ue_id;
            paging.s_tmsi = req.s_tmsi;
            pthread_t t;
            Message *mp = malloc(sizeof(Message));
            *mp = paging;
            pthread_create(&t, NULL, send_paging_thread, mp);
            pthread_detach(t);
        }
    } else if (req.msgid == 0xFF) {
        printf("AMF%d final: %d UEs (%.2f%%)\n", amf_index_global+1, current_load, (float)current_load/NUM_UE*100.0f);
    }
    close(fd);
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 3) { printf("Usage: %s <amf_index 1..5> <capacity>\n", argv[0]); return 1; }
    amf_index_global = atoi(argv[1]) - 1;
    capacity_global = atoi(argv[2]);
    srand(time(NULL) ^ amf_index_global);

    int srv = socket(AF_INET, SOCK_STREAM, IPPROTO_SCTP);
    if (srv < 0) { perror("AMF SCTP socket"); return 1; }
    int opt = 1; setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET; addr.sin_port = htons(AMF_BASE_PORT + amf_index_global); addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) < 0) { perror("AMF SCTP bind"); return 1; }
    if (listen(srv, 50) < 0) { perror("AMF SCTP listen"); return 1; }
    printf("AMF%d listening on %d (cap=%d)\n", amf_index_global+1, AMF_BASE_PORT + amf_index_global, capacity_global);

    while (1) {
        int *cfd = malloc(sizeof(int));
        *cfd = accept(srv, NULL, NULL);
        if (*cfd < 0) { free(cfd); continue; }
        pthread_t t; pthread_create(&t, NULL, handle_connection, cfd); pthread_detach(t);
    }
    close(srv);
    return 0;
}
