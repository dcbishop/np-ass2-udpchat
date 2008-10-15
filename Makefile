SOLARISHACK=-lsocket
all: msg

msg: msg.c
	gcc ${SOLARISHACK} -g -lcurses -lnsl -lpthread msg.c -o msg -Wall

clean:
	rm -f msg_recv msg_send msg *~ *.o core
