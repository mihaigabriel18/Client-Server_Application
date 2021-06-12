CFLAGS = -Wall -Wextra -g

PORT = 8163

IP_SERVER = 127.0.0.1

all: server subscriber

server: server.cpp
	g++ server.cpp -o server ${CFLAGS}

subscriber: subscriber.cpp
	g++ subscriber.cpp -o subscriber ${CFLAGS}

.PHONY: clean run_server run_subscriber

run_server:
	./server ${PORT}

# Ruleaza clientul
run_subscriber:
	./subscriber C1 ${IP_SERVER} ${PORT}

run_subscriber2:
	./subscriber C2 ${IP_SERVER} ${PORT}

run_udp:
	python3 udp_client.py 127.0.0.1 ${PORT}

clean:
	rm -f server subscriber
