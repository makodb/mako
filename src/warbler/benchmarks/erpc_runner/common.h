#pragma once

#include <cstdint>
#include <cstddef>

typedef uint8_t DATA_TYPE;
typedef uint32_t uint;


/* tune the maximum number of requests/responses to be cached here */
#define ERPC_QUEUE_SIZE 1000

#define REQ_NUM 2
#define TYPE_A 0
#define TYPE_B 1

#define SUCCESS 1
#define ERROR 0

#define TIMEOUT 100 // 100 sec

#define MAXIMUM_DATA_SIZE 100

struct TargetServerIDReader {
    DATA_TYPE targert_server_id;
};

struct RequestA {
    DATA_TYPE targert_server_id;
    uint req_nr;
    uint16_t msg_len;
    char data[MAXIMUM_DATA_SIZE];
};

struct ResponseA {
    uint req_nr;
    uint16_t msg_len;
    char data[MAXIMUM_DATA_SIZE];
};

struct RequestB {
    DATA_TYPE targert_server_id;
    uint req_nr;
    uint16_t msg_len;
    char data[MAXIMUM_DATA_SIZE];
};

struct ResponseB {
    uint req_nr;
    uint16_t msg_len;
    char data[MAXIMUM_DATA_SIZE];
};

size_t calculate_req_msg_size(void *req, DATA_TYPE req_type);

size_t calculate_resp_msg_size(void *resp, DATA_TYPE req_type);


/* color */

#define F_BLACK  0x01	// 000001
#define F_RED  0x02		// 000010
#define F_GREEN  0x03	// 000011
#define F_YELLOW  0x04	// 000100
#define F_BLUE  0x05		// 000101
#define F_PURPLE  0x06	// 000110
#define F_WHITE  0x07	// 000111

#define B_BLACK  0x08	// 001000
#define B_RED  0x10		// 010000
#define B_GREEN  0x18	// 011000
#define B_BROWN  0x80	// 100000
#define B_BLUE  0x88		// 101000
#define B_WHITE  0x90	// 110000

bool set_color(int color);
void reset_fcolor();
void reset_bcolor();
void reset_color();


/* utils */

uint count_bit(uint bitset);
