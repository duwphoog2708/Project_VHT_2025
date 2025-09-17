#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <time.h>
#include <sys/select.h>
#include <errno.h>
#include <netinet/sctp.h>

#define NUM_UE 200
#define NUM_AMF 5
#define SHM_NAME "/5g_sim_shm"
#define SHM_SIZE (sizeof(SharedMemory))
#define GNB_LISTEN_PORT 9100   // gNB listen cho AMF

#define MSG_UE_RRC_CONNECTION_REQUEST 0x10
#define MSG_RRC_UE_CONNECTION_RESPONSE 0x11
#define MSG_RRC_NGAP_REQ              0x12
#define MSG_NGAP_RESP                 0x13
#define MSG_NGAP_RRC_PAGING           0x14
#define MSG_RRC_UE_PAGING             0x15

#define BM_RANDOM_VALUE 0x01
#define BM_5G_STMSI     0x02


enum UE_State{
    UE_IDLE,
    UE_REGISTERED,
    UE_CONNECTED
};
typedef struct {
    uint8_t msgid;
    uint8_t bitmask;
    uint16_t ue_id;
    uint64_t tmsi;
    uint64_t s_tmsi;
} Message;

typedef struct {
    pthread_mutex_t mutex;
    Message ul[NUM_UE];
    int ul_ready[NUM_UE];
    Message dl[NUM_UE];
    int dl_ready[NUM_UE];
    int ue_states[NUM_UE];
} SharedMemory;

typedef struct {
    int amf_id;
    int sock_fd;
} AmfConn;

SharedMemory *shm = NULL;
AmfConn amf_conns[NUM_AMF];

int ue_to_amf[NUM_UE];
int amf_counts[NUM_AMF];
int amf_capacity[NUM_AMF] = {40, 20, 30, 70, 40};
int amf_weight[NUM_AMF];
int amf_current_weight[NUM_AMF];

void init_shm() {
    int fd = shm_open(SHM_NAME, O_RDWR, 0666);
    if (fd < 0) { perror("gNB shm_open"); exit(1); }
    shm = mmap(NULL, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shm == MAP_FAILED) { perror("gNB mmap"); exit(1); }
    close(fd);
}

int pick_amf_wrr() {
    int total = 0;
    for (int i = 0; i < NUM_AMF; i++) total += amf_weight[i];
    int best_i = -1;
    int best_val = -2147483648;  // INT32_MIN
    for (int i = 0; i < NUM_AMF; i++) {
        amf_current_weight[i] += amf_weight[i];
        if (amf_current_weight[i] > best_val && amf_counts[i] < amf_capacity[i]) {
            best_val = amf_current_weight[i];
            best_i = i;
        }
    }
    if (best_i >= 0) {
        amf_current_weight[best_i] -= total;
    } else {
        for (int i = 0; i < NUM_AMF; i++) {
            if (amf_counts[i] < amf_capacity[i]) {
                best_i = i;
                break;
            }
        }
    }
    return best_i;
}

// =============== UPLINK THREAD ===============
void *uplink_thread(void *arg) {
    (void)arg;
    while (1) {
        for (int i = 0; i < NUM_UE; i++) {
            pthread_mutex_lock(&shm->mutex);
            if (!shm->ul_ready[i]) {
                pthread_mutex_unlock(&shm->mutex);
                continue;
            }
            Message m = shm->ul[i];
            shm->ul_ready[i] = 0;  // clear flag
            pthread_mutex_unlock(&shm->mutex);

            if (m.msgid != MSG_UE_RRC_CONNECTION_REQUEST) continue;

            // chọn AMF cho UE nếu chưa gán
            int amf = ue_to_amf[i];
            if (amf < 0) {
                amf = pick_amf_wrr();
                if (amf < 0) continue;
                ue_to_amf[i] = amf;
                amf_counts[amf]++;
            }

            // đóng gói NGAP gửi AMF
            Message ngap = {
                .msgid  = MSG_RRC_NGAP_REQ,
                .bitmask= m.bitmask,
                .ue_id  = i,
                .tmsi   = m.tmsi,
                .s_tmsi = m.s_tmsi
            };

            int fd = amf_conns[amf].sock_fd;
            if (fd <= 0) {
                amf_counts[amf]--;
                ue_to_amf[i] = -1;
                continue;
            }

            if (sctp_sendmsg(fd, &ngap, sizeof(ngap),
                             NULL, 0, 0, 0, 0, 0, 0) < 0) {
                perror("uplink send");
                amf_counts[amf]--;
                ue_to_amf[i] = -1;
                continue;
            }

            printf("gNB: Forwarded uplink req from UE%d to AMF%d\n", i, amf + 1);
        }
        usleep(1000);
    }
    return NULL;
}


