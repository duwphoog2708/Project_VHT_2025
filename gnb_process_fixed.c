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
#include <netinet/sctp.h>

#define NUM_UE 200
#define NUM_AMF 5
#define SHM_NAME "/5g_sim_shm"
#define SHM_SIZE (sizeof(SharedMemory))
#define AMF_BASE_PORT 9101
#define GNB_PAGING_PORT 9200

#define MSG_UE_RRC_CONNECTION_REQUEST 0x10
#define MSG_RRC_UE_CONNECTION_RESPONSE 0x11
#define MSG_RRC_UE_PAGING             0x14
#define MSG_RRC_NGAP_REQ              0x12
#define MSG_NGAP_RESP                 0x13
#define BM_RANDOM_VALUE 0x01
#define BM_5G_STMSI     0x02

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

SharedMemory *shm = NULL;

int ue_to_amf[NUM_UE];
int amf_counts[NUM_AMF];
int amf_capacity[NUM_AMF] = {40, 20, 30, 70, 40};
int amf_current_weight[NUM_AMF];
int amf_weight[NUM_AMF];

void init_shm() {
    int fd = shm_open(SHM_NAME, O_RDWR, 0666);
    if (fd < 0) { perror("gNB shm_open"); exit(1); }
    shm = mmap(NULL, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
}

int pick_amf_wrr() {
    int total = 0;
    for (int i = 0; i < NUM_AMF; i++) total += amf_weight[i];
    int best_i = -1;
    int best_val = -2147483648;
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

void *paging_server(void *arg) {
    int srv = socket(AF_INET, SOCK_STREAM, IPPROTO_SCTP);
    if (srv < 0) { perror("gNB SCTP socket"); exit(1); }
    int opt = 1; setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET; addr.sin_port = htons(GNB_PAGING_PORT); addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) < 0) { perror("gNB SCTP bind"); exit(1); }
    if (listen(srv, 50) < 0) { perror("gNB SCTP listen"); exit(1); }
    while (1) {
        int c = accept(srv, NULL, NULL);
        if (c < 0) continue;
        Message m; int r = sctp_recvmsg(c, &m, sizeof(m), NULL, 0, NULL, NULL);
        close(c);
        if (r <= 0) continue;
        if (m.msgid == MSG_NGAP_RESP) {
            int uid = m.ue_id;
            pthread_mutex_lock(&shm->mutex);
            shm->dl[uid].msgid = MSG_RRC_UE_PAGING;
            shm->dl[uid].bitmask = BM_5G_STMSI;
            shm->dl[uid].s_tmsi = m.s_tmsi & 0xFFFFFFFFFF; // Mask to 40 bits
            shm->dl_ready[uid] = 1;
            pthread_mutex_unlock(&shm->mutex);
        }
    }
    return NULL;
}

int send_ngap_to_amf(int amf_idx, Message *req, Message *resp) {
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_SCTP);
    if (sock < 0) { perror("gNB SCTP socket"); return -1; }
    struct sockaddr_in serv = {0};
    serv.sin_family = AF_INET;
    serv.sin_port = htons(AMF_BASE_PORT + amf_idx);
    serv.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (connect(sock, (struct sockaddr*)&serv, sizeof(serv)) < 0) { close(sock); return -1; }
    if (sctp_sendmsg(sock, req, sizeof(*req), NULL, 0, 0, 0, 0, 0, 0) < 0) { close(sock); return -1; }
    int r = resp ? sctp_recvmsg(sock, resp, sizeof(*resp), NULL, 0, NULL, NULL) : 0;
    close(sock);
    return r > 0 || !resp ? 0 : -1;
}

