SOLARISHACK=-lsocket
all: msg_recv msg_send

msg_recv: msg_recv.c
	gcc ${SOLARISHACK} -lnsl msg_recv.c -o msg_recv -Wall

msg_send: msg_send.c
	gcc ${SOLARISHACK} -lnsl msg_send.c -o msg_send -Wall

clean:
	rm -f msg_recv *~ *.o
