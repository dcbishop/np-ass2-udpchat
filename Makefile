#SOLARISHACK=-lsocket
all: msg

msg: msg.c
	gcc ${SOLARISHACK} -lncurses -lnsl -pthread msg.c -o msg -Wall

clean:
	rm -f msg_recv msg_send msg *~ *.o
