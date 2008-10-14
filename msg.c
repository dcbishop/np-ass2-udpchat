#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h> /* sleep() */
#include <netinet/in.h> /* INADDR_ANY, IPPROTO_*, IP_* declarations */
#include <arpa/inet.h> /* inet_aton */
#include <string.h> /* memset */
#include <unistd.h> /* gethostname(...) */
#include <netdb.h> /* MAXHOSTNAMELEN (Solaris only)*/
#include <pthread.h>
#include <curses.h> /* For the prettyness */
#include <signal.h>
#include <getopt.h>
#include <time.h>
#include <errno.h>
extern int errno;


#define DEFAULT_NAME "Sgt.Fury"
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
   // Non-critial, don't care about mutex for this
   int running;
   int resize_event;

   // Read only after set, no mutex needed
   int sockfd;
   char username[MAX_DATA_SIZE];
   char hostname[MAXHOSTNAMELEN];
   struct sockaddr_in* their_addr;
   pthread_mutex_t mp; // The actual mutex
   uint16_t priv_port;

   // Mutex locks needed
   message_node_t* message_que[2];
   message_node_t* message_last[2];
   int message_count[2];
   int new_messages;
   
   char whois_username[MAX_DATA_SIZE];
   char whois_hostname[MAX_DATA_SIZE];
   int whois_port;
   
} thread_data_t;
static thread_data_t thread_data;

// Function prototypes
void nuke_whois(thread_data_t* thread_data);
message_node_t* add_message_actual(message_node_t** head_node, char* message);
void message_dump(message_node_t* node);
message_node_t* add_message(thread_data_t* thread_data, int que, char* message);
void message_apocalypse(thread_data_t* thread_data, int que);
int sendRaw(char* message, thread_data_t* thread_data);
int sendMessage(char* message, thread_data_t* thread_data);
void* msg_send(void *arg);
void* msg_recv(void *arg);
void* msg_recv_priv(void *arg);
void* msg_recv_priv_recieved(void *arg);
int priv_mesg(thread_data_t* thread_data, char* name, char* message);
void prwdy_resize();
void* prwdy(void *arg);
void draw_prwdy(thread_data_t* thread_data, WINDOW* win, char* buffer);
static void seppuku(int sig);

/* Clears the whois information */
void nuke_whois(thread_data_t* thread_data) {
   pthread_mutex_lock(&thread_data->mp); // Lock the mutex
   thread_data->whois_username[0]='\0';
   thread_data->whois_hostname[0]='\0';
   thread_data->whois_port = 0;
   pthread_mutex_unlock(&thread_data->mp); // Unlock the mutex
}

/* Adds a message to the message que, (No counter increase) [WARNING: No mutex locking] */
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

/* Adds a message to the specified que */
message_node_t* add_message(thread_data_t* thread_data, int que, char* message) {
   message_node_t* newmessage;
   
   pthread_mutex_lock(&thread_data->mp); // Lock the mutex
   newmessage = add_message_actual(&thread_data->message_que[que], message);
   if(!newmessage) {
      return NULL;
   }
   
   thread_data->message_count[que]++;
   thread_data->message_last[que] = newmessage;
   thread_data->new_messages=1;
   pthread_mutex_unlock(&thread_data->mp); // Unlock the mutex
   return newmessage;
}

/* Frees all messages in a que [WARNING: No mutex locking] */
void message_apocalypse(thread_data_t* thread_data, int que) {
   message_node_t* node = thread_data->message_que[que];
   message_node_t* prev = NULL;
   while(node) {
      free(node->message);
      prev = node;
      node = node->next;
      free(prev);
   }
   thread_data->message_que[que] = NULL;      
   thread_data->message_count[que] = 0;
}

/* Dumps a message to the console and the message que */
void logmsg(thread_data_t* thread_data, FILE* pipe, char* format, ...) {
   char message[MAX_DATA_SIZE];
   va_list ap;
   va_start(ap, format);
   vsnprintf(message, MAX_DATA_SIZE-1, format, ap);
   va_end(ap);
   mvprintw(4, 4, message);
   //add_message(thread_data, QUE_RECEIVE, message);
   fprintf(pipe, "%s\n", message);   
}

