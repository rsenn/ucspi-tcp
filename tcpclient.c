#include <sys/types.h>
#include <sys/param.h>
#include <netdb.h>
#include "sig.h"
#include "exit.h"
#include "sgetopt.h"
#include "uint16.h"
#include "fmt.h"
#include "scan.h"
#include "str.h"
#include "ip4.h"
#include "ip6.h"
#include "uint16.h"
#include "socket.h"
#include "fd.h"
#include "stralloc.h"
#include "buffer.h"
#include "error.h"
#include "strerr.h"
#include "pathexec.h"
#include "timeoutconn.h"
#include "remoteinfo.h"
#include "dns.h"
#include "byte.h"

#define FATAL "tcpclient: fatal: "
#define CONNECT "tcpclient: unable to connect to "

void nomem(void)
{
  strerr_die2x(111,FATAL,"out of memory");
}
void usage(void)
{
  strerr_die1x(100,"tcpclient: usage: tcpclient \
[ -46hHrRdDqQv ] \
[ -i localip ] \
[ -p localport ] \
[ -T timeoutconn ] \
[ -l localname ] \
[ -t timeoutinfo ] \
[ -I interface ] \
host port program");
}

int forcev6 = 0;
int verbosity = 1;
int flagdelay = 1;
int flagremoteinfo = 1;
int flagremotehost = 1;
unsigned long itimeout = 26;
unsigned long ctimeout[2] = { 2, 58 };
uint32 netif = 0;

char iplocal[16] = { 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0 };
uint16 portlocal = 0;
char *forcelocal = 0;

char ipremote[16];
uint16 portremote;

char *hostname;
static stralloc addresses;
static stralloc moreaddresses;

static stralloc tmp;
static stralloc fqdn;
char strnum[FMT_ULONG];
char ipstr[IP6_FMT];

char seed[128];

