#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <time.h>
#include <netinet/sctp.h>
#include <sys/time.h>

#define NUM_AMF 5
#define NUM_UE 200
#define GNB_PORT 9100
#define GNB_IP "127.0.0.1"

#define MSG_RRC_NGAP_REQ 0x12
#define MSG_NGAP_RESP    0x13
#define MSG_PAGING       0xFF
#define BM_RANDOM_VALUE  0x01
#define BM_5G_STMSI      0x02

typedef struct {
    uint8_t msgid;
    uint8_t bitmask;
    uint16_t ue_id;
    uint64_t tmsi;
    uint64_t s_tmsi;
} Message;

typedef struct {
    int amf_id;
    int capacity;
    int current_load;
    int sock_fd;
    uint16_t registered_ues[NUM_UE];
    uint64_t ue_s_tmsi[NUM_UE];
} AMF;

AMF amfs[NUM_AMF];
pthread_mutex_t send_mutex = PTHREAD_MUTEX_INITIALIZER;

void print_current_time() {
    struct timeval tv;
    struct tm* tm_info;
    gettimeofday(&tv, NULL);
    char buff[64];
    tm_info = localtime(&tv.tv_sec);
    strftime(buff, sizeof(buff), "%Y-%m-%d %H-%M-%S", tm_info);
    printf("[Time] %s:%06ld\n", buff, tv.tv_usec);
}

void *paging_thread(void *arg) {
    while (1) {
        usleep(5000 * 1000); // gửi paging mỗi 5 giây
        Message paging = {0};
        paging.msgid = MSG_PAGING;
        paging.bitmask = BM_5G_STMSI;
        paging.ue_id = 0xFFFF; // broadcast
        strcpy((char*) &paging.s_tmsi, "PAGING");

        pthread_mutex_lock(&send_mutex);
        for (int i = 0; i < NUM_AMF; i++) {
            if (amfs[i].sock_fd > 0) {
                int r = sctp_sendmsg(amfs[i].sock_fd, &paging, sizeof(paging),
                                     NULL, 0, 0, 0, 0, 0, 0);
                if (r > 0) {
                    printf("AMF%d: Sent Paging to gNB\n", i+1);
                }
            }
        }
        pthread_mutex_unlock(&send_mutex);
    }
    return NULL;
}

void *amf_thread(void *arg) {
    AMF *a = (AMF *)arg;

    // Kết nối đến gNB
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_SCTP);
    if (sock < 0) { perror("socket"); pthread_exit(NULL); }

    struct sockaddr_in gnb_addr = {0};
    gnb_addr.sin_family = AF_INET;
    gnb_addr.sin_port = htons(GNB_PORT);
    inet_pton(AF_INET, GNB_IP, &gnb_addr.sin_addr);

    if (connect(sock, (struct sockaddr*)&gnb_addr, sizeof(gnb_addr)) < 0) {
        perror("connect gNB");
        close(sock);
        pthread_exit(NULL);
    }

    a->sock_fd = sock;
    printf("AMF%d: Connected to gNB on socket %d (cap=%d)\n",
           a->amf_id+1, sock, a->capacity);

    // Vòng lặp xử lý bản tin
    while (1) {
        Message req;
        int r = sctp_recvmsg(sock, &req, sizeof(req), NULL, 0, NULL, NULL);
        if (r <= 0) {
            printf("AMF%d: gNB closed connection\n", a->amf_id+1);
            break;
        }

        if (req.msgid == MSG_RRC_NGAP_REQ) {
            if (req.bitmask & BM_RANDOM_VALUE && a->current_load < a->capacity) {
                uint64_t s = ((uint64_t)(a->amf_id & 0x3FF) << 30) |
                             ((uint64_t)(a->amf_id & 0x3F) << 24) |
                             (req.tmsi & 0xFFFFFF);

                Message resp = {0};
                resp.msgid = MSG_NGAP_RESP;
                resp.bitmask = BM_RANDOM_VALUE;
                resp.ue_id = req.ue_id;
                resp.tmsi = req.tmsi;
                resp.s_tmsi = s;

                a->ue_s_tmsi[req.ue_id] = s;
                sctp_sendmsg(sock, &resp, sizeof(resp), NULL, 0, 0, 0, 0, 0, 0);

                if (!a->registered_ues[req.ue_id]) {
                    a->registered_ues[req.ue_id] = 1;
                    a->current_load++;
                    print_current_time();
                    printf("AMF%d: current load = %d (%.2f%%)\n",
                           a->amf_id+1, a->current_load,
                           (float)a->current_load/NUM_UE*100.0f);
                }
            } else if (req.bitmask & BM_5G_STMSI) {
                Message resp = {0};
                resp.msgid = MSG_NGAP_RESP;
                resp.bitmask = BM_5G_STMSI;
                resp.ue_id = req.ue_id;
                if (a->ue_s_tmsi[req.ue_id] == 0) {
                    uint64_t s = ((uint64_t)(a->amf_id & 0x3FF) << 30) |
                                 ((uint64_t)(a->amf_id & 0x3F) << 24) |
                                 (req.tmsi & 0xFFFFFF);
                    a->ue_s_tmsi[req.ue_id] = s;
                }
                resp.s_tmsi = a->ue_s_tmsi[req.ue_id];
                sctp_sendmsg(sock, &resp, sizeof(resp), NULL, 0, 0, 0, 0, 0, 0);
            }
        } else if (req.msgid == MSG_PAGING) {
            printf("AMF%d final: %d UEs (%.2f%%)\n", a->amf_id+1,
                   a->current_load,
                   (float)a->current_load/NUM_UE*100.0f);
        }
    }

    close(sock);
    a->sock_fd = -1;
    pthread_exit(NULL);
}

int main(int argc, char **argv) {
    if (argc < 6) {
        printf("Usage: %s <cap1> <cap2> <cap3> <cap4> <cap5>\n", argv[0]);
        return 1;
    }

    pthread_t tids[NUM_AMF];
    for (int i = 0; i < NUM_AMF; i++) {
        amfs[i].amf_id = i;
        amfs[i].capacity = atoi(argv[i+1]);
        amfs[i].current_load = 0;
        memset(amfs[i].registered_ues, 0, sizeof(amfs[i].registered_ues));
        memset(amfs[i].ue_s_tmsi, 0, sizeof(amfs[i].ue_s_tmsi));
        if (pthread_create(&tids[i], NULL, amf_thread, &amfs[i]) != 0) {
            perror("pthread_create AMF");
            exit(1);
        }
    }

    // Thread gửi Paging định kỳ
    pthread_t tid_paging;
    pthread_create(&tid_paging, NULL, paging_thread, NULL);

    for (int i = 0; i < NUM_AMF; i++) pthread_join(tids[i], NULL);
    pthread_join(tid_paging, NULL);

    return 0;
}
