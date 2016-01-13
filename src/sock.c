/**
 * \file sock.c
 * \brief Socket handling.
 *
 *    This contains system calls wrappers, socket and BPF creation 
 *    functions, tun interface creation functions, network utility 
 *    functions and die().
 *    Note that raw socket and tun interface related functions are 
 *    Planetlab-specific.
 *
 * \author k.edeline
 * \version 0.1
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pcap.h>

#include <netinet/ip_icmp.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/stat.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <linux/errqueue.h>
#include <sys/socket.h>

#include "sock.h"
#include "debug.h"
#include "icmp.h"
#include "net.h"

/**
 * \fn static build_sel(fd_set *input_set, int *fds_raw, int len, int *max_fd_raw)
 *
 * \brief build a fd_set structure to be used with select() or similar.
 *
 * \param input_set modified on return to the fd_set.
 * \param fds_raw The fd to set.
 * \param len The number of fd.
 * \param max_fd_raw modified on return to indicate the max fd value.
 */ 
static void build_sel(fd_set *input_set, int *fds_raw, int len, int *max_fd_raw);

/**
 * \fn unsigned short calcsum(unsigned short *buffer, int length)
 *
 * \brief used to calculate IP and ICMP header checksums using
 * one's compliment of the one's compliment sum of 16 bit words of the header
 * 
 * \param buffer the packet buffer
 * \param length the buffer length
 * \return checksum
 */ 
static unsigned short calcsum(unsigned short *buffer, int length);


