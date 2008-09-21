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
#include <ncurses.h> /* For the prettyness */
#include <curses.h>
#include <signal.h>

#define DEFAULT_NAME "Sgt. Fury"
#define DEFAULT_PORT 12345
#define DEFAULT_ADDRESS "225.0.0.1"
#define MAX_DATA_SIZE 1024

#define QUE_SEND 0
#define QUE_RECEIVE 1

#ifndef MAXHOSTNAMELEN
#define MAXHOSTNAMELEN 255
#endif

typedef struct message_node_s {
   char* message;
   struct message_node_s* next;
   struct message_node_s* prev;
} message_node_t;

typedef struct thread_data_s {
   int running;
   int sockfd;
   char username[MAX_DATA_SIZE];
   char hostname[MAXHOSTNAMELEN];
   struct sockaddr_in* their_addr;
   struct message_node_s* message_que[2];
   int message_count[2];
} thread_data_t;
static thread_data_t thread_data;

message_node_t* add_message_actual(message_node_t** head_node, char* message);
void message_dump(message_node_t* node);
message_node_t* add_message(thread_data_t* thread_data, int que, char* message);
int sendRaw(char* message, thread_data_t* thread_data);
int sendMessage(char* message, thread_data_t* thread_data);
void* msg_send(void *arg);
void* msg_recv(void *arg);
void* prwdy(void *arg);
void draw_prwdy(thread_data_t* thread_data, WINDOW* win);
static void seppuku(int sig);

/* Adds a message to the message que, (Doesn't increase counter) */
message_node_t* add_message_actual(message_node_t** head_node, char* message) {
   message_node_t* mn = *head_node;
   message_node_t* prev = NULL;
   size_t len;
   
   len = strlen(message);
   if(len<1) {
      return NULL;
   }
   
   if(!mn) {
      *head_node = malloc(sizeof(message_node_t));
      mn = *head_node;
   } else {
      while(mn->next) {
         mn = mn->next;
      }
      mn->next = malloc(sizeof(message_node_t));
      prev = mn;
      mn=mn->next;
   }
   
   if(!mn) {
      return NULL;
   }
   
   mn->next = NULL;
   mn->prev = prev;
   mn->message = malloc(len * sizeof(char)+1);
   strncpy(mn->message, message, len);
   mn->message[len] = '\0';
   
   return mn;
}

/* Dumps a list of messages for debugging */
void message_dump(message_node_t* node) {
   while(node) {
      printf("DEBUG MESSAGE: %s\n", node->message);
      node = node->next;
   }
   printf("yay!\n");
}

/* Adds a message to the specified que */
message_node_t* add_message(thread_data_t* thread_data, int que, char* message) {
   message_node_t* newmessage;
   newmessage = add_message_actual(&thread_data->message_que[que], message);
   if(!newmessage) {
      return NULL;
   }
   
   thread_data->message_count[que]++;
   return newmessage;
}

int main(int argc, char* argv[]) {
   const unsigned char ttl = 1;
   const int on = 1;
   int sockfd = 0xDEADC0DE;
   struct sockaddr_in my_addr;
   struct sockaddr_in their_addr;
   
   char address[MAX_DATA_SIZE] = {'\0'};
   int port = 0xDEADC0DE;
   
   thread_data.running = 1;

   pthread_t msg_send_tid = 0xDEADC0DE;
   pthread_t msg_recv_tid = 0xDEADC0DE;
   pthread_t prwdy_tid = 0xDEADC0DE;
   
   /* return values for pthread */
   int msg_send_result = 0xDEADC0DE;
   int msg_recv_result = 0xDEADC0DE;
   int prwdy_result = 0xDEADC0DE;
   
   struct ip_mreq mreq;
   inet_aton(DEFAULT_ADDRESS, &mreq.imr_multiaddr);
   mreq.imr_interface.s_addr = INADDR_ANY;
   
   gethostname(thread_data.hostname, MAXHOSTNAMELEN);
   
   thread_data.username[0] = '\0';
   
   add_message(&thread_data, QUE_SEND, "Hello");
   add_message(&thread_data, QUE_SEND, "Another message");
   add_message(&thread_data, QUE_SEND, "Yet another message");
   
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
   
   (void) signal(SIGINT, seppuku);
   
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
   
   prwdy_result = pthread_create(&prwdy_tid, NULL, prwdy, (void*)&thread_data);
   if(prwdy_result != 0) {
      perror("pthread_create (prwdy)");
      exit(1);
   }

   pthread_detach(msg_recv_tid);
   pthread_join(prwdy_tid, NULL);
   pthread_join(msg_send_tid, NULL);
   
   close(sockfd);
   
   printf("Good for you! You reached the end of the program....\n");
   message_dump(thread_data.message_que[QUE_SEND]);
   
   exit(0);
}

