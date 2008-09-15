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

#define DEFAULT_NAME "Sgt. Fury"
#define DEFAULT_PORT 12345
#define DEFAULT_ADDRESS "225.0.0.1"
#define MAX_DATA_SIZE 1024

#ifndef MAXHOSTNAMELEN
#define MAXHOSTNAMELEN 255
#endif

typedef struct thread_data_s {
   int running;
   int sockfd;
   char username[MAX_DATA_SIZE];
   char hostname[MAXHOSTNAMELEN];
   struct sockaddr_in* their_addr;
} thread_data_t;

int sendRaw(char* message, thread_data_t* thread_data);
int sendMessage(char* message, thread_data_t* thread_data);
void* msg_send(void *arg);
void* msg_recv(void *arg);

int main(int argc, char* argv[]) {
   const unsigned char ttl = 1;
   const int on = 1;
   int sockfd = 0xDEADC0DE;
   struct sockaddr_in my_addr;
   struct sockaddr_in their_addr;
   
   char address[MAX_DATA_SIZE] = {'\0'};
   int port = 0xDEADC0DE;
   
   thread_data_t thread_data;
   thread_data.running = 1;

   pthread_t msg_send_tid, msg_recv_tid;
   
   /* return values for pthread */
   int msg_send_result = 0xDEADC0DE;
   int msg_recv_result = 0xDEADC0DE;
   
   struct ip_mreq mreq;
   inet_aton(DEFAULT_ADDRESS, &mreq.imr_multiaddr);
   mreq.imr_interface.s_addr = INADDR_ANY;
   
   gethostname(thread_data.hostname, MAXHOSTNAMELEN);
   
   thread_data.username[0] = '\0';
   
   int name = 0xDEADC0DE;
   while ((name = getopt(argc, argv, "u:r")) != -1) {
      switch(name) {
         case 'u':
            strncpy(thread_data.username, optarg, MAX_DATA_SIZE);
            break;
         case 'm':
            strncpy(address, optarg, MAX_DATA_SIZE);
            break;
         case 'r':
            printf("%s: You have set the r flag which does nothing, thats just swell.\n", argv[0]);
            break;
         case 'p':
            port = atoi(optarg);
            break;
         case '?': 
            fprintf(stderr, "%s: Thats not right... try again...\n", argv[0]);
            exit(EXIT_FAILURE);
      }
   }
   
   if(thread_data.username[0] == '\0') {
      strncpy(thread_data.username, DEFAULT_NAME, MAX_DATA_SIZE);
      printf("%s: Due to your failure to specify a name, you will hence forth be known as '%s'.\n", argv[0], thread_data.username);
   }
   
   if(address[0] == '\0') {
      strncpy(address, DEFAULT_ADDRESS, MAX_DATA_SIZE);
      printf("%s: %s, because of your apparent inability to specify a multicast address, %s has been assigned for you.\n", argv[0], thread_data.username, address);
   }
   
   if(port < 0) {
      port = 12345;
      printf("%s: %s, as you have not chosen a port number %d has been set.\n", argv[0], thread_data.username, port);
   }
   
   memset(&my_addr, 0, sizeof(my_addr));
   my_addr.sin_family = AF_INET;
   my_addr.sin_addr.s_addr = htonl(INADDR_ANY);
   my_addr.sin_port = htons(DEFAULT_PORT);

   their_addr.sin_family = AF_INET;
   their_addr.sin_port = htons(DEFAULT_PORT);
   inet_aton(address, &their_addr.sin_addr);
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
   close(sockfd);
   
   printf("Good for you! You reached the end of the program....\n");
   
   exit(0);
}

int sendRaw(char* message, thread_data_t* thread_data) {
   if(sendto(thread_data->sockfd, message, strlen(message), 0, (struct sockaddr *)thread_data->their_addr, sizeof(*thread_data->their_addr)) < 0) {
      perror("sentto");
      thread_data->running = 0;
      return EXIT_FAILURE;
   }
   return EXIT_SUCCESS;
}

int sendMessage(char* message, thread_data_t* thread_data) {
   char buffer[MAX_DATA_SIZE];
   snprintf(buffer, MAX_DATA_SIZE-1, "<%s> %s", thread_data->username, message);
   if(!sendRaw(buffer, thread_data)) {
      return EXIT_FAILURE;
   }  

   return EXIT_SUCCESS;
}

void* msg_send(void *arg) {
   thread_data_t* thread_data;
   thread_data = (thread_data_t*)arg;

   char buffer[MAX_DATA_SIZE];
   snprintf(buffer, MAX_DATA_SIZE-1, "<%s@%s entered chat group>\n", thread_data->username, thread_data->hostname);   
   sendRaw(buffer, thread_data);
   
   while(thread_data->running) {
      sleep(5);
      sendMessage("Be amazed! For I am sending continious messages at intervals of exactly 5 seconds!\n", thread_data);
   }
   
   snprintf(buffer, MAX_DATA_SIZE-1, "<%s@%s leaving chat group>\n", thread_data->username, thread_data->hostname);   
   sendRaw(buffer, thread_data);
   
   fprintf(stdout, "Send thread, self-termination...\n");
   return (void*)EXIT_SUCCESS;
}

void* msg_recv(void *arg) {
   thread_data_t* thread_data;
   thread_data = (thread_data_t*)arg;
   
   int bytes = 0xDEADC0DE;
   char buffer[MAX_DATA_SIZE];
  
   while(thread_data->running) {
      bytes = recvfrom(thread_data->sockfd, buffer, MAX_DATA_SIZE-1, 0, NULL, 0);
      if(bytes < 0) {
         perror("recvfrom");
         thread_data->running = 0;
         return (void*)EXIT_FAILURE;
      }
      
      if(buffer[bytes-1] != '\n') {
         buffer[bytes-1] = '\n';
      }
      buffer[bytes] = '\0';

      printf("%s", buffer);
   }
   
   fprintf(stdout, "Recv thread, self-termination...\n");
   return (void*)EXIT_SUCCESS;
}
