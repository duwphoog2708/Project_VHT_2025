#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <time.h>
#include <stdint.h>

#define NUM_UE 200
#define SHM_NAME "/5g_sim_shm"
#define SHM_SIZE (sizeof(SharedMemory))
#define AMF_TOTAL 5

#define MSG_UE_RRC_CONNECTION_REQUEST 0x10
#define MSG_RRC_UE_CONNECTION_RESPONSE 0x11
#define MSG_RRC_UE_PAGING             0x14
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

typedef struct {
    int idx;
    uint64_t tmsi;
    uint64_t s_tmsi;
    int x, z, x1, x2;
    int service_count;
    int state;
} UECtx;

static inline int rand_step500() {
    return 500 * (rand() % 6 + 1);
}

static void init_shm() {
    int fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    ftruncate(fd, SHM_SIZE);
    shm = mmap(NULL, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    memset(shm, 0, SHM_SIZE);
    pthread_mutexattr_t a; pthread_mutexattr_init(&a); pthread_mutexattr_setpshared(&a, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&shm->mutex, &a);
}

void send_ul_msg(int idx, Message *m) {
    pthread_mutex_lock(&shm->mutex);
    shm->ul[idx] = *m;
    shm->ul_ready[idx] = 1;
    pthread_mutex_unlock(&shm->mutex);
}

int poll_dl_msg(int idx, Message *out) {
    int got = 0;
    pthread_mutex_lock(&shm->mutex);
    if (shm->dl_ready[idx]) {
        *out = shm->dl[idx];
        shm->dl_ready[idx] = 0;
        got = 1;
    }
    pthread_mutex_unlock(&shm->mutex);
    return got;
}

void *ue_thread(void *arg) {
    UECtx *ue = (UECtx*)arg;
    srand(time(NULL) ^ (ue->tmsi & 0xFFFF)); // Unique seed per UE
    Message req, resp;
    while (ue->state != 2) {
        if (ue->state == 0) {
            if (ue->s_tmsi == 0) {
                req.msgid = MSG_UE_RRC_CONNECTION_REQUEST;
                req.bitmask = BM_RANDOM_VALUE;
                req.ue_id = ue->idx;
                req.tmsi = ue->tmsi;
                req.s_tmsi = 0;
                send_ul_msg(ue->idx, &req);
                while (!poll_dl_msg(ue->idx, &resp)) usleep(2000);
                if (resp.msgid == MSG_RRC_UE_CONNECTION_RESPONSE) {
                    ue->s_tmsi = resp.s_tmsi & 0xFFFFFFFFFF; // Mask to 40 bits
                    ue->state = 1;
                    shm->ue_states[ue->idx] = 1;
                    usleep(ue->x * 1000);
                    ue->state = 0;
                    shm->ue_states[ue->idx] = 0;
                }
            } else {
                if (poll_dl_msg(ue->idx, &resp)) {
	             if(resp.msgid == MSG_RRC_UE_PAGING){
		//	printf("UE%d: got Paging S-TMSI = 0x%llx, saved S-TMSI=0x%llx\n", ue->idx, (unsigned long long)(resp.s_tmsi & 0xFFFFFFFFFF), (unsigned long long)ue->s_tmsi);
		     }
                     if (resp.msgid == MSG_RRC_UE_PAGING && (resp.s_tmsi & 0xFFFFFFFFFF) == ue->s_tmsi) {
                        req.msgid = MSG_UE_RRC_CONNECTION_REQUEST;
                        req.bitmask = BM_5G_STMSI;
                        req.ue_id = ue->idx;
                        req.tmsi = ue->tmsi;
                        req.s_tmsi = ue->s_tmsi;
                        send_ul_msg(ue->idx, &req);
                        while (!poll_dl_msg(ue->idx, &resp)) usleep(2000);
                        if (resp.msgid == MSG_RRC_UE_CONNECTION_RESPONSE) {
			    ue->s_tmsi = resp.s_tmsi & 0xFFFFFFFFFF;
                            ue->service_count++;
                            if (ue->service_count == ue->x1) {
                                ue->s_tmsi = 0;
                                usleep(ue->z * 1000);
                            }
                   //         if (ue->service_count >= ue->x2) {
                              if(ue-> service_count >= ue->x2){
				  ue->state = 2;
                                shm->ue_states[ue->idx] = 2;
                                break;
                            }
                        }
                    }
                } else {
                    usleep(1000);
                }
            }
        }
        usleep(1000);
    }
    return NULL;
}

int main() {
    srand(time(NULL));
    init_shm();

    UECtx ues[NUM_UE];
    pthread_t th[NUM_UE];
    for (int i = 0; i < NUM_UE; i++) {
        ues[i].idx = i;
        ues[i].tmsi = 452040000000001ULL + i;
        ues[i].s_tmsi = 0;
        ues[i].x = rand_step500();
        ues[i].z = rand_step500();
        do {
            ues[i].x1 = rand() % 21;
            ues[i].x2 = rand() % 21;
        } while (ues[i].x2 <= ues[i].x1);
        ues[i].service_count = 0;
        ues[i].state = 0;
        shm->ul_ready[i] = 0;
        shm->dl_ready[i] = 0;
        shm->ue_states[i] = 0;
        pthread_create(&th[i], NULL, ue_thread, &ues[i]);
        usleep(2000);
    }

    for (int i = 0; i < NUM_UE; i++) pthread_join(th[i], NULL);

    pthread_mutex_destroy(&shm->mutex);
    munmap(shm, SHM_SIZE);
    shm_unlink(SHM_NAME);
    printf("UE process: all UE reached CONNECTED and exited.\n");
    return 0;
}
	
