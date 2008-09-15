#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <netinet/in.h> /* INADDR_ANY, IPPROTO_*, IP_* declarations */
#include <arpa/inet.h> /* inet_aton */
#include <string.h> /* memset */
#include <unistd.h> /* gethostname(...) */
#include <netdb.h> /* MAXHOSTNAMELEN */
#include <pthread.h>

#define DEFAULT_PORT 12345
#define DEFAULT_ADDRESS "225.0.0.1"
#define MAX_DATA_SIZE 1024

#ifndef MAXHOSTNAMELEN
#define MAXHOSTNAMELEN 255
#endif


typedef struct thread_data_s {
   int test;
} thread_data_t;

void* msg_send(void *arg);
void* msg_recv(void *arg);

int main(int argc, char* argv[]) {
   const unsigned char ttl = 1;
   const int on = 1;
   int sockfd;
   struct sockaddr_in my_addr;
   struct sockaddr_in their_addr;
   char hostname[MAXHOSTNAMELEN];
   
   thread_data_t thread_data;
   thread_data.test = 1;

   pthread_t msg_send_tid, msg_recv_tid;
   
   /* return values for pthread */
   int msg_send_result, msg_recv_result;
   
   struct ip_mreq mreq;
   inet_aton(DEFAULT_ADDRESS, &mreq.imr_multiaddr);
   mreq.imr_interface.s_addr = INADDR_ANY;
   
   gethostname(hostname, MAXHOSTNAMELEN);
   
   char buffer[MAX_DATA_SIZE];
   char username[] = "MyName";

   memset(&my_addr, 0, sizeof(my_addr));
   my_addr.sin_family = AF_INET;
   my_addr.sin_addr.s_addr = htonl(INADDR_ANY);
   my_addr.sin_port = htons(DEFAULT_PORT);

   their_addr.sin_family = AF_INET;
   their_addr.sin_port = htons(DEFAULT_PORT);
   inet_aton(DEFAULT_ADDRESS, &their_addr.sin_addr);
   memset(their_addr.sin_zero, '\0', sizeof their_addr.sin_zero);

   sockfd = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);
   if(sockfd < 0) {
      perror("socket");
      exit(1);
   }
   if(setsockopt(sockfd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) < 0) {
      perror("setsockopt (ttl)");
      exit(1);
   }
   if(setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0 ) {
      perror("setsockopt (reuseaddr)");
      exit(1);
   }
   if(bind(sockfd, (struct sockaddr*) &my_addr, sizeof(my_addr)) < 0) {
      perror("bind");
      exit(1);
   }
   
   if(setsockopt(sockfd, IPPROTO_IP, IP_ADD_MEMBERSHIP, (void*)&mreq, sizeof(mreq)) < 0) {
      perror("setsockopt (addmemebership)");
      exit(1);
   }
   
   snprintf(buffer, MAX_DATA_SIZE, "<%s> %s", username, "Hello\0");
   if(sendto(sockfd, buffer, strlen(buffer), 0, (struct sockaddr *)&their_addr, sizeof
   their_addr) < 0) {
      perror("sentto");
      exit(1);
   }
   
   msg_send_result = pthread_create(&msg_send_tid, NULL, msg_send, (void*)&thread_data);
   if(msg_send_result != 0) {
      perror("pthread_create (send)");
      exit(1);
   }

   msg_recv_result = pthread_create(&msg_recv_tid, NULL, msg_recv, (void*)&thread_data);
   if(msg_recv_result != 0) {
      perror("pthread_create (recv)");
      exit(1);
   }
   
   pthread_join(msg_send_tid, NULL);
   pthread_join(msg_recv_tid, NULL);
   
   //pthread_detach(msg_send_tid);
   //pthread_detach(msg_recv_tid);
   
   while(1) {
      printf("Nothing....\n");
   }
   exit(0);
}

void* msg_send(void *arg) {
   thread_data_t* thread_data;
   thread_data = (thread_data_t*)arg;
   while(1) {
      if(thread_data->test == 1) {
      	 printf("SEND\n");
	 thread_data->test = 0;
      }
   }
}

void* msg_recv(void *arg) {
   thread_data_t* thread_data;
   thread_data = (thread_data_t*)arg;
   while(1) {
      if(thread_data->test == 0) {
      	 printf("RECV\n");
	 thread_data->test = 1;
      }
   }
}
