#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <ctype.h>
#include <hiredis/hiredis.h>

#define NUM_KEYS 1000000 // 요청 수 (입력 없으면 100만개)

#pragma pack(1)
struct myheader {
    uint32_t op; //
    uint64_t key;
    char value[128];
    uint64_t start_time;
    uint64_t latency;
    uint64_t num;
} __attribute__((packed));

char* get(redisContext *c, char* key) {
    redisReply *reply = redisCommand(c, "GET %s", key);
    char* value;

	if (reply->str == NULL) { value = strdup("null"); } // key 값 없으면 null 복사
	else value = strdup(reply->str);

	freeReplyObject(reply);
	return value;
}

int put(redisContext *c, char* key, char *value) {
    redisReply *reply = redisCommand(c, "SET %s %s", key, value);
    freeReplyObject(reply);

	return 0;
}

int RorW(redisContext* c, struct myheader* RecvBuffer, struct myheader* SendBuffer) {
	if (RecvBuffer->op == 0) { // 읽기(get) 
		char key_str[10];
		sprintf(key_str, "%ld", RecvBuffer->key);
		char* value = get(c, key_str); // value 값 읽어오기

        // SendBuffer -> RecvBuffer
		SendBuffer->op = 0;
		SendBuffer->key = RecvBuffer->key;
		strcpy(SendBuffer->value, value);
		SendBuffer->start_time = RecvBuffer->start_time;
		SendBuffer->latency = RecvBuffer->latency;
        SendBuffer->num = RecvBuffer->num;

		free(value);
	}

	else if (RecvBuffer->op == 1) { // 쓰기(put) 
		char key_str[10], value_str[512];
		sprintf(key_str, "%ld", RecvBuffer->key);
		sprintf(value_str, "%s", RecvBuffer->value);

		put(c, key_str, value_str); // value 값 적기(쓰기)

		// SendBuffer -> RecvBuffer
		SendBuffer->op = 1;
		SendBuffer->key = RecvBuffer->key;
        SendBuffer->start_time = RecvBuffer->start_time;
		SendBuffer->latency = RecvBuffer->latency;
        SendBuffer->num = RecvBuffer->num;
	}
	else {
		return -1;
	}

	return 0;
}

int main(int argc, char *argv[]) {
    // Connect to Redis server
    redisContext *redis_context = redisConnect("127.0.0.1", 6379);
    if (redis_context->err) {
        printf("Failed to connect to Redis: %s\n", redis_context->errstr);
        return 1;
    }
    if (argc < 2) {
        printf("Input : %s port number\n", argv[0]);
        return 1;
    }

    int SERVER_PORT = atoi(argv[1]);

    struct sockaddr_in srv_addr;
    memset(&srv_addr, 0, sizeof(srv_addr));
    srv_addr.sin_family = AF_INET;
    srv_addr.sin_port = htons(SERVER_PORT);
    srv_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    int sock;
    if ((sock = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        printf("Could not create listen socket\n");
        exit(1);
    }

    if ((bind(sock, (struct sockaddr *)&srv_addr, sizeof(srv_addr))) < 0) {
        printf("Could not bind socket\n");
        exit(1);
    }

    struct sockaddr_in cli_addr;
    int cli_addr_len = sizeof(cli_addr);

    int maxlen = 1024;
	int n = 0;

    struct myheader SendBuffer;
    struct myheader RecvBuffer;

    for (int i = 0; i < NUM_KEYS; i++) { // 초기 데이터 처리
        char key[16];
        sprintf(key, "%d", i);
        put(redis_context, key, "initial values");
    }

    while (1) {
		n = recvfrom(sock, &RecvBuffer, sizeof(RecvBuffer), 0, (struct sockaddr*)&cli_addr, &cli_addr_len);
		if (n > 0) {
			// 클라이언트 요청 처리
			if (RorW(redis_context, &RecvBuffer, &SendBuffer) == 0) { 
				sendto(sock, &SendBuffer, sizeof(SendBuffer), 0, (struct sockaddr*)&cli_addr, sizeof(cli_addr));
			}
		}
	}
    close(sock);

    redisFree(redis_context); // 메인 함수 마지막에 넣어준다. return 0; 하기 직전에.
    
    return 0;
}
