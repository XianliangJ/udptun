/**
 * \file cli.c
 * \brief The client implementation.
 * \author k.edeline
 * \version 0.1
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>

#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "cli.h"
#include "debug.h"
#include "state.h"
#include "thread.h"
#include "sock.h"
#include "net.h"
#include "xpcap.h"

/**
 * \var static volatile int loop
 * \brief The client loop guardian.
 */
static volatile int loop;

/**
 * \fn static void tun_cli_in(int fd_udp, int fd_tun, struct sockaddr_in *udp_addr, char *buf)
 * \brief Forward a packet in the tunnel.
 *
 * \param fd_udp The udp socket fd.
 * \param fd_tun The tun interface fd.
 * \param udp_addr The address of the udp target.
 * \param buf The buffer.
 */ 
static void tun_cli_in(int fd_tun, int fd_udp4,  int fd_udp6,
                       struct tun_state *state, char *buf);
static void tun_cli_in4(int fd_tun, int fd_udp, 
                        struct tun_state *state, char *buf);
static void tun_cli_in6(int fd_tun, int fd_udp, 
                        struct tun_state *state, char *buf);
static void tun_cli_in4_aux(int fd_udp, struct tun_state *state, char *buf, int recvd);
static void tun_cli_in6_aux(int fd_udp, struct tun_state *state, char *buf, int recvd);

/**
 * \fn static void tun_cli_out(int fd_udp, int fd_tun, char *buf)
 * \brief Forward a packet out of the tunnel.
 *
 * \param fd_udp The udp socket fd.
 * \param fd_tun The tun interface fd.
 * \param buf The buffer. 
 */ 
static void tun_cli_out(int fd_udp, int fd_tun, struct tun_state *state, char *buf);

static void tun_cli_single(struct arguments *args);
static void tun_cli_dual(struct arguments *args);


void cli_shutdown(int UNUSED(sig)) { 
   debug_print("shutting down client ...\n");

   /* Wait for delayed acks to avoid sending icmps */
   sleep(CLOSE_TIMEOUT);
   loop = 0; 
}

void tun_cli(struct arguments *args) {
   if (args->dual_stack)
      tun_cli_dual(args);
   else
      tun_cli_single(args);
}

void tun_cli_in(int fd_tun, int fd_udp4, int fd_udp6,
                struct tun_state *state, char *buf) {
   int recvd=xread(fd_tun, buf, BUFF_SIZE);
   debug_print("recvd %db from tun\n", recvd);

   switch (buf[0] & 0xf0) {
      case 0x40:
         tun_cli_in4_aux(fd_udp4, state, buf, recvd);
         break;
      case 0x60:
         tun_cli_in6_aux(fd_udp6, state, buf, recvd);
         break;
      default:
         debug_print("non-ip proto:%d\n", buf[0]);
         break;
   }
}

void tun_cli_in6(int fd_tun, int fd_udp, 
                 struct tun_state *state, char *buf) {
   int recvd=xread(fd_tun, buf, BUFF_SIZE);
   debug_print("recvd %db from tun\n", recvd);
   tun_cli_in6_aux(fd_udp, state, buf, recvd);
}

void tun_cli_in4(int fd_tun, int fd_udp,
                 struct tun_state *state, char *buf) {
   int recvd=xread(fd_tun, buf, BUFF_SIZE);
   debug_print("recvd %db from tun\n", recvd);
   tun_cli_in4_aux(fd_udp, state, buf, recvd);
}

void tun_cli_in4_aux(int fd_udp, struct tun_state *state, char *buf, int recvd) {
   /* Remove PlanetLab TUN PPI header */
   if (state->planetlab) {
      buf+=4;recvd-=4;
   }

   /* lookup initial server database from file */
   struct tun_rec *rec = NULL; 
   in_addr_t priv_addr4 = (int) *((uint32_t *)(buf+16));
   debug_print("%s\n", inet_ntoa((struct in_addr){priv_addr4}));

   /* lookup private addr */
   if ( (rec = g_hash_table_lookup(state->cli4, &priv_addr4)) ) {
      int sent = xsendto4(fd_udp, rec->sa4, buf, recvd);
      debug_print("cli: wrote %dB to udp\n",sent);

   } else {
      errno=EFAULT;
      die("cli lookup");
   }
}

void tun_cli_in6_aux(int fd_udp, struct tun_state *state, char *buf, int recvd) {
   struct tun_rec *rec = NULL; 

   /* Remove PlanetLab TUN PPI header */
   if (state->planetlab) {
      buf+=4;recvd-=4;
   }

   /* lookup initial server database from file */
   char priv_addr6[16], str_addr6[INET6_ADDRSTRLEN];
   memcpy(priv_addr6, buf+24, 16);
   debug_print("%s\n", inet_ntop(AF_INET6, priv_addr6, 
                         str_addr6, INET6_ADDRSTRLEN));

   /* lookup private addr */
   if ( (rec = g_hash_table_lookup(state->cli6, priv_addr6)) ) {
      int sent = xsendto6(fd_udp, rec->sa6, buf, recvd);
      debug_print("cli: wrote %dB to udp\n",sent);

   } else {
      errno=EFAULT;
      die("cli lookup");
   }
}

