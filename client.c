#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>
#include <pthread.h>
#include <math.h>
#include <gsl/gsl_rng.h>
#include <gsl/gsl_randist.h>

#pragma pack(1)
struct myheader {
    uint32_t op; //
    uint64_t key;
    char value[128];
    uint64_t start_time;
    uint64_t latency;
    uint64_t num;
} __attribute__((packed));

struct sockArg {
    int sock;
    struct sockaddr_in srv_addr, cli_addr;
    int cli_addr_len;
};

// compare 함수 정의
int compare(const void *a, const void *b) {
    return (*(uint64_t*)a - *(uint64_t*)b);
}

uint64_t get_cur_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t t = ts.tv_sec * 1000 * 1000 * 1000 + ts.tv_nsec;
    return t;
}
// 전역 변수
int TARGET_QPS, s_time, WRatio; // 입력 받은 수 저장할 변수들
int total;

// Tx 스레드
void *tx_thread(void *arg) {
    // 함수의 인자를 sockArg 구조체로 캐스팅
    struct sockArg* args = (struct sockArg*)arg;
    int sock = args->sock;
    struct sockaddr_in srv_addr = args->srv_addr;

    // GSL 난수 생성기 및 exponential 분포 설정
    const gsl_rng_type * T;
    gsl_rng * r;
    gsl_rng_env_setup();
    T = gsl_rng_default;
    r = gsl_rng_alloc(T);

    double lambda = TARGET_QPS * 1e-9; // TARGET_QPS가 인자로 받은 target tx rate임
    double mu = 1.0 / lambda;
    uint64_t temp_time = get_cur_ns();

    for(int i = 0; i < TARGET_QPS; i++) {
        /*Packet inter-arrival time 을 Exponentional하게 보내기 위한 연산 과정들이다 */
        uint64_t inter_arrival_time = (uint64_t)(gsl_ran_exponential(r, mu));
        temp_time += inter_arrival_time;

        // Inter-inter_arrival_time만큼 시간이 지나지 않았다면 무한루프를 돌며 대기한다.
        while (get_cur_ns() < temp_time)
            ;

        // 요청을 위한 구조체 생성 및 전송
        struct myheader SendBuffer;
        memset(&SendBuffer, 0, sizeof(SendBuffer)); // 초기화
        SendBuffer.op = (rand() % 100 < WRatio) ? 1 : 0;
        SendBuffer.key = rand() % 1000000;
        SendBuffer.start_time = get_cur_ns(); 
        SendBuffer.latency = 0;
        SendBuffer.num = i+1;

        // printf("Tx #%ld\n", SendBuffer.num); // 확인을 위해 찍어본다
        sendto(sock, &SendBuffer, sizeof(SendBuffer), 0, (struct sockaddr *)&srv_addr, sizeof(srv_addr));
    }
    return NULL;
}
// Rx 스레드
void *rx_thread(void *arg) {
    struct sockArg* args = (struct sockArg*)arg;
    int sock = args->sock;
    struct sockaddr_in srv_addr = args->srv_addr;
    struct sockaddr_in cli_addr = args->cli_addr;
    int cli_addr_len = args->cli_addr_len;

    struct myheader RecvBuffer;
    uint64_t latencies[total];
    memset(latencies, 0, sizeof(uint64_t) * total);

    uint64_t sum = 0;
    uint64_t median = 0; // 50%
    uint64_t for99 = 0; // 99%

    int rxReqs = 0; // 수신된 요청 수

    uint64_t start = get_cur_ns(); // 스레드 시작 시간
    uint64_t elapsed = 0;  // 스레드 경과 시간

    // 최대 대기 시간 (초)
    int max_wait_sec = 10;
    uint64_t max_wait_ns = max_wait_sec * 1000000000ULL; // 초를 나노초로 변환

    while(elapsed < max_wait_ns && rxReqs < total) {
        ssize_t rx = recvfrom(sock, &RecvBuffer, sizeof(RecvBuffer), MSG_DONTWAIT, (struct sockaddr *)&srv_addr, &cli_addr_len);
        if (rx < 0) {
            elapsed = get_cur_ns() - start;
            continue;
        }

        // 패킷을 수신한 경우
        start = get_cur_ns(); // 스레드 시작 시간 초기화
        RecvBuffer.latency = start - RecvBuffer.start_time;

        latencies[rxReqs] = RecvBuffer.latency;
        sum += RecvBuffer.latency;
        rxReqs++;
    }

    // 총 요청 수
    printf("총 요청 수: %d\n", rxReqs);

    // 총 요청 수만큼 수신해야 latency 기록
    if (rxReqs == total) {
        // 50%
        qsort(latencies, rxReqs, sizeof(uint64_t), compare);
        if (rxReqs % 2 != 0) {
            median = (double)latencies[rxReqs / 2];
        }
        else {
            uint64_t median1 = latencies[rxReqs / 2 - 1];
            uint64_t median2 = latencies[rxReqs / 2];
            median = (double)((median1 + median2) / 2);
        }
        printf("Median latency: %.2lf ns\n", (double)median);

        // 99%
        qsort(latencies, rxReqs, sizeof(uint64_t), compare);
        int index_99 = (int)(0.99 * rxReqs) - 1;
        for99 = (double)latencies[index_99];
        printf("99th percentile latency: %.2lf ns\n", (double)for99);
    } 
    else {
        printf("Timeout or incomplete request\n");
    }

    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc < 4) {
        printf("Input: %s [Target Tx rate] [sending time] [Write ratio]\n", argv[0]);
        return 1;
    }
    int enteredQPS = atoi(argv[1]);
    s_time = atoi(argv[2]);
    WRatio = atoi(argv[3]);
    total = TARGET_QPS * s_time;

    int SERVER_PORT = 5001;
    const char *server_name = "localhost";
    struct sockaddr_in srv_addr;
    memset(&srv_addr, 0, sizeof(srv_addr));
    srv_addr.sin_family = AF_INET;
    srv_addr.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, server_name, &srv_addr.sin_addr);

    // 소켓 생성 확인
    int sock;
    if ((sock = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        printf("Could not create socket\n");
        exit(1);
    }

    // 서버 연결 확인
    int connect_status = connect(sock, (struct sockaddr*)&srv_addr, sizeof(srv_addr));
    if (connect_status < 0) {
        printf("Failed to connect to the server\n");
        exit(1);
    }

    struct sockaddr_in cli_addr;
    int cli_addr_len = sizeof(cli_addr);

    pthread_t tx_tid, rx_tid;

    struct sockArg forTx = { .sock = sock, .srv_addr = srv_addr };
    struct sockArg forRx = { .sock = sock, .srv_addr = srv_addr, .cli_addr = cli_addr, .cli_addr_len = cli_addr_len };


    for (int qps = enteredQPS; qps <= 10000000; qps += 50) {
        close(sock);
        if ((sock = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
            printf("Could not create socket\n");
            exit(1);
        }

        TARGET_QPS = qps;
        total = TARGET_QPS * s_time;

        for (int j = 0; j < s_time; j++)
        {
            pthread_create(&tx_tid, NULL, tx_thread, (void*)&forTx);

        }
        pthread_create(&rx_tid, NULL, rx_thread, (void*)&forRx);

        printf("Tx QPS: %d\n", TARGET_QPS);

        pthread_join(tx_tid, NULL);
        pthread_join(rx_tid, NULL);
    }

    close(sock);
    return 0;
}