/* Prints instructions */
void usage(char* name) {
   printf("Usage: %s [OPTION]\n\n", name);
   printf("-u, --username set username.\n");
   printf("-p, --port set port.\n");
   printf("-m, --mcast_addr set multicast address.\n");
}

int main(int argc, char* argv[]) {
   const unsigned char ttl = 1;
   const int on = 1;
   int sockfd = 0xDEADC0DE;
   struct sockaddr_in my_addr;
   struct sockaddr_in their_addr;
   char buffer[MAX_DATA_SIZE];
   
   char address[MAX_DATA_SIZE] = {'\0'};
   int port = 0xDEADC0DE;
   
   thread_data.running = 1;

   nuke_whois(&thread_data);

   pthread_t msg_send_tid = 0xDEADC0DE;
   pthread_t msg_recv_tid = 0xDEADC0DE;
   pthread_t msg_recv_priv_tid = 0xDEADC0DE;
   pthread_t prwdy_tid = 0xDEADC0DE;
   
   /* return values for pthread */
   int msg_send_result = 0xDEADC0DE;
   int msg_recv_result = 0xDEADC0DE;
   int msg_recv_priv_result = 0xDEADC0DE;
   int prwdy_result = 0xDEADC0DE;
   
   if(pthread_mutex_init(&thread_data.mp, NULL)) {
      logmsg(&thread_data, stderr, "mutex init failed..."); 
      perror("pthread_mutex_init");
      exit(EXIT_FAILURE);
   }

   struct ip_mreq mreq;
   inet_aton(DEFAULT_ADDRESS, &mreq.imr_multiaddr);
   mreq.imr_interface.s_addr = INADDR_ANY;
   
   gethostname(thread_data.hostname, MAXHOSTNAMELEN);
   
   thread_data.username[0] = '\0';
  
   static struct option long_options[] =
   {
      {"rflag", no_argument, 0, 'r'},
      {"help", no_argument, 0, 'h'},
      {"username", required_argument, 0, 'u'},
      {"mcast_addr", required_argument, 0, 'm'},
      {"port", required_argument, 0, 'p'},
      {0, 0, 0, 0}
   };
   int option_index;
  
   int name = 0xDEADC0DE;
   while ((name = getopt_long(argc, argv, "u:m:p:rh", long_options, &option_index)) != -1) {
      switch(name) {
         case 'u':
            strncpy(thread_data.username, optarg, MAX_DATA_SIZE);
            break;
         case 'm':
            strncpy(address, optarg, MAX_DATA_SIZE);
            break;
         case 'r':
            logmsg(&thread_data, stdout, "%s: You have set the r flag which does nothing, thats just swell.", argv[0]);
            break;
         case 'p':
            port = atoi(optarg);
            break;
         case 'h':
            usage(argv[0]);
            exit(EXIT_SUCCESS);
            break;
         case '?': 
            logmsg(&thread_data, stderr, buffer, "%s: Thats not right... try again...", argv[0]);
            usage(argv[0]);
            exit(EXIT_FAILURE);
            break;
      }
   }
   
   if(thread_data.username[0] == '\0') {
      strncpy(thread_data.username, DEFAULT_NAME, MAX_DATA_SIZE);
      logmsg(&thread_data, stdout, buffer, "%s: Due to your failure to specify a name, you will hence forth be known as", argv[0]);
      logmsg(&thread_data, stdout, buffer, "'%s'.", thread_data.username);
   }
   
   if(address[0] == '\0') {
      strncpy(address, DEFAULT_ADDRESS, MAX_DATA_SIZE);
      logmsg(&thread_data, stdout, buffer, "%s: %s, because of your apparent inability to specify a", argv[0], thread_data.username);
      logmsg(&thread_data, stdout, buffer, "multicast address, %s has been assigned for you.", address);
   }
   
   if(port < 0) {
      port = 12345;
      logmsg(&thread_data, stdout, buffer, "%s: %s, as you have not chosen a port number %d has been set.", argv[0], thread_data.username, port);
   }
   
   memset(&my_addr, 0, sizeof(my_addr));
   my_addr.sin_family = AF_INET;
   my_addr.sin_addr.s_addr = htonl(INADDR_ANY);
   my_addr.sin_port = htons(port);

   their_addr.sin_family = AF_INET;
   their_addr.sin_port = htons(port);
   inet_aton(address, &their_addr.sin_addr);
   memset(their_addr.sin_zero, '\0', sizeof their_addr.sin_zero);
   thread_data.their_addr = &their_addr;

   sockfd = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);
   if(sockfd < 0) {
      perror("socket");
      logmsg(&thread_data, stderr, "socket failed..."); 
      exit(EXIT_FAILURE);
   }
   if(setsockopt(sockfd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) < 0) {
      logmsg(&thread_data, stderr, "setsockopt failed [IP_MULTICAST_TTL]..."); 
      perror("setsockopt (IP_MULTICAST_TTL)");
      exit(EXIT_FAILURE);
   }
   if(setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0 ) {
      logmsg(&thread_data, stderr, "setsockopt failed REUSEADDR..."); 
      perror("setsockopt (SO_REUSEADDR)");
      exit(EXIT_FAILURE);
   }
   if(bind(sockfd, (struct sockaddr*) &my_addr, sizeof(my_addr)) < 0) {
      logmsg(&thread_data, stderr, "bind failed...");    
      perror("bind");
      exit(EXIT_FAILURE);
   }
   if(setsockopt(sockfd, IPPROTO_IP, IP_ADD_MEMBERSHIP, (void*)&mreq, sizeof(mreq)) < 0) {
      logmsg(&thread_data, stderr, "bind failed...");   
      perror("setsockopt (IP_ADD_MEMBERSHIP)");
      exit(EXIT_FAILURE);
   }
   
   thread_data.sockfd = sockfd;

   // Handle signals
   (void) signal(SIGINT, seppuku);
   (void) signal(SIGPIPE, SIG_IGN);
   (void) signal(SIGWINCH, prwdy_resize);
   
   // Spawn threads...   
   msg_send_result = pthread_create(&msg_send_tid, NULL, msg_send, (void*)&thread_data);
   if(msg_send_result != 0) {
      logmsg(&thread_data, stderr, "pthread_create (msg_send) failed...");
      perror("pthread_create (send)");
      exit(EXIT_FAILURE);
   }

   msg_recv_result = pthread_create(&msg_recv_tid, NULL, msg_recv, (void*)&thread_data);
   if(msg_recv_result != 0) {
      logmsg(&thread_data, stderr, "pthread_create (msg_recv) failed...");  
      perror("pthread_create (recv)");
      exit(EXIT_FAILURE);
   }

   msg_recv_priv_result = pthread_create(&msg_recv_priv_tid, NULL, msg_recv_priv, (void*)&thread_data);
   if(msg_recv_priv_result != 0) {
      logmsg(&thread_data, stderr, "pthread_create (msg_recv_priv) failed..."); 
      perror("pthread_create (recv_priv)");
      exit(EXIT_FAILURE);
   }
   
   prwdy_result = pthread_create(&prwdy_tid, NULL, prwdy, (void*)&thread_data);
   if(prwdy_result != 0) {
      logmsg(&thread_data, stderr, "pthread_create (prwdy) failed..."); 
      perror("pthread_create (prwdy)");
      exit(EXIT_FAILURE);
   }

   pthread_detach(msg_recv_tid);
   pthread_detach(msg_recv_priv_tid);
   pthread_join(prwdy_tid, NULL);
   pthread_join(msg_send_tid, NULL);

   // Drop membership
   if(setsockopt(sockfd, IPPROTO_IP, IP_DROP_MEMBERSHIP, (void*)&mreq, sizeof(mreq)) < 0) {
      logmsg(&thread_data, stderr, "ERROR: setsockopt (IP_DROP_MEMBERSHIP) failed..."); 
      perror("setsockopt (IP_DROP_MEMBERSHIP)");
      exit(EXIT_FAILURE);
   }
   
   // Close UDP socket
   close(sockfd);
   
   //Destroy the mutex
   if(pthread_mutex_destroy(&thread_data.mp)) {
      fprintf(stderr, "%s: Error destroying mutex...\n", argv[0]);
   }
   
   printf("Good for you! You reached the end of the program....\n");
   
   printf("%d\n",  thread_data.running);  /*DEBUG*/
   exit(EXIT_SUCCESS);
}

