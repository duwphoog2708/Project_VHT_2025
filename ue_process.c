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
#define AMF_TOTAL 5

#define MSG_UE_RRC_CONNECTION_REQUEST 0x10
#define MSG_RRC_UE_CONNECTION_RESPONSE 0x11
#define MSG_RRC_UE_PAGING             0x14
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

SharedMemory *shm = NULL;

typedef struct {
    int idx;
    uint64_t tmsi;
    uint64_t s_tmsi;
    int x; // z, x1, x2;
  //  int service_count;
    enum UE_State state;
    unsigned long long next_action_time;
} UECtx;

UECtx ue_list[NUM_UE];

static inline int rand_step500() {
    return 500 * (rand() % 6 + 1);
}

unsigned long long current_millis(){
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return tv.tv_sec * 1000ULL + tv_tv_usec / 1000;
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

// send_thread – gửi uplink(UE->gNB)
void *send_thread(void *arg) {
    UECtx *ue = (UECtx*)arg;
    Message req;

    while (ue->state != UE_CONNECTED) {
        if (ue->state == UE_IDLE && ue->uplink_ready) {
            if (ue->s_tmsi == 0) {
                // Attach lần đầu
                req.msgid  = MSG_UE_RRC_CONNECTION_REQUEST;
                req.bitmask = BM_RANDOM_VALUE;
                req.ue_id  = ue->idx;
                req.tmsi   = ue->tmsi;
                req.s_tmsi = 0;
                send_ul_msg(ue->idx, &req);
                ue->uplink_ready = 0;
            } else {
                // Re-attach theo Paging (sử dụng S-TMSI)
                req.msgid  = MSG_UE_RRC_CONNECTION_REQUEST;
                req.bitmask = BM_5G_STMSI;
                req.ue_id  = ue->idx;
                req.tmsi   = ue->tmsi;
                req.s_tmsi = ue->s_tmsi;
                send_ul_msg(ue->idx, &req);
                ue->uplink_ready = 0;
            }
        }
        usleep(1000);
    }
    return NULL;
}

// recv_thread – nhận downlink (gNB->UE)
void *recv_thread(void *arg) {
    UECtx *ue = (UECtx*)arg;
    Message resp;
    unsigned long long now = current_millis();
    while (ue->state != UE_CONNECTED) {
        if (poll_dl_msg(ue->idx, &resp)) {
            if (resp.msgid == MSG_RRC_UE_CONNECTION_RESPONSE) {
                //ue->s_tmsi = resp.s_tmsi & 0xFFFFFFFFFF;

                if (ue->state == UE_IDLE && ue->s_tmsi != 0) {
                    // Lần đầu attach → sang REGISTERED
					ue->s_tmsi = resp.s_tmsi & 0xFFFFFFFFFF; // UE lưu S-TMSI
                    ue->state = UE_REGISTERED;
                    shm->ue_states[ue->idx] = UE_REGISTERED;

		    ue->next_action_time = now + ue->x; // setup timer
                    printf("[UE %d] Registered with S-TMSI=0x%llx\n",
                           ue->idx, (unsigned long long)ue->s_tmsi);

                    // Sau x ms → quay lại IDLE
                   if(ue->state = UE_REGISTERED && ue->next_action_time > 0 && now >= ue->next_action_time){

                    ue->state = UE_IDLE;
                    shm->ue_states[ue->idx] = UE_IDLE;
                    ue->uplink_ready = 0; // đợi paging
		    ue->next_action_time = 0;
		   }
                }
                else if (ue->state == UE_IDLE && ue->s_tmsi == resp.s_tmsi & 0xFFFFFFFFFFF) {
                    // Response sau khi Paging → sang CONNECTED
                    ue->state = UE_CONNECTED;
                    shm->ue_states[ue->idx] = UE_CONNECTED;

                    printf("[UE %d] Connected after Paging Response\n", ue->idx);
                }
            }
            else if (resp.msgid == MSG_RRC_UE_PAGING) {
                if ((resp.s_tmsi & 0xFFFFFFFFFF) == ue->s_tmsi) {
                    printf("[UE %d] Got PAGING -> uplink again\n", ue->idx);
                    ue->uplink_ready = 1; // bật cờ gửi uplink
                }
            }
        } 
		usleep(1000);
		//else {
          //  usleep(1000);
        //}
			
    }
    return NULL;
}

int main() {
    srand(time(NULL));
    init_shm();
    
    for(int i=0;i<NUM_UE;i++){
	ue_list[i].idx = i;
	ue_list[i].tmsi = 452040000000001ULL + i;
	ue_list[i].s_tmsi =  0;
	ue_list[i].x = rand_step500();
//	ue_list[i].service_count = 0;
	ue_list[i].state =  UE_IDLE;
	ue_list[i].next_action_time = 0;
    }
    pthread_t tid_send, tid_recv;
    pthread_create(&tid_send, NULL, uplink_thread, &ue_list[i]);
    pthread_create(&tid_recv, NULL, downlink_thread, &ue_list[i]);

    pthread_join(tid_send, NULL);
    pthread_join(tid_recv, NULL);
    return 0;
}
	


