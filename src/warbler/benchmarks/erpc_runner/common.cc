#include "common.h"
#include <iostream>


size_t calculate_req_msg_size(void *req, DATA_TYPE req_type) {
    switch (req_type)
    {
    case TYPE_A:
    {
        struct RequestA *tagged_req = (struct RequestA *)req;
        return sizeof(RequestA) - MAXIMUM_DATA_SIZE + tagged_req->msg_len;
    }
    case TYPE_B:
    {
        struct RequestB *tagged_req = (struct RequestB *)req;
        return sizeof(RequestB) - MAXIMUM_DATA_SIZE + tagged_req->msg_len;
    } 
    default:
        break;
    }
}

size_t calculate_resp_msg_size(void *resp, DATA_TYPE req_type) {
    switch (req_type)
    {
    case TYPE_A:
    {
        struct ResponseA *tagged_resp = (struct ResponseA *)resp;
        return sizeof(ResponseA) - MAXIMUM_DATA_SIZE + tagged_resp->msg_len;
    }
    case TYPE_B:
    {
        struct ResponseB *tagged_resp = (struct ResponseB *)resp;
        return sizeof(ResponseB) - MAXIMUM_DATA_SIZE + tagged_resp->msg_len;
    }
    default:
        break;
    }
}

bool set_color(int color)
{
	bool ret = true;
	int fore = color%8;
	int back = (color/8)*8;
	switch (fore)
	{
	case F_BLACK:std::cout<<"\033[30m";break;
	case F_RED:std::cout<<"\033[31m";break;
	case F_GREEN:std::cout<<"\033[32m";break;
	case F_YELLOW:std::cout<<"\033[33m";break;
	case F_BLUE:std::cout<<"\033[34m";break;
	case F_PURPLE:std::cout<<"\033[35m";break;
	case F_WHITE:std::cout<<"\033[37m";break;
	default:ret = false;
	}
	switch (back)
	{
	case B_BLACK:std::cout<<"\033[40m";break;
	case B_RED:std::cout<<"\033[41m";break;
	case B_GREEN:std::cout<<"\033[42m";break;
	case B_BROWN:std::cout<<"\033[43m";break;
	case B_BLUE:std::cout<<"\033[44m";break;
	case B_WHITE:std::cout<<"\033[47m";break;
	default:ret = false;
	}
	return ret;
}

void reset_fcolor()
{std::cout<<"\033[39m";}
void reset_bcolor()
{std::cout<<"\033[49m";}
void reset_color()
{reset_bcolor(); reset_fcolor();}


uint count_bit(uint bitset) {
    uint cnt = 0;
    while (bitset) {
        cnt += bitset % 2;
        bitset /= 2;        
    }
    return cnt;
}