// =============== DOWNLINK THREAD ===============
void *downlink_thread(void *arg) {
    (void)arg;
    fd_set readfds;
    while (1) {
        FD_ZERO(&readfds);
        int maxfd = -1;
        for (int i = 0; i < NUM_AMF; i++) {
            int fd = amf_conns[i].sock_fd;
            if (fd > 0) {
                FD_SET(fd, &readfds);
                if (fd > maxfd) maxfd = fd;
            }
        }
        if (maxfd < 0) { usleep(1000); continue; }

        if (select(maxfd + 1, &readfds, NULL, NULL, NULL) < 0) continue;

        for (int i = 0; i < NUM_AMF; i++) {
            int fd = amf_conns[i].sock_fd;
            if (fd > 0 && FD_ISSET(fd, &readfds)) {
                Message m;
                int r = sctp_recvmsg(fd, &m, sizeof(m), NULL, 0, NULL, NULL);
                if (r <= 0) {
                    if (r == 0) printf("gNB: AMF%d disconnected\n", i + 1);
                    close(fd);
                    amf_conns[i].sock_fd = -1;
                    continue;
                }
                if (m.msgid == MSG_NGAP_RESP || m.msgid == MSG_NGAP_RRC_PAGING) {
                    int uid = m.ue_id;
                    if (uid < 0 || uid >= NUM_UE) {
                        printf("gNB: Invalid UE ID %d from AMF%d, ignoring\n", uid, i + 1);
                        continue;
                    }
                    pthread_mutex_lock(&shm->mutex);
                    if (m.msgid == MSG_NGAP_RESP) {
                       shm->dl[uid].msgid =  MSG_RRC_UE_CONNECTION_RESPONSE;
                       //shm->ue_states[uid] = (m.bitmask & BM_5G_STMSI) ? 2 : 1; // CONNECTED or REGISTERED
                    } else if (m.msgid == MSG_NGAP_RRC_PAGING) {
                            shm->dl[uid].msgid = MSG_RRC_UE_PAGING; 
                       //     shm->ue_states[uid] = 1; // REGISTERED cho paging
                    }
                    shm->dl[uid].bitmask = m.bitmask;
                    shm->dl[uid].s_tmsi = m.s_tmsi & 0xFFFFFFFFFF;
                    shm->dl_ready[uid] = 1;
                    pthread_mutex_unlock(&shm->mutex);
                        // printf("gNB: Forwarded %s from AMF%d to UE%d\n",
                           //(shm->dl[uid].msgid == MSG_RRC_UE_CONNECTION_RESPONSE) ? "response" : "paging",
                             //    i + 1, uid);
               }
                // if (m.msgid == MSG_NGAP_RESP || m.msgid == MSG_NGAP_RRC_PAGING){
                //     int uid = m.ue_id;
                //     if (uid < 0 || uid >= NUM_UE) continue;

                //     pthread_mutex_lock(&shm->mutex);
                //     shm->dl[uid].msgid   = ;
                //     shm->dl[uid].bitmask = m.bitmask;
                //     shm->dl[uid].s_tmsi  = m.s_tmsi & 0xFFFFFFFFFF;
                //     shm->dl_ready[uid]   = 1;
                //     pthread_mutex_unlock(&shm->mutex);

                //     printf("gNB: Forwarded %s from AMF%d to UE%d\n",
                //            (m.bitmask & BM_5G_STMSI) ? "response" : "paging",
                //            i + 1, uid);
                // }
            }
        }
    }
    return NULL;
}


