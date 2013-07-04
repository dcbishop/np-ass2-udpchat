#SOLARISHACK=-lsocket
all: msg

msg: msg.c
	gcc msg.c -o msg -Wall ${SOLARISHACK} -g -lcurses -lnsl -lpthread

clean:
	rm -f msg_recv msg_send msg *~ *.o core
