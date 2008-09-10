all: msg_recv

msg_recv: msg_recv.c
	gcc -lnsl msg_recv.c -o msg_recv -Wall

clean:
	rm -f msg_recv *~ *.o
