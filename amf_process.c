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
#define MSG_NGAP_RRC_PAGING 0x14
#define MSG_INIT         0x09  

#define BM_RANDOM_VALUE  0x01
#define BM_5G_STMSI      0x02

typedef struct {
    uint8_t msgid;
    uint8_t bitmask;
    uint16_t ue_id;
    uint64_t tmsi;
    uint64_t s_tmsi;
} Message;

// khởi tạo init message để gán capacity cho gnb
typedef struct {
    uint8_t msgid;  
    int amf_id;
    int capacity;
} InitMessage;

typedef struct {
    int amf_id;
    int capacity;
    int current_load;
    int sock_fd;
    uint16_t registered_ues[NUM_UE];  // Lưu số lương UE registered
    uint64_t ue_s_tmsi[NUM_UE];       // lưu s-tmsi của UE
    unsigned long long ue_attach_time[NUM_UE]; // Lưu thời gian attach
    int ue_paging_delay[NUM_UE]; // Lưu y random cho mỗi UE
} AMF;

AMF amfs[NUM_AMF];
pthread_mutex_t send_mutex = PTHREAD_MUTEX_INITIALIZER;

// Hàm lấy thời gian thực
unsigned long long current_millis() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000ULL + tv.tv_usec / 1000;
}

static inline int rand_step500() {
    return 500 * (rand() % 6 + 1); // 500..3000 ms
}

// Hàm in time thực
void print_current_time() {
    struct timeval tv;
    struct tm* tm_info;
    gettimeofday(&tv, NULL);
    char buff[64];
    tm_info = localtime(&tv.tv_sec);
    strftime(buff, sizeof(buff), "%Y-%m-%d %H:%M:%S", tm_info);
    printf("[Time] %s:%06ld\n", buff, tv.tv_usec);
}

// Thread gửi paging theo y (ms) delay
void *paging_thread(void *arg) {
    srand(time(NULL));
    while (1) {
        unsigned long long now = current_millis();
        for (int i = 0; i < NUM_AMF; i++) {
            AMF *a = &amfs[i];
            for (int j = 0; j < NUM_UE; j++) {
                if (a->registered_ues[j] && a->ue_s_tmsi[j] && a->ue_attach_time[j] > 0) {
					
                    // Kiểm tra nếu đã đến thời điểm gửi paging (attach_time + y)
                    if (now >= a->ue_attach_time[j] + a->ue_paging_delay[j]) {
                        Message paging = {0};
                        paging.msgid = MSG_NGAP_RRC_PAGING; // 0x14
                        paging.bitmask = BM_5G_STMSI;
                        paging.ue_id = j;
                        paging.s_tmsi = a->ue_s_tmsi[j];

                        pthread_mutex_lock(&send_mutex);
                        if (a->sock_fd > 0) {
                            int r = sctp_sendmsg(a->sock_fd, &paging, sizeof(paging),
                                                 NULL, 0, 0, 0, 0, 0, 0);
                            if (r > 0) {
                                printf("AMF%d: Sent Paging for UE%d (S-TMSI=0x%llx, y=%dms)\n",
                                       i+1, j, (unsigned long long)a->ue_s_tmsi[j], a->ue_paging_delay[j]);
                            }
                            // Reset attach_time để tránh gửi lại paging
                            a->ue_attach_time[j] = 0;
                        }
                        pthread_mutex_unlock(&send_mutex);
                    }
                }
            }
        }
        usleep(1000); // Ngủ ngắn để giảm tải CPU
    }
    return NULL;
}

// Thread xử lý kết nối của mỗi AMF
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

  
    InitMessage init = { .msgid = MSG_INIT, .amf_id = a->amf_id, .capacity = a->capacity };
    if (sctp_sendmsg(sock, &init, sizeof(init), NULL, 0, 0, 0, 0, 0, 0) < 0) {
        perror("send init");
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
                a->ue_attach_time[req.ue_id] = current_millis(); // Lưu thời gian attach
                a->ue_paging_delay[req.ue_id] = rand_step500(); // Random y
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
                // if (a->ue_s_tmsi[req.ue_id] == 0) {
                //     uint64_t s = ((uint64_t)(a->amf_id & 0x3FF) << 30) |
                //                  ((uint64_t)(a->amf_id & 0x3F) << 24) |
                //                  (req.tmsi & 0xFFFFFF);
                //     a->ue_s_tmsi[req.ue_id] = s;
                // }
                resp.s_tmsi = a->ue_s_tmsi[req.ue_id];
                sctp_sendmsg(sock, &resp, sizeof(resp), NULL, 0, 0, 0, 0, 0, 0);
		printf("AMF%d: Service response for UE%d (S-TMSI=0x%llx, load unchanged)\n", a->amf_id+1, req.ue_id, (unsigned long long)resp.s_tmsi);
            }
        }
    }
     printf("AMF%d final: %d UEs (%.2f%%)\n", a->amf_id+1,a->current_load, (float)a->current_load/NUM_UE*100.0f);
    close(sock);
    a->sock_fd = -1;
    pthread_exit(NULL);
}

int main() {
    srand(time(NULL));
    pthread_t tids[NUM_AMF];
    int fixed_caps[NUM_AMF] = {40, 20, 30, 70, 40};  
    for (int i = 0; i < NUM_AMF; i++) {
        amfs[i].amf_id = i;
        amfs[i].capacity = fixed_caps[i];
        memset(amfs[i].registered_ues, 0, sizeof(amfs[i].registered_ues));
        memset(amfs[i].ue_s_tmsi, 0, sizeof(amfs[i].ue_s_tmsi));
        memset(amfs[i].ue_attach_time, 0, sizeof(amfs[i].ue_attach_time));
        memset(amfs[i].ue_paging_delay, 0, sizeof(amfs[i].ue_paging_delay));
        if (pthread_create(&tids[i], NULL, amf_thread, &amfs[i]) != 0) {
            perror("pthread_create AMF");
            exit(1);
        }
    }

    // Thread gửi Paging
    pthread_t tid_paging;
    pthread_create(&tid_paging, NULL, paging_thread, NULL);

    for (int i = 0; i < NUM_AMF; i++) pthread_join(tids[i], NULL);
    pthread_join(tid_paging, NULL);

    return 0;
}