/* Sends a message without any formatting applied to it */
int sendRaw(char* message, thread_data_t* thread_data) {
   if(sendto(thread_data->sockfd, message, strlen(message), 0, (struct sockaddr *)thread_data->their_addr, sizeof(*thread_data->their_addr)) < 0) {
      perror("sentto");
      thread_data->running = 0;
      return EXIT_FAILURE;
   }
   return EXIT_SUCCESS;
}

/* Sends a message with the username attached */
int sendMessage(char* message, thread_data_t* thread_data) {
   char buffer[MAX_DATA_SIZE];
   snprintf(buffer, MAX_DATA_SIZE-1, "<%s> %s", thread_data->username, message);
   if(!sendRaw(buffer, thread_data)) {
      return EXIT_FAILURE;
   }  

   return EXIT_SUCCESS;
}

/* The sending thread */
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
   
   fprintf(stdout, "Send thread, self-terminating...\n");
   return (void*)EXIT_SUCCESS;
}

/* The reciving thread */
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
   
   fprintf(stdout, "Recv thread, self-terminating...\n");
   return (void*)EXIT_SUCCESS;
}

/* The ncurses thread */
void* prwdy(void *arg) {
   thread_data_t* thread_data;
   thread_data = (thread_data_t*)arg; 
   int c = 0xDEADC0DE;
   WINDOW* win = NULL;
   
   if(!(win = initscr())) {
      thread_data->running = 0;
      fprintf(stderr, "ERROR initilizing ncurses...\n");
      return (void*)EXIT_FAILURE;
   }
   
   nonl();
   cbreak();
   nodelay(win, 1);
   
   //if(has_color()) { - This doesn't really exist,
      start_color();
      
      init_pair(COLOR_BLACK, COLOR_BLACK, COLOR_BLACK);
      init_pair(COLOR_GREEN, COLOR_GREEN, COLOR_BLACK);
      init_pair(COLOR_RED, COLOR_RED, COLOR_BLACK);
      init_pair(COLOR_CYAN, COLOR_CYAN, COLOR_BLACK);
      init_pair(COLOR_WHITE, COLOR_WHITE, COLOR_BLACK);
      init_pair(COLOR_MAGENTA, COLOR_MAGENTA, COLOR_BLACK);
      init_pair(COLOR_BLUE, COLOR_BLUE, COLOR_BLACK);
      init_pair(COLOR_YELLOW, COLOR_YELLOW, COLOR_BLACK);
   //}
   
   draw_prwdy(thread_data, win);
   while(thread_data->running) {
      c = getch();
      if(c != ERR) {
         if(c == 13) {
            draw_prwdy(thread_data, win);
         }
      }
      
   }
   
   endwin();
   
   fprintf(stdout, "prwdy thread, self-terminating...\n");
   
   return (void*)EXIT_SUCCESS;
}

/* Draws the ncurses interface */
void draw_prwdy(thread_data_t* thread_data, WINDOW* win) {
   int xmax = 0xDEADC0DE;
   int ymax = 0xDEADC0DE;
   getmaxyx(win, ymax, xmax);
   
   border('|', '|', '-', '-', '/', '\\', '\\', '/');
   mvgetch(ymax-3, 1);
   hline('-', xmax-2);
   mvgetch(ymax-2, 2);
}

/* Handles ctrl+c */
static void seppuku(int sig) {
   if(thread_data.running == 0) {
      printf("Whoever is out of patience is out of possession of his soul.\n");
      printf("\t- Francis Bacon\n");
   } else {
      thread_data.running = 0;
      printf("Death-threat recived, Suiciding!\n");
   }
}
