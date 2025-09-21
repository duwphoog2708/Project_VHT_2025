#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <time.h>
#include <stdint.h>
#include <sys/time.h>

#define NUM_UE 200
#define SHM_NAME "/5g_sim_shm"
#define SHM_SIZE (sizeof(SharedMemory))

#define MSG_UE_RRC_CONNECTION_REQUEST 0x10
#define MSG_RRC_UE_CONNECTION_RESPONSE 0x11
#define MSG_RRC_UE_PAGING             0x15
#define BM_RANDOM_VALUE 0x01
#define BM_5G_STMSI     0x02

enum UE_State {
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

SharedMemory *shm = NULL;

typedef struct {
    int idx;
    uint64_t tmsi;
    uint64_t s_tmsi;
    int x; // Registered -> Idle timer (ms)
    enum UE_State state;
    unsigned long long next_action_time;
    int uplink_ready;   // trigger attach uplink
} UECtx;

UECtx ue_list[NUM_UE];

unsigned long long current_millis() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000ULL + tv.tv_usec / 1000;
}

static inline int rand_step500() {
    return 500 * (rand() % 6 + 1);  // 500..3000 ms
}

static void init_shm() {
    int fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    ftruncate(fd, SHM_SIZE);
    shm = mmap(NULL, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    memset(shm, 0, SHM_SIZE);
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_setpshared(&a, PTHREAD_PROCESS_SHARED);
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

/* uplink thread: check uplink_ready cho toàn bộ UE */
void *uplink_thread(void *arg) {
    while (1) {
        for (int i = 0; i < NUM_UE; i++) {
            UECtx *ue = &ue_list[i];
            if (ue->state == UE_CONNECTED) continue;
            if (ue->state == UE_IDLE && ue->uplink_ready) {
                Message req;
                req.msgid  = MSG_UE_RRC_CONNECTION_REQUEST;
                req.ue_id  = ue->idx;
                req.tmsi   = ue->tmsi;

                if (ue->s_tmsi == 0) { // attach lần đầu
                    req.bitmask = BM_RANDOM_VALUE;
                    req.s_tmsi  = 0;
                } else { // re-attach sau Paging
                    req.bitmask = BM_5G_STMSI;
                    req.s_tmsi  = ue->s_tmsi;
                    printf("[UE %d] Sending re-attach with S-TMSI=0x%llx\n",
                           ue->idx, (unsigned long long)ue->s_tmsi);
                }
                send_ul_msg(ue->idx, &req);
                ue->uplink_ready = 0;
            }
        }
        usleep(1000);
    }
    return NULL;
}

/* downlink + timer thread: check DL và timer cho toàn bộ UE */
void *downlink_thread(void *arg) {
    Message resp;
    while (1) {
        unsigned long long now = current_millis();
        for (int i = 0; i < NUM_UE; i++) {
            UECtx *ue = &ue_list[i];
            if (ue->state == UE_CONNECTED) continue;

            // check DL message
            if (poll_dl_msg(ue->idx, &resp)) {
                if (resp.msgid == MSG_RRC_UE_CONNECTION_RESPONSE) {
                    if (ue->state == UE_IDLE && ue->s_tmsi == 0 &&
                        resp.bitmask == BM_RANDOM_VALUE) {
                        ue->s_tmsi = resp.s_tmsi & 0xFFFFFFFFFF;
                        ue->state = UE_REGISTERED;
  //      		pthread_mutex_lock(&shm->mutex);
                        shm->ue_states[ue->idx] = UE_REGISTERED;
    //    		pthread_mutex_unlock(&shm->mutex);
                        ue->next_action_time = now + ue->x;
                        printf("[UE %d] Registered (S-TMSI=0x%llx)\n",
                               ue->idx, (unsigned long long)ue->s_tmsi);
                    }
                    else if (ue->state == UE_IDLE && resp.bitmask == BM_5G_STMSI) {
                        ue->state = UE_CONNECTED;
//			pthread_mutex_lock(&shm->mutex);
                        shm->ue_states[ue->idx] = UE_CONNECTED;
//			pthread_mutex_unlock(&shm->mutex);
                        printf("[UE %d] Connected after Paging Response\n", ue->idx);
                    }
                }
                else if (resp.msgid == MSG_RRC_UE_PAGING) {
                    if ((resp.s_tmsi & 0xFFFFFFFFFF) == ue->s_tmsi) {
                        ue->uplink_ready = 1;
                        if (ue->state == UE_REGISTERED) {
                            ue->state = UE_IDLE;
 //			    pthread_mutex_lock(&shm->mutex);
                            shm->ue_states[ue->idx] = UE_IDLE;
//			    pthread_mutex_unlock(&shm->mutex);
                            ue->next_action_time = 0;
                            printf("[UE %d] Paging while REGISTERED -> force to IDLE\n", ue->idx);
                        }
                    }
                }
            }

            // check timer Registered->Idle
            if (ue->state == UE_REGISTERED &&
                ue->next_action_time > 0 &&
                now >= ue->next_action_time) {
                ue->state = UE_IDLE;
//		pthread_mutex_lock(&shm->mutex);
                shm->ue_states[ue->idx] = UE_IDLE;
//		pthread_mutex_unlock(&shm->mutex);
                ue->uplink_ready = 0;
                ue->next_action_time = 0;
                printf("[UE %d] Timer expired -> back to IDLE\n", ue->idx);
            }
        }
        usleep(1000);
    }
    return NULL;
}

int main() {
    srand(time(NULL));
    init_shm();

    for (int i = 0; i < NUM_UE; i++) {
        ue_list[i].idx = i;
        ue_list[i].tmsi = 452040000000001ULL + i;
        ue_list[i].s_tmsi = 0;
        ue_list[i].x = rand_step500();
        ue_list[i].state = UE_IDLE;
        ue_list[i].uplink_ready = 1; // trigger attach lần đầu
    }

    pthread_t tid_ul, tid_dl;
    pthread_create(&tid_ul, NULL, uplink_thread, NULL);
    pthread_create(&tid_dl, NULL, downlink_thread, NULL);

    pthread_join(tid_ul, NULL);
    pthread_join(tid_dl, NULL);
    munmap(shm, SHM_SIZE);
    return 0;
}
