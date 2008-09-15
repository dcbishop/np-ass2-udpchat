#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h> /* sleep() */
#include <netinet/in.h> /* INADDR_ANY, IPPROTO_*, IP_* declarations */
#include <arpa/inet.h> /* inet_aton */
#include <string.h> /* memset */
#include <unistd.h> /* gethostname(...) */
#include <netdb.h> /* MAXHOSTNAMELEN (Solaris only)*/
#include <pthread.h>

#define DEFAULT_PORT 12345
#define DEFAULT_ADDRESS "225.0.0.1"
#define MAX_DATA_SIZE 1024

#ifndef MAXHOSTNAMELEN
#define MAXHOSTNAMELEN 255
#endif

typedef struct thread_data_s {
   int running;
   int sockfd;
   char* username;
   struct sockaddr_in* their_addr;
} thread_data_t;

void* msg_send(void *arg);
void* msg_recv(void *arg);

int main(int argc, char* argv[]) {
   const unsigned char ttl = 1;
   const int on = 1;
   int sockfd = -0xDEADC0DE;
   struct sockaddr_in my_addr;
   struct sockaddr_in their_addr;
   char hostname[MAXHOSTNAMELEN];
   
   thread_data_t thread_data;
   thread_data.running = 1;

   pthread_t msg_send_tid, msg_recv_tid;
   
   /* return values for pthread */
   int msg_send_result = -0xDEADC0DE;
   int msg_recv_result = -0xDEADC0DE;
   
   struct ip_mreq mreq;
   inet_aton(DEFAULT_ADDRESS, &mreq.imr_multiaddr);
   mreq.imr_interface.s_addr = INADDR_ANY;
   
   gethostname(hostname, MAXHOSTNAMELEN);
   
   //char buffer[MAX_DATA_SIZE];
   //strcpy("MyName", thread_data.username);
    
   memset(&my_addr, 0, sizeof(my_addr));
   my_addr.sin_family = AF_INET;
   my_addr.sin_addr.s_addr = htonl(INADDR_ANY);
   my_addr.sin_port = htons(DEFAULT_PORT);

   their_addr.sin_family = AF_INET;
   their_addr.sin_port = htons(DEFAULT_PORT);
   inet_aton(DEFAULT_ADDRESS, &their_addr.sin_addr);
   memset(their_addr.sin_zero, '\0', sizeof their_addr.sin_zero);
   thread_data.their_addr = &their_addr;

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
   
   thread_data.sockfd = sockfd;
   
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

   pthread_detach(msg_recv_tid);
   pthread_join(msg_send_tid, NULL);
   
   printf("Good for you! You reached the end of the program....\n");

   exit(0);
}

void* msg_send(void *arg) {
   thread_data_t* thread_data;
   thread_data = (thread_data_t*)arg;
   
   char buffer[MAX_DATA_SIZE];
   
   while(thread_data->running) {
      sleep(5);
    
      snprintf(buffer, MAX_DATA_SIZE, "<%s> %s", "USERNAME", "Hello\0");
      if(sendto(thread_data->sockfd, buffer, strlen(buffer), 0, (struct sockaddr *)&thread_data->their_addr, sizeof(thread_data->their_addr)) < 0) {
         perror("sentto");
         thread_data->running = 0;
         return (void*)EXIT_FAILURE;
      }
   }
   
   fprintf(stdout, "Send thread, self-termination...");
   return (void*)EXIT_SUCCESS;
}

void* msg_recv(void *arg) {
   thread_data_t* thread_data;
   thread_data = (thread_data_t*)arg;
   
   int bytes = -0xDEADC0DE;
   char buffer[MAX_DATA_SIZE];
  
   while(thread_data->running) {
      bytes = recvfrom(thread_data->sockfd, buffer, MAX_DATA_SIZE, 0, NULL, 0);
      if(bytes < 0) {
         perror("recvfrom");
         thread_data->running = 0;
         return (void*)EXIT_FAILURE;
      }
      
      buffer[bytes+1] = '\0';
      printf("%s\n", buffer);
   }
   
   fprintf(stdout, "Recv thread, self-termination...");
   return (void*)EXIT_SUCCESS;
}