main(int argc,char **argv)
{
  int fakev4=0;
  unsigned long u;
  int opt;
  char *x;
  int j;
  int s;
  int cloop;

  dns_random_init(seed);

  close(6);
  close(7);
  sig_ignore(sig_pipe);
 
  while ((opt = getopt(argc,argv,"46dDvqQhHrRi:p:t:T:l:I:")) != opteof)
    switch(opt) {
      case '4': noipv6 = 1; break;
      case '6': forcev6 = 1; break;
      case 'd': flagdelay = 1; break;
      case 'D': flagdelay = 0; break;
      case 'v': verbosity = 2; break;
      case 'q': verbosity = 0; break;
      case 'Q': verbosity = 1; break;
      case 'l': forcelocal = optarg; break;
      case 'H': flagremotehost = 0; break;
      case 'h': flagremotehost = 1; break;
      case 'R': flagremoteinfo = 0; break;
      case 'r': flagremoteinfo = 1; break;
      case 't': scan_ulong(optarg,&itimeout); break;
      case 'T': j = scan_ulong(optarg,&ctimeout[0]);
		if (optarg[j] == '+') ++j;
		scan_ulong(optarg + j,&ctimeout[1]);
		break;
      case 'i': if (!scan_ip6(optarg,iplocal)) usage(); break;
      case 'I': netif=socket_getifidx(optarg); break;
      case 'p': scan_ulong(optarg,&u); portlocal = u; break;
      default: usage();
    }
  argv += optind;

  if (!verbosity)
    buffer_2->fd = -1;

  hostname = *argv;
  if (!hostname) usage();
  if (!hostname[0] || str_equal(hostname,"0"))
    hostname = (noipv6?"127.0.0.1":"::1");

  x = *++argv;
  if (!x) usage();
  if (!x[scan_ulong(x,&u)])
    portremote = u;
  else {
    struct servent *se;
    se = getservbyname(x,"tcp");
    if (!se)
      strerr_die3x(111,FATAL,"unable to figure out port number for ",x);
    portremote = ntohs(se->s_port);
    /* i continue to be amazed at the stupidity of the s_port interface */
  }

  if (!*++argv) usage();

  if (!stralloc_copys(&tmp,hostname)) nomem();
  if (dns_ip6_qualify(&addresses,&fqdn,&tmp) == -1)
    strerr_die4sys(111,FATAL,"temporarily unable to figure out IP address for ",hostname,": ");
  if (addresses.len < 16)
    strerr_die3x(111,FATAL,"no IP address for ",hostname);

  if (addresses.len == 16) {
    ctimeout[0] += ctimeout[1];
    ctimeout[1] = 0;
  }

  for (cloop = 0;cloop < 2;++cloop) {
    if (!stralloc_copys(&moreaddresses,"")) nomem();
    for (j = 0;j + 16 <= addresses.len;j += 4) {
      s = socket_tcp6();
      if (s == -1)
        strerr_die2sys(111,FATAL,"unable to create socket: ");
      if (socket_bind6(s,iplocal,portlocal,netif) == -1)
        strerr_die2sys(111,FATAL,"unable to bind socket: ");
      if (timeoutconn6(s,addresses.s + j,portremote,ctimeout[cloop],netif) == 0)
        goto CONNECTED;
      close(s);
      if (!cloop && ctimeout[1] && (errno == error_timeout)) {
	if (!stralloc_catb(&moreaddresses,addresses.s + j,16)) nomem();
      }
      else {
        strnum[fmt_ulong(strnum,portremote)] = 0;
	if (ip6_isv4mapped(addresses.s+j))
	  ipstr[ip4_fmt(ipstr,addresses.s + j + 12)] = 0;
	else
	  ipstr[ip6_fmt(ipstr,addresses.s + j)] = 0;
        strerr_warn5(CONNECT,ipstr," port ",strnum,": ",&strerr_sys);
      }
    }
    if (!stralloc_copy(&addresses,&moreaddresses)) nomem();
  }

  _exit(111);



  CONNECTED:

  if (!flagdelay)
    socket_tcpnodelay(s); /* if it fails, bummer */

  if (socket_local6(s,iplocal,&portlocal,&netif) == -1)
    strerr_die2sys(111,FATAL,"unable to get local address: ");

  if (!forcev6 && (ip6_isv4mapped(iplocal) || byte_equal(iplocal,16,V6any)))
    fakev4=1;

  if (!pathexec_env("PROTO",fakev4?"TCP":"TCP6")) nomem();

  strnum[fmt_ulong(strnum,portlocal)] = 0;
  if (!pathexec_env("TCPLOCALPORT",strnum)) nomem();
  if (fakev4)
    ipstr[ip4_fmt(ipstr,iplocal+12)] = 0;
  else
    ipstr[ip6_fmt(ipstr,iplocal)] = 0;
  if (!pathexec_env("TCPLOCALIP",ipstr)) nomem();

  x = forcelocal;
  if (!x)
    if (dns_name6(&tmp,iplocal) == 0) {
      if (!stralloc_0(&tmp)) nomem();
      x = tmp.s;
    }
  if (!pathexec_env("TCPLOCALHOST",x)) nomem();

  if (socket_remote6(s,ipremote,&portremote,&netif) == -1)
    strerr_die2sys(111,FATAL,"unable to get remote address: ");

  strnum[fmt_ulong(strnum,portremote)] = 0;
  if (!pathexec_env("TCPREMOTEPORT",strnum)) nomem();
  if (fakev4)
    ipstr[ip4_fmt(ipstr,ipremote+12)] = 0;
  else
    ipstr[ip6_fmt(ipstr,ipremote)] = 0;
  if (!pathexec_env("TCPREMOTEIP",ipstr)) nomem();
  if (verbosity >= 2)
    strerr_warn4("tcpclient: connected to ",ipstr," port ",strnum,0);

  x = 0;
  if (flagremotehost)
    if (dns_name6(&tmp,ipremote) == 0) {
      if (!stralloc_0(&tmp)) nomem();
      x = tmp.s;
    }
  if (!pathexec_env("TCPREMOTEHOST",x)) nomem();

  x = 0;
  if (flagremoteinfo)
    if (remoteinfo6(&tmp,ipremote,portremote,iplocal,portlocal,itimeout,netif) == 0) {
      if (!stralloc_0(&tmp)) nomem();
      x = tmp.s;
    }
  if (!pathexec_env("TCPREMOTEINFO",x)) nomem();

  if (fd_move(6,s) == -1)
    strerr_die2sys(111,FATAL,"unable to set up descriptor 6: ");
  if (fd_copy(7,6) == -1)
    strerr_die2sys(111,FATAL,"unable to set up descriptor 7: ");
  sig_uncatch(sig_pipe);
 
  pathexec(argv);
  strerr_die4sys(111,FATAL,"unable to run ",*argv,": ");
}
