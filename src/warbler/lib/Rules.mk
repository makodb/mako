libd := $(dir $(lastword $(MAKEFILE_LIST)))

SRCS += $(addprefix $(libd), \
	lookup3.cc message.cc memory.cc helper_queue.cc transport.cc \
	fasttransport.cc configuration.cc timestamp.cc promise.cc client.cc shardClient.cc server.cc)

LIB-hash := $(libo)lookup3.o

LIB-message := $(libo)message.o $(LIB-hash)

LIB-hashtable := $(LIB-hash) $(LIB-message)

LIB-memory := $(libo)memory.o

LIB-configuration := $(libo)configuration.o $(LIB-message)

LIB-transport := $(libo)transport.o $(LIB-message) $(LIB-configuration)

LIB-fasttransport := $(libo)fasttransport.o $(LIB-transport)