int udp_sock(int port) {
   int s;
   struct sockaddr_in sin;
   /* UDP socket */
   if ((s=socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1)
      die("socket");

   /* sockaddr */
   memset(&sin, 0, sizeof(sin));
   sin.sin_family      = AF_INET;
   sin.sin_port        = htons(port);
   sin.sin_addr.s_addr = htonl(INADDR_ANY);

   /* bind to port */
   if( bind(s, (struct sockaddr*)&sin, sizeof(sin) ) == -1)
      die("bind");

   /* enable icmp catching */
   int on = 1;
   if (setsockopt(s, SOL_IP, IP_RECVERR, (char*)&on, sizeof(on))) 
      die("IP_RECVERR");
   
   debug_print("udp socket created on port %d\n", port);
   return s;
}

int raw_tcp_sock(const char *addr, int port, const struct sock_fprog * bpf, const char *dev) {
   return raw_sock(addr, port, bpf, dev, IPPROTO_TCP);
}

int raw_sock(const char *addr, int port, const struct sock_fprog * bpf, const char *dev, int proto) {
   int s;
   struct sockaddr_in sin;
   if ((s=socket(PF_INET, SOCK_RAW, proto)) == -1) 
      die("socket");

   int on = 1;
   if (setsockopt(s, 0, IP_HDRINCL, &on, sizeof(on))) 
      die("IP_HDRINCL");

   if (dev && setsockopt(s, SOL_SOCKET, SO_BINDTODEVICE, dev, strlen(dev))) 
      die("bind to device");

   //set bpf
   if (bpf && setsockopt(s, SOL_SOCKET, SO_ATTACH_FILTER, 
                         bpf, sizeof(struct sock_fprog)) < 0 ) 
       die("attach filter");

   memset(&sin, 0, sizeof(sin));
   sin.sin_family = AF_INET;
   sin.sin_port = htons(port);

   //bind socket to port (PL-specific)
   if(port && bind(s, (struct sockaddr*)&sin, sizeof(sin) ) == -1) 
     die("bind");

   debug_print("raw socket created on %s port %d\n", dev, port);
   return s;
}

struct sock_fprog *gen_bpf(const char *dev, const char *addr, int sport, int dport) {
   pcap_t *handle;		// Session handle 
   char errbuf[PCAP_ERRBUF_SIZE];	// Error string 
   struct bpf_program *fp= malloc(sizeof(struct bpf_program));// The compiled filter expression 

   char filter_exp[64]; // "src port " p " and dst port " p2
   if (sport && dport)
      sprintf(filter_exp, "src port %d and dst port %d", sport, dport);
   else if (sport && !dport)
      sprintf(filter_exp, "src port %d", sport);
   else if (!sport && dport)
      sprintf(filter_exp, "dst port %d", dport);

   bpf_u_int32 net = inet_addr(addr);
   handle = pcap_open_live(dev, BUFSIZ, 1, 1000, errbuf);
   if (!handle) 
      die( "Couldn't open device %s: %s");
   if (pcap_compile(handle, fp, filter_exp, 0, net) == -1) 
      die("Couldn't parse filter %s: %s\n");

   return (struct sock_fprog *)fp;
}

int xselect(fd_set *input_set, int fd_max, struct timeval *tv, int timeout) {
   int sel;
   if (timeout != -1) {
      tv->tv_sec  = timeout;
      tv->tv_usec = 0;
      sel = select(fd_max+1, input_set, NULL, NULL, tv);
   } else {
      sel = select(fd_max+1, input_set, NULL, NULL, NULL);
   }
   if (sel < 0) die("select");
   return sel;
}

int xsendto(int fd, struct sockaddr *sa, const void *buf, size_t buflen) {
   int sent = 0;
   if ((sent = sendto(fd,buf,buflen,0,sa,sizeof(struct sockaddr))) < 0) 
       die("sendto");
   return sent;
}

int xrecverr(int fd, void *buf, size_t buflen) {
   struct iovec iov;                      
   struct msghdr msg;                      
   struct cmsghdr *cmsg;                   
   struct sock_extended_err *sock_err;     
   struct icmphdr icmph;  
   struct sockaddr_in remote;

   // init structs
   iov.iov_base       = &icmph;
   iov.iov_len        = sizeof(icmph);
   msg.msg_name       = (void*)&remote;
   msg.msg_namelen    = sizeof(remote);
   msg.msg_iov        = &iov;
   msg.msg_iovlen     = 1;
   msg.msg_flags      = 0;
   msg.msg_control    = buf;
   msg.msg_controllen = buflen;

   // recv msg
   int return_status  = recvmsg(fd, &msg, MSG_ERRQUEUE);
   if (return_status < 0)
      return return_status;

   // parse msg 
   for (cmsg = CMSG_FIRSTHDR(&msg);cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
      // ip level and error 
      if (cmsg->cmsg_level == SOL_IP && cmsg->cmsg_type == IP_RECVERR) {
         sock_err = (struct sock_extended_err*)CMSG_DATA(cmsg); 
         // icmp msgs
         if (sock_err && sock_err->ee_origin == SO_EE_ORIGIN_ICMP) 
            print_icmp_type(sock_err->ee_type, sock_err->ee_code);
         else debug_print("non-icmp err msg\n");
      } 
   }
   return 0;
}

int xfwerr(int fd, void *buf, size_t buflen, int fd_out, struct tun_state *state) {
   struct iovec iov;                      
   struct msghdr msg;                      
   struct cmsghdr *cmsg;                   
   struct sock_extended_err *sock_err;     
   struct icmphdr icmph;  
   struct sockaddr_in remote;

   // init structs
   iov.iov_base       = &icmph;
   iov.iov_len        = sizeof(icmph);
   msg.msg_name       = (void*)&remote;
   msg.msg_namelen    = sizeof(remote);
   msg.msg_iov        = &iov;
   msg.msg_iovlen     = 1;
   msg.msg_flags      = 0;
   msg.msg_control    = buf;
   msg.msg_controllen = buflen;

   // recv msg
   int return_status  = recvmsg(fd, &msg, MSG_ERRQUEUE), i;
   if (return_status < 0)
      return return_status;

   // parse msg 
   for (cmsg = CMSG_FIRSTHDR(&msg);cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
      // ip level and error 
      if (cmsg->cmsg_level == SOL_IP && cmsg->cmsg_type == IP_RECVERR) {
         sock_err = (struct sock_extended_err*)CMSG_DATA(cmsg);
          /* print err type */
         if (sock_err && sock_err->ee_origin == SO_EE_ORIGIN_ICMP) 
            print_icmp_type(sock_err->ee_type, sock_err->ee_code);
         else debug_print("non-icmp err msg\n");

         struct sockaddr *sa = SO_EE_OFFENDER(sock_err);
         debug_print("%s\n", inet_ntoa(((struct sockaddr_in *)sa)->sin_addr));

         /* re-build icmp msg */
	      struct ip_header* ipheader;
	      struct icmp_msg* icmp;
         char *pkt;
         int pkt_len = sizeof(struct ip_header) + sizeof(struct icmp_msg);
	      if ( (pkt = calloc(1, pkt_len)) == NULL)
		      die("Could not allocate memory for packet\n");
	      ipheader = (struct ip_header*)pkt;
	      icmp = (struct icmp_msg*)(pkt+sizeof(struct ip_header));

         /* fill packet */
	      ipheader->ver 		= 4; 	
	      ipheader->hl		= 5; 	
	      ipheader->tos		= 0;
	      ipheader->totl		= pkt_len;
	      ipheader->id		= 0;
	      ipheader->notused	= 0;	
	      ipheader->ttl		= 255;  
	      ipheader->prot		= 1;	
	      ipheader->csum		= 0;
	      ipheader->saddr 	= ((struct sockaddr_in *)sa)->sin_addr.s_addr;
	      ipheader->daddr   = (unsigned long)inet_addr(state->private_addr);
	      icmp->type		   = sock_err->ee_type;		
	      icmp->code		   = sock_err->ee_code;		
		   icmp->checksum    = 0;
         for (i=0; i<8; i++)          
            icmp->data[i]  = ((unsigned char *) iov.iov_base)[i];
		   icmp->checksum    = calcsum((unsigned short*)icmp, sizeof(struct icmp_msg));
	      ipheader->csum		= calcsum((unsigned short*)ipheader, sizeof(struct ip_header));

         int sent = xwrite(fd_out, pkt, pkt_len);
         free(pkt);
      } 
   }
   return 0;
}

int xrecv(int fd, void *buf, size_t buflen) {
   int recvd = 0;
   if ((recvd = recvfrom(fd, buf, buflen, 0, NULL, 0)) < 0) {
      debug_print("%s\n",strerror(errno));
      return -1;
   }
   return recvd;
}

int xrecvfrom(int fd, struct sockaddr *sa, unsigned int *salen, void *buf, size_t buflen) {
   int recvd = 0;
   if ((recvd = recvfrom(fd, buf, buflen, 0, sa, salen)) < 0) {
      debug_print("%s\n",strerror(errno));
      return -1;
   }
   return recvd;
}

void die(char *s) {
    perror(s);
    exit(1);
}

int xread(int fd, char *buf, int buflen) {
   int nread;
   if((nread=read(fd, buf, buflen)) < 0) 
      die("read");
   return nread;
}

int xwrite(int fd, char *buf, int buflen) {
   int nwrite;
   if((nwrite=write(fd, buf, buflen)) < 0) 
      die("write");
   return nwrite;
}

int xfwrite(FILE *fp, char *buf, int size, int nmemb) {
   int wsize = fwrite(buf, size, nmemb, fp); 
   if(wsize < nmemb) 
      die("fwrite");
   return wsize;
}

unsigned short calcsum(unsigned short *buffer, int length) {
	unsigned long sum; 	
	for (sum=0; length>1; length-=2) 
		sum += *buffer++;	

	if (length==1)
		sum += (char)*buffer;

	sum = (sum >> 16) + (sum & 0xFFFF); 
	sum += (sum >> 16);		   
	return ~sum;
}

void build_sel(fd_set *input_set, int *fds_raw, int len, int *max_fd_raw) {
   int i = 0, max_fd = 0, fd = 0;
   FD_ZERO(input_set);
   for (;i<len;i++) {
      fd = fds_raw[i];
      if (fd) {
       FD_SET(fd, input_set);
       max_fd = max(fd,max_fd);
      } else break;
   }

   *max_fd_raw = max_fd;
}