void tun_cli_out(int fd_udp, int fd_tun, struct tun_state *state, char *buf) {
   int recvd = xrecv(fd_udp, buf, BUFF_SIZE);

   if (recvd > MIN_PKT_SIZE) {
      debug_print("cli: recvd %dB from udp\n", recvd);

      /* Add PlanetLab TUN PPI header */
      if (state->planetlab) {
         buf-=4; recvd+=4;
      }

      int sent = xwrite(fd_tun, buf, recvd);
      debug_print("cli: wrote %dB to tun\n", sent);
   } else if (recvd < 0) {
      /* recvd ICMP msg */
      xrecverr(fd_udp, buf, BUFF_SIZE, 0, NULL);
   } else {
      /* recvd unknown packet */
      debug_print("recvd empty pkt\n");
   }   
}

void tun_cli_single(struct arguments *args) {
   int fd_tun = 0, fd_udp = 0; 
   void (*tun_cli_in_func)(int,int,struct tun_state*,char*);

   /* init state */
   struct tun_state *state = init_tun_state(args);

   /* create tun if and sockets */   
   tun(state, &fd_tun);
   if (state->ipv6) {
      fd_udp = udp_sock6(state->public_port, 1, state->public_addr6);
      tun_cli_in_func = &tun_cli_in6;
   } else {
      fd_udp = udp_sock4(state->public_port, 1, state->public_addr4);
      tun_cli_in_func = &tun_cli_in4;
   }

   /* run capture threads */
   xthread_create(capture_notun, (void *) state, 1);
   synchronize();

   /* run client */
   debug_print("running cli ...\n");    
   xthread_create(cli_thread, (void*) state, 1);

   /* init select loop */
   fd_set input_set;
   struct timeval tv;
   int sel = 0, fd_max = 0;
   char buf[BUFF_SIZE], *buffer;
   buffer = buf;

   if (state->planetlab) {
      buffer[0]=0;buffer[1]=0;
      buffer[2]=8;buffer[3]=0;
      buffer+=4;
   }

   fd_max = max(fd_udp, fd_tun);
   loop = 1;
   signal(SIGINT, cli_shutdown);
   signal(SIGTERM, cli_shutdown);

   while (loop) {
      FD_ZERO(&input_set);
      FD_SET(fd_udp, &input_set);
      FD_SET(fd_tun, &input_set);

      sel = xselect(&input_set, fd_max, &tv, state->inactivity_timeout);

      if (sel == 0) {
         debug_print("timeout\n"); 
         break;
      } else if (sel > 0) {
         if (FD_ISSET(fd_tun, &input_set))      
            (*tun_cli_in_func)(fd_udp, fd_tun, state, buffer);
         if (FD_ISSET(fd_udp, &input_set)) 
            tun_cli_out(fd_udp, fd_tun, state, buffer);
      }
   }
}

void tun_cli_dual(struct arguments *args) {
   int fd_tun = 0, fd_udp4 = 0, fd_udp6 = 0; 
   void (*tun_cli_in_func)(int,int,struct tun_state*,char*);

   /* init state */
   struct tun_state *state = init_tun_state(args);

   /* create tun if and sockets */   
   tun(state, &fd_tun);
   fd_udp4 = udp_sock4(state->public_port, 1, state->public_addr4);
   fd_udp6 = udp_sock6(state->public_port, 1, state->public_addr6);

   /* run capture threads */
   xthread_create(capture_notun, (void *) state, 1);
   synchronize();

   /* run client */
   debug_print("running cli ...\n");    
   xthread_create(cli_thread, (void*) state, 1);

   /* init select loop */
   fd_set input_set;
   struct timeval tv;
   int sel = 0, fd_max = 0;
   char buf[BUFF_SIZE], *buffer;
   buffer = buf;

   if (state->planetlab) {
      buffer[0]=0;buffer[1]=0;
      buffer[2]=8;buffer[3]=0;
      buffer+=4;
   }

   fd_max = max(max(fd_udp4, fd_udp6), fd_tun);
   loop = 1;
   signal(SIGINT, cli_shutdown);
   signal(SIGTERM, cli_shutdown);

   while (loop) {
      FD_ZERO(&input_set);
      FD_SET(fd_udp4, &input_set);
      FD_SET(fd_udp6, &input_set);
      FD_SET(fd_tun, &input_set);

      sel = xselect(&input_set, fd_max, &tv, state->inactivity_timeout);

      if (sel == 0) {
         debug_print("timeout\n"); 
         break;
      } else if (sel > 0) {
         if (FD_ISSET(fd_tun, &input_set))      
            tun_cli_in(fd_tun, fd_udp4, fd_udp6, state, buffer);
         if (FD_ISSET(fd_udp4, &input_set)) 
            tun_cli_out(fd_udp4, fd_tun, state, buffer);
         if (FD_ISSET(fd_udp6, &input_set)) 
            tun_cli_out(fd_udp6, fd_tun, state, buffer);
      }
   }
}

