#include <stdio.h>
#include <iostream>
#include "rpc.h"

static const std::string kServerHostname = "localhost";
static const std::string kClientHostname = "localhost";

static constexpr uint16_t kUDPPort = 32001;
static constexpr uint16_t kUDPPortClient = 32002;
static constexpr uint8_t kReqType = 1;
static constexpr size_t kMsgSize = 980;