// =============== MAIN ===============
int main() {
    srand(time(NULL));
    init_shm();

    for (int i = 0; i < NUM_AMF; i++) {
        amf_weight[i] = amf_capacity[i];
        amf_current_weight[i] = 0;
        amf_counts[i] = 0;
        amf_conns[i].amf_id = i;
        amf_conns[i].sock_fd = -1;
    }
    for (int i = 0; i < NUM_UE; i++) ue_to_amf[i] = -1;

    // SCTP server
    int listen_fd = socket(AF_INET, SOCK_SEQPACKET, IPPROTO_SCTP);
    if (listen_fd < 0) { perror("gNB SCTP socket"); exit(1); }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(GNB_LISTEN_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("gNB SCTP bind"); exit(1);
    }
    if (listen(listen_fd, NUM_AMF) < 0) {
        perror("gNB SCTP listen"); exit(1);
    }
    printf("gNB: Listening for AMFs on port %d...\n", GNB_LISTEN_PORT);

    // Accept AMF connections
    // for (int i = 0; i < NUM_AMF; i++) {
    //     int conn_fd = accept(listen_fd, NULL, NULL);
    //     if (conn_fd < 0) { perror("gNB accept"); exit(1); }

    //     struct sctp_status status;
    //     socklen_t optlen = sizeof(status);
    //     if (getsockopt(conn_fd, IPPROTO_SCTP, SCTP_STATUS, &status, &optlen) < 0) {
    //         perror("getsockopt SCTP_STATUS"); close(conn_fd); continue;
    //     }

    //     int peel_fd = sctp_peeloff(listen_fd, status.sstat_assoc_id);
    //     if (peel_fd < 0) { perror("sctp_peeloff"); close(conn_fd); continue; }

    //     amf_conns[i].sock_fd = peel_fd;
    //     printf("gNB: AMF%d connected on socket %d\n", i+1, peel_fd);
    // }
    
     // Accept AMF connections (theo bất kỳ thứ tự nào)
    int connected_amf = 0;
    while (connected_amf < NUM_AMF) {
        int conn_fd = accept(listen_fd, NULL, NULL);
        if (conn_fd < 0) {
            perror("gNB accept");
            continue;
        }

        struct sctp_status status;
        socklen_t optlen = sizeof(status);
        if (getsockopt(conn_fd, IPPROTO_SCTP, SCTP_STATUS, &status, &optlen) < 0) {
            perror("getsockopt SCTP_STATUS");
            close(conn_fd);
            continue;
        }

        int peel_fd = sctp_peeloff(listen_fd, status.sstat_assoc_id);
        if (peel_fd < 0) {
            perror("sctp_peeloff");
            close(conn_fd);
            continue;
        }

        // Gán vào slot trống đầu tiên
        int slot = -1;
        for (int i = 0; i < NUM_AMF; i++) {
            if (amf_conns[i].sock_fd < 0) {
                slot = i;
                break;
            }
        }
        if (slot >= 0) {
            amf_conns[slot].sock_fd = peel_fd;
            printf("gNB: AMF%d connected on socket %d\n", slot + 1, peel_fd);
            connected_amf++;
        } else {
            printf("gNB: Too many AMFs connected, closing extra\n");
            close(peel_fd);
        }
    }

    // Create uplink + downlink threads
    pthread_t tid_ul, tid_dl;
    pthread_create(&tid_ul, NULL, uplink_thread, NULL);
    pthread_create(&tid_dl, NULL, downlink_thread, NULL);

    // Monitor loop
    while (1) {
        int connected = 0;
        pthread_mutex_lock(&shm->mutex);
        for (int i = 0; i < NUM_UE; i++) {
            if (shm->ue_states[i] == UE_CONNECTED) connected++;
            if (shm->ue_states[i] == UE_IDLE && ue_to_amf[i] >= 0) {
                int amf = ue_to_amf[i];
                amf_counts[amf]--;
                ue_to_amf[i] = -1;
            }
        }
        pthread_mutex_unlock(&shm->mutex);

        printf("gNB: Connected=%d\n", connected);
        for (int i = 0; i < NUM_AMF; i++) {
            printf("  AMF%d: %d/%d\n", i+1, amf_counts[i], amf_capacity[i]);
        }
        if (connected >= NUM_UE) {
            printf("gNB: All UEs connected, exiting\n");
            break;
        }
        sleep(1);
    }

    // Cleanup
    Message term = { .msgid = 0xFF };
    for (int i = 0; i < NUM_AMF; i++) {
        if (amf_conns[i].sock_fd > 0) {
            sctp_sendmsg(amf_conns[i].sock_fd, &term, sizeof(term), NULL, 0, 0, 0, 0, 0, 0);
            close(amf_conns[i].sock_fd);
        }
    }
    close(listen_fd);
    return 0;
}