int main() {
    srand(time(NULL));
    init_shm();
    for (int i = 0; i < NUM_AMF; i++) {
        amf_weight[i] = amf_capacity[i];
        amf_current_weight[i] = 0;
        amf_counts[i] = 0;
    }
    for (int i = 0; i < NUM_UE; i++) ue_to_amf[i] = -1;

    pthread_t t; pthread_create(&t, NULL, paging_server, NULL);

//    int handled_initial = 0;
    while (1) {
        pthread_mutex_lock(&shm->mutex);
        for (int i = 0; i < NUM_UE; i++) {
            if (shm->ue_states[i] == 0 && ue_to_amf[i] >= 0) {
                amf_counts[ue_to_amf[i]]--;
                ue_to_amf[i] = -1;
            }
        }
        pthread_mutex_unlock(&shm->mutex);

        for (int i = 0; i < NUM_UE; i++) {
            int do_process = 0;
            pthread_mutex_lock(&shm->mutex);
            if (shm->ul_ready[i]) { do_process = 1; }
            pthread_mutex_unlock(&shm->mutex);
            if (!do_process) continue;

            Message m;
            pthread_mutex_lock(&shm->mutex);
            m = shm->ul[i];
            shm->ul_ready[i] = 0;
            pthread_mutex_unlock(&shm->mutex);

            if (m.msgid == MSG_UE_RRC_CONNECTION_REQUEST) {
                if (m.bitmask & BM_RANDOM_VALUE) {
                    int amf = pick_amf_wrr();
                    if (amf < 0) continue;
                    ue_to_amf[i] = amf;
                    amf_counts[amf]++;
                    Message ngap = {0};
                    ngap.msgid = MSG_RRC_NGAP_REQ;
                    ngap.bitmask = BM_RANDOM_VALUE;
                    ngap.ue_id = i;
                    ngap.tmsi = m.tmsi;
                    Message ngap_resp = {0};
                    if (send_ngap_to_amf(amf, &ngap, &ngap_resp) == 0) {
                        pthread_mutex_lock(&shm->mutex);
                        shm->dl[i].msgid = MSG_RRC_UE_CONNECTION_RESPONSE;
                        shm->dl[i].bitmask = ngap_resp.bitmask;
                        shm->dl[i].s_tmsi = ngap_resp.s_tmsi & 0xFFFFFFFFFF; // Mask to 40 bits
                        shm->dl_ready[i] = 1;
                        shm->ue_states[i] = 1;
                        pthread_mutex_unlock(&shm->mutex);
               //         handled_initial++;
                    }
                } else if (m.bitmask & BM_5G_STMSI) {
                    int amf = ue_to_amf[i];
                    if (amf < 0) {
                        amf = pick_amf_wrr();
                        if (amf < 0) continue;
                        ue_to_amf[i] = amf;
                        amf_counts[amf]++;
                    }
                    Message ngap = {0};
                    ngap.msgid = MSG_RRC_NGAP_REQ;
                    ngap.bitmask = BM_5G_STMSI;
                    ngap.ue_id = i;
                    ngap.tmsi = m.tmsi;
                    ngap.s_tmsi = m.s_tmsi;
                    Message ngap_resp = {0};
                    send_ngap_to_amf(amf, &ngap, &ngap_resp);
		   // if(send_ngap_to_amf(amf, &ngap, &ngap_resp) == 0){
                    pthread_mutex_lock(&shm->mutex);
                    shm->dl[i].msgid = MSG_RRC_UE_CONNECTION_RESPONSE;
                    shm->dl_ready[i] = 1;
//		    shm->ue_states[i] = 2;
//                    shm->ue_states[i] = (shm->ue_states[i] == 2) ? 2 : 1;
                    pthread_mutex_unlock(&shm->mutex);

		   // printf("gNB: foward CONNECT_RESPONSE to UE %d s_tmsi = 0x%llX\n", i, (unsigned long long) (ngap_resp.s_tmsi & 0xFFFFFFFFFF));
		   // }
                }
            }
        }

        int connected = 0;
        pthread_mutex_lock(&shm->mutex);
        for (int i = 0; i < NUM_UE; i++) if (shm->ue_states[i] == 2) connected++;
//	printf("gNB: UE%d CONNECTED (total connected=%d)\n", uid, connected);
        pthread_mutex_unlock(&shm->mutex);
	printf("gNB: total UE CONNECTED=%d\n", connected);
        if ( connected == NUM_UE) {
            printf("gNB: all UE CONNECTED. distribution:\n");
  //          fflush(stdout);
  //          for (int k = 0; k < NUM_AMF; k++) {
//                printf("AMF%d: %d UEs (%.2f%%)\n", k+1, amf_counts[k], (float)amf_counts[k]/NUM_UE*100.0f);
//		fflush(stdout);
        }
            Message term = { .msgid = 0xFF };
            for (int i = 0; i < NUM_AMF; i++) send_ngap_to_amf(i, &term, NULL);
            break;
//        }
        usleep(2000);
    }

    return 0;
}
	