/* Sends a message without any formatting applied to it */
int sendRaw(char* message, thread_data_t* thread_data) {
   if(sendto(thread_data->sockfd, message, strlen(message), 0, (struct sockaddr *)thread_data->their_addr, sizeof(*thread_data->their_addr)) < 0) {
      if(errno == EPIPE) {
         logmsg(thread_data, stderr, "WARNING: sendto failed, EPIPE received...");
         return EXIT_SUCCESS;
      /*} else if (errno = EINT) {
         logmsg(thread_data, stderr, "WARNING: sendto failed, EINT received...");
         return EXIT_SUCCESS;*/
      }
      perror("sendto");
      logmsg(thread_data, stderr, "ERROR: sendto failed...");
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
   logmsg(thread_data, stderr, "Send message calld...");
   
   return EXIT_SUCCESS;
}

/* The sending thread */
void* msg_send(void *arg) {
   thread_data_t* thread_data;
   thread_data = (thread_data_t*)arg;
   message_node_t* node = NULL;

   char buffer[MAX_DATA_SIZE];
   snprintf(buffer, MAX_DATA_SIZE-1, "<%s@%s entered chat group>", thread_data->username, thread_data->hostname);   
   
   if(sendRaw(buffer, thread_data) == EXIT_FAILURE) {
      logmsg(thread_data, stderr, "ERROR: Failed to send join message...");
   }
   
   while(thread_data->running) {
      pthread_mutex_lock(&thread_data->mp); // Lock the mutex
      if(thread_data->message_count[QUE_SEND] > 0) {
         node=thread_data->message_que[QUE_SEND];
         while(node != NULL) {
            sendMessage(node->message, thread_data);
            node = node->next;
         }
         message_apocalypse(thread_data, QUE_SEND);
      }
      pthread_mutex_unlock(&thread_data->mp); // Lock the mutex
   }
   
   snprintf(buffer, MAX_DATA_SIZE-1, "<%s@%s leaving chat group>", thread_data->username, thread_data->hostname);   
   if(sendRaw(buffer, thread_data)) {
      logmsg(thread_data, stderr, "WARNING: Failed to send depart message...");
   }
   
   logmsg(thread_data, stdout, "Send thread, self-terminating...");
   return (void*)EXIT_SUCCESS;
}

#define MSG_WHOIS "whois "
/* The reciving thread */
void* msg_recv(void *arg) {
   thread_data_t* thread_data;
   thread_data = (thread_data_t*)arg;
   
   int bytes = 0xDEADC0DE;
   char buffer[MAX_DATA_SIZE];
   char message[MAX_DATA_SIZE];
  
   while(thread_data->running) {
      bytes = recvfrom(thread_data->sockfd, buffer, MAX_DATA_SIZE-1, 0, NULL, 0);
      if(bytes < 0) {
         perror("recvfrom");
         thread_data->running = 0;
         return (void*)EXIT_FAILURE;
      }
      buffer[bytes] = '\0';
      
      /* Handle whois requests */
      if(strncmp(buffer, MSG_WHOIS, strlen(MSG_WHOIS)) == 0) {
         char* namestart = buffer+strlen(MSG_WHOIS);

         /* Check if the request is for this user and respond */
         if((strncmp(namestart, thread_data->username, strlen(namestart) ) == 0) 
         && strlen(namestart) == strlen(thread_data->username)) {
            add_message(thread_data, QUE_RECEIVE, "Recieved a whois request...");
            snprintf(message, MAX_DATA_SIZE, "%s@%s:%d", 
               thread_data->username, thread_data->hostname, thread_data->priv_port);
               
            sendRaw(message, thread_data); /* Send a whois response */
         }         
      /* Handle a normal message */
      } else if(buffer[0] == '<') { 
         add_message(thread_data, QUE_RECEIVE, buffer);
      /* Handle a whois response */
      } else {
         char* first = strchr(buffer, '@');
         char* second = strchr(buffer, ':');
         char port[MAX_DATA_SIZE];
         
         if(first && second) {
            strncpy(thread_data->whois_username, buffer, first-buffer);
            strncpy(thread_data->whois_hostname, first+1, second-first-1);
            strncpy(port, second+1, strlen(buffer));
            thread_data->whois_port = atoi(port);
         } else {
            /* Display unknown messages */
            add_message(thread_data, QUE_RECEIVE, buffer);
         }

      }
   }
   
   logmsg(thread_data, stdout, "Recv thread, self-terminating...");
   return (void*)EXIT_SUCCESS;
}

/* Process thread for receving private messages */
void* msg_recv_priv(void *arg) {
   thread_data_t* thread_data;
   thread_data = (thread_data_t*)arg;

   pthread_t thread_id;   
   int sockfd = 0xDEADC0DE;
   int fd_new = 0xDEADC0DE;
   int error = 0;
   const int on = 1;
   int pthread_result = 0xDEADC0DE;
   struct sockaddr_in result;
   socklen_t result_size = sizeof(result);

   sockfd = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
   if(sockfd == -1) {
      logmsg(thread_data, stderr, "Private message socket() error...");
      thread_data->running = 0;
      perror("[priv_msg] socket");
      return (void*)EXIT_FAILURE;
   }   

   if(setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0) {
      logmsg(thread_data, stderr, "Private message setsockopt() SO_REUSEADDR error...");
      perror("[priv_msg] setsockopt");
      thread_data->running = 0;
      return (void*)EXIT_FAILURE;
   }

   if(listen(sockfd, 10) < 0) {
      logmsg(thread_data, stderr, "Private message listen() error...");
      perror("[priv_msg] listen");
      thread_data->running = 0;
      return (void*)EXIT_FAILURE;
   }

   if(getsockname(sockfd, (struct sockaddr*)&result, &result_size) < 0) {
      logmsg(thread_data, stderr, "priv_msg: getsockname() error...");
      perror("[priv_msg] listen");
      thread_data->running = 0;
      return (void*)EXIT_FAILURE;      
   }

   thread_data->priv_port = ntohs(result.sin_port);
   logmsg(thread_data, stdout, "Private messages on port %d.", thread_data->priv_port);

   while(thread_data->running) {
      fd_new = accept(sockfd, NULL, NULL);
      if(fd_new < 0) {
         logmsg(thread_data, stdout, "Private message accept() error...");
         perror("[priv_msg] accept");
         error = 1;
         break;
      }

      pthread_result = pthread_create(&thread_id, NULL, msg_recv_priv_recieved, (void*)&fd_new);
      if (pthread_result != 0) {
      	 logmsg(thread_data, stdout, "Private message pthread() error...");
         perror("[priv_msg] pthread");
         error = 1;
         break;
      }
      pthread_detach(thread_id);
      close(fd_new);
   }  

   thread_data->running = 0;   
   close(sockfd);
   return (void*)error;
}

/* For when a private message is recieved */
void* msg_recv_priv_recieved(void *arg) {
   int fd_new = *((int*)arg);
   thread_data_t* thread_data = thread_data;
   
   char buf[MAX_DATA_SIZE];
   int numbytes = recv(fd_new, buf, MAX_DATA_SIZE-1, 0);
   if (numbytes == -1) {
      perror("[msg_recv_priv_recieved] recv");
      logmsg(thread_data, stdout, "msg_recv_priv_recieved recv error...");
      close(fd_new);
      return (void*)EXIT_FAILURE;
   }
   
   buf[numbytes] = '\0';
   add_message(thread_data, QUE_RECEIVE, buf);
   
   close(fd_new);
   return (void*)EXIT_SUCCESS;
}

/* Sends a private message */
int priv_mesg(thread_data_t* thread_data, char* name, char* message) {

   char hostname[MAX_DATA_SIZE] = {'\0'};
   int port = 0xDEADC0DE;
   char final_message[MAX_DATA_SIZE];
   
   if(name == NULL || message == NULL) {
      return EXIT_FAILURE;
   }
   
   /* Perform a whois request */
   time_t start_time = time(NULL);
   nuke_whois(thread_data);
   char whois_message[MAX_DATA_SIZE];
   snprintf(whois_message, MAX_DATA_SIZE-1, "whois %s", name);
   sendRaw(whois_message, thread_data);
   
   int got_reply = 0;

   while(time(NULL) - start_time < 6 && !got_reply) {
      pthread_mutex_lock(&thread_data->mp); // Lock the mutex
      if(strcmp(thread_data->whois_username, name) == 0) {
         got_reply = 1;
         strncpy(hostname, thread_data->whois_hostname, MAX_DATA_SIZE);
         port = thread_data->whois_port;
      }
      pthread_mutex_unlock(&thread_data->mp); // Unlock the mutex
   }
         
   if(!got_reply) {
      logmsg(thread_data, stderr, "[priv_mesg] ERROR: No whois response...");
      return EXIT_FAILURE;
   }
   
   /* Print the send message */
   snprintf(final_message, MAX_DATA_SIZE-1, "*%s > %s* %s", thread_data->username, name, message);
   logmsg(thread_data, stdout, final_message);
   
   snprintf(final_message, MAX_DATA_SIZE-1, "*%s* %s", thread_data->username, message);
   
   // Send the message...
   int sockfd = 0xDEADC0DE;
   int ret = 0xDEADC0DE;
   struct sockaddr_in server;
   struct hostent *hptr;
   ssize_t nbytes;
   char **ptr = NULL;

   sockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
   if(sockfd==-1) {
      perror("socket");
      logmsg(thread_data, stderr, "[priv_mesg] ERROR: socket() call failed...");
      return EXIT_FAILURE;
   }
   
   memset(&server, 0, sizeof(struct sockaddr_in));
   server.sin_family = AF_INET;
   server.sin_port = htons(port);
   
   if(inet_aton(hostname, &server.sin_addr) < 0) {
      hptr = gethostbyname(hostname);
      
      if(hptr == NULL) {
         perror("gethostbyname");
         logmsg(thread_data, stderr, "[priv_mesg] ERROR: gethostbyname() failed...");
         return EXIT_FAILURE;
      }
      ptr = hptr->h_addr_list;
      memcpy( &server.sin_addr, *ptr, sizeof( struct in_addr ) );
   }
   
   ret = connect( sockfd, (struct sockaddr *)&server, sizeof server );
   if( ret == -1 ) {
      //endwin(); /* DEBUG */
      //printf("%s, %d\n", inet_ntoa(server.sin_addr), ntohs(server.sin_port));
      //exit(1);
      perror("connect");
      logmsg(thread_data, stderr, "[priv_mesg] ERROR: connect() failed...");
      return EXIT_FAILURE;
   }

   // TODO: epipe
   nbytes = write( sockfd, final_message, strlen( final_message ) );
   if( nbytes == -1 ) {
      perror("write");
      logmsg(thread_data, stderr, "[priv_mesg] ERROR: write() failed...");
      close(sockfd);
      return EXIT_FAILURE;
   }

   close(sockfd);
   return EXIT_SUCCESS;
}

/* When a resize signal is recieved */
void prwdy_resize() {
   thread_data.resize_event = 1;
}

/* The ncurses thread */
void* prwdy(void *arg) {
   thread_data_t* thread_data = (thread_data_t*)arg; 
   int c = 0xDEADC0DE;
   int chnum = 0;
   char buffer[MAX_DATA_SIZE] = {'\0'};
   
   /* For private messages */
   char name[MAX_DATA_SIZE];
   char* message = '\0';
   char* name_start = NULL;
   WINDOW* win = NULL;
   
   thread_data->resize_event = 1; // Ensure that the screen is initilized on first run
   while(thread_data->running) {
   
      // Initilize the screen on a resize
      if(thread_data->resize_event) {
         if(win)
       endwin();
       
         win = initscr();
         if(!win) {
            thread_data->running = 0;
            snprintf(buffer, MAX_DATA_SIZE, "ERROR initilizing ncurses...");
            logmsg(thread_data, stderr, buffer);
            return (void*)EXIT_FAILURE;
         }

         thread_data->resize_event = 0;
         nonl();
         cbreak();
         nodelay(win, 1);
         keypad(win, 1);
    
         //if(can_change_color()) { // This fails always on Solaris?
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
    
         draw_prwdy(thread_data, win, buffer);
      }
      
      // Update the display if there is a new message
      pthread_mutex_lock(&thread_data->mp); // Lock the mutex
      if(thread_data->new_messages) {
         thread_data->new_messages = 0;
         pthread_mutex_unlock(&thread_data->mp); // Unlock the mutex
         draw_prwdy(thread_data, win, buffer);
      } else {
         pthread_mutex_unlock(&thread_data->mp); // Unlock the mutex
      }
      
      // Read keys
      while((c = getch()) != ERR) {
         // When person pushes enter
         if(c == 13 || c == KEY_ENTER || chnum > MAX_DATA_SIZE-1) {
            if(buffer[0] == '\0') {
               draw_prwdy(thread_data, win, buffer);
               continue;
            }
            
            if(buffer[0] == '/') { // If its a / command
               if(strncmp(buffer, "/msg ", 5)) { // If its a /msg command
                  add_message(thread_data, QUE_RECEIVE, "Bad command.");
               } else {
                  name_start = strchr(buffer, ' ')+1;
                  if(name_start) {
                     message = strchr(name_start, ' ');
                  }
                  if(message) {
                     strncpy(name, name_start, message - name_start);
                  }

                  name[message - name_start] = '\0';
                  if(name[0] == '\0' || message == NULL) {
                     add_message(thread_data, QUE_RECEIVE, "Your syntax is in error.");
                  } else {
                     priv_mesg(thread_data, name, message+1);
                  }

                  draw_prwdy(thread_data, win, buffer);                  
                  message = NULL;
                  name[0]='\0';
                  
               }
            } else { // For normal messages
               add_message(thread_data, QUE_SEND, buffer);
            }
            chnum = 0; // Reset position to start of buffer
            draw_prwdy(thread_data, win, buffer);
         } else if(c == KEY_BACKSPACE || c == 127) {
            buffer[chnum]='\0';
            if(chnum > 0) {
               chnum--;
               buffer[chnum]='\0';
            }
            draw_prwdy(thread_data, win, buffer);
         } else { //Add new character to buffer
            buffer[chnum] = c;
            chnum++;
         }
         buffer[chnum] = '\0'; // Ensure last character is terminator
      }
   }
   
   endwin();
   
   snprintf(buffer, MAX_DATA_SIZE, "Prwdy thread, self-terminating...");
   logmsg(thread_data, stdout, buffer);
   
   return (void*)EXIT_SUCCESS;
}

/* Draws the ncurses interface */
void draw_prwdy(thread_data_t* thread_data, WINDOW* win, char* buffer) {
   int nummessages = thread_data->message_count[QUE_RECEIVE];
   int xmax = 0xDEADC0DE;
   int ymax = 0xDEADC0DE;;
   getmaxyx(win, ymax,xmax);
   int y = 0xDEADC0DE;
   y = LINES-nummessages-3;

   attron(COLOR_PAIR(COLOR_WHITE));

   werase(win);
   wrefresh(win);
   
   pthread_mutex_lock(&thread_data->mp); // Lock the mutex
   message_node_t* node = thread_data->message_que[QUE_RECEIVE];
   while(node && nummessages > 0) {
      mvwprintw(win, y, 1, "%s", node->message);
      node = node->next;
      nummessages--;
      
      y++;
      if(y > LINES-3) {
         break;
      }
   }
   pthread_mutex_unlock(&thread_data->mp); // Unlock the mutex
   
   attron(COLOR_PAIR(COLOR_YELLOW));
   border('|', '|', '-', '-', '/', '\\', '\\', '/');
   mvwhline(win, LINES-3, 1, '-', COLS-2);
   attron(COLOR_PAIR(COLOR_GREEN));
   mvwprintw(win, LINES-2, 2, buffer);
   wgetch(win);
}

/* Handles ctrl+c */
static void seppuku(int sig) {
   char buffer[MAX_DATA_SIZE];
   static int deaths = 0;
   
   if(thread_data.running == 0) {
      if(deaths < 3) {
         logmsg(&thread_data, stdout, buffer, "Whoever is out of patience is out of possession of his soul.");
         logmsg(&thread_data, stdout, "\t- Francis Bacon");
      } else {
         logmsg(&thread_data, stdout, "%s is to instant, dying badly...", thread_data.username);
         endwin();
         exit(EXIT_FAILURE);
      }
      deaths++;
   } else {
      thread_data.running = 0;
      logmsg(&thread_data, stdout, "Death-threat recived, Suiciding!");
   }
}
