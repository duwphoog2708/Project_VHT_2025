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
#include <sys/time.h>

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
#define MSG_INIT                      0x09  // Mới: msgid cho init message

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

// Mới: Struct cho init message từ AMF
typedef struct {
    uint8_t msgid;  // 0x01
    int amf_id;
    int capacity;
} InitMessage;

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
int amf_capacity[NUM_AMF] = {0};  
int amf_weight[NUM_AMF] = {0};    
int amf_current_weight[NUM_AMF];

void init_shm() {
    int fd = shm_open(SHM_NAME, O_RDWR, 0666);
    if (fd < 0) { perror("gNB shm_open"); exit(1); }
    shm = mmap(NULL, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shm == MAP_FAILED) { perror("gNB mmap"); exit(1); }
    close(fd);
}

void print_current_time() {
    struct timeval tv;
    struct tm* tm_info;
    gettimeofday(&tv, NULL);
    char buff[64];
    tm_info = localtime(&tv.tv_sec);
    strftime(buff, sizeof(buff), "%Y-%m-%d %H:%M:%S", tm_info);
    printf("[Time] %s:%06ld\n", buff, tv.tv_usec);
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
		printf("gNB: Received from AMF%d, msgid=0x%x, ue_id=%d, bitmask=0x%x, s_tmsi=0x%llx\n",
               i + 1, m.msgid, m.ue_id, m.bitmask, (unsigned long long)(m.s_tmsi & 0xFFFFFFFFFF));
                if (m.msgid == MSG_NGAP_RESP || m.msgid == MSG_NGAP_RRC_PAGING) {
                    int uid = m.ue_id;
                    if (uid < 0 || uid >= NUM_UE) {
                        printf("gNB: Invalid UE ID %d from AMF%d, ignoring\n", uid, i + 1);
                        continue;
                    }
                    pthread_mutex_lock(&shm->mutex);
		    shm->dl[uid].msgid = (m.msgid == MSG_NGAP_RESP) ? MSG_RRC_UE_CONNECTION_RESPONSE : MSG_RRC_UE_PAGING;
                    shm->dl[uid].bitmask = m.bitmask;
                    shm->dl[uid].s_tmsi = m.s_tmsi & 0xFFFFFFFFFF;
                    shm->dl_ready[uid] = 1;
                    pthread_mutex_unlock(&shm->mutex);
                    printf("gNB: Forwarded %s from AMF%d to UE%d (S-TMSI=0x%llx)\n",
                            (m.msgid == MSG_NGAP_RESP) ? "response" : "paging", i + 1, uid, (unsigned long long)(m.s_tmsi & 0xFFFFFFFFFF));
               }
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
        amf_current_weight[i] = 0;
        amf_counts[i] = 0;
        amf_conns[i].amf_id = -1;  // Init -1
        amf_conns[i].sock_fd = -1;
    }
    for (int i = 0; i < NUM_UE; i++) ue_to_amf[i] = -1;

    // SCTP server
    int listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_SCTP);
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

    // Accept AMF connections with init message
    int connected_amf = 0;
    while (connected_amf < NUM_AMF) {
        int conn_fd = accept(listen_fd, NULL, NULL);
        if (conn_fd < 0) {
            perror("gNB accept");
            continue;
        }

        InitMessage init;
        int r = sctp_recvmsg(conn_fd, &init, sizeof(init), NULL, 0, NULL, NULL);
        if (r != sizeof(init) || init.msgid != MSG_INIT) {
            printf("gNB: Invalid init from AMF, closing\n");
            close(conn_fd);
            continue;
        }
        int aid = init.amf_id;
        if (aid < 0 || aid >= NUM_AMF || amf_conns[aid].sock_fd > 0) {
            printf("gNB: Invalid/duplicate AMF ID %d, closing\n", aid);
            close(conn_fd);
            continue;
        }
        amf_conns[aid].sock_fd = conn_fd;
        amf_conns[aid].amf_id = aid;
        amf_capacity[aid] = init.capacity;
        amf_weight[aid] = init.capacity;
        amf_current_weight[aid] = 0;
        amf_counts[aid] = 0;
        connected_amf++;
        printf("gNB: AMF%d (cap=%d) connected on socket %d\n", aid + 1, init.capacity, conn_fd);
    }

    // Create uplink + downlink threads
    pthread_t tid_ul, tid_dl;
    pthread_create(&tid_ul, NULL, uplink_thread, NULL);
    pthread_create(&tid_dl, NULL, downlink_thread, NULL);

    // Monitor loop
    while (1) {
        int connected = 0;
	int total_assign = 0;
        pthread_mutex_lock(&shm->mutex);
        for (int i = 0; i < NUM_UE; i++) {
            if (shm->ue_states[i] == UE_CONNECTED) connected++;
	    if(ue_to_amf[i] >= 0) total_assign++;
        }
        pthread_mutex_unlock(&shm->mutex);
        printf("gNB: Connected=%d, Registered=%d\n", connected, total_assign); 
        for (int i = 0; i < NUM_AMF; i++) {
            printf("  AMF%d: %d/%d\n", i+1, amf_counts[i], amf_capacity[i]);
        }
    
       if (total_assign >= NUM_UE && connected == NUM_UE) {  
            printf("gNB: All UEs connected, exiting\n");
            break;
        }
        sleep(1);
    }

    // Cleanup
    // Message term = { .msgid = 0xFF };
    // for (int i = 0; i < NUM_AMF; i++) {
    //     if (amf_conns[i].sock_fd > 0) {
    //         sctp_sendmsg(amf_conns[i].sock_fd, &term, sizeof(term), NULL, 0, 0, 0, 0, 0, 0);
    //         close(amf_conns[i].sock_fd);
    //     }
    // }
    close(listen_fd);
    return 0;
}
