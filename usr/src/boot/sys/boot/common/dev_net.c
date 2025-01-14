/*
 * Copyright (c) 1997 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Gordon W. Ross.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Copyright 2018 OmniOS Community Edition (OmniOSce) Association.
 */

#include <sys/cdefs.h>

/*
 * This module implements a "raw device" interface suitable for
 * use by the stand-alone I/O library NFS code.  This interface
 * does not support any "block" access, and exists only for the
 * purpose of initializing the network interface, getting boot
 * parameters, and performing the NFS mount.
 *
 * At open time, this does:
 *
 * find interface      - netif_open()
 * RARP for IP address - rarp_getipaddress()
 * RPC/bootparams      - callrpc(d, RPC_BOOTPARAMS, ...)
 * RPC/mountd          - nfs_mount(sock, ip, path)
 *
 * the root file handle from mountd is saved in a global
 * for use by the NFS open code (NFS/lookup).
 */

#include <machine/stdarg.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/in_systm.h>

#include <stand.h>
#include <stddef.h>
#include <string.h>
#include <net.h>
#include <netif.h>
#include <bootp.h>
#include <bootparam.h>

#include "dev_net.h"
#include "bootstrap.h"

#ifdef	NETIF_DEBUG
int debug = 0;
#endif

static char *netdev_name;
static int netdev_sock = -1;
static int netdev_opens;

static int	net_init(void);
static int	net_open(struct open_file *, ...);
static int	net_close(struct open_file *);
static void	net_cleanup(void);
static int	net_strategy(void *, int, daddr_t, size_t, char *, size_t *);
static int	net_print(int);

static int net_getparams(int sock);

struct devsw netdev = {
	"net",
	DEVT_NET,
	net_init,
	net_strategy,
	net_open,
	net_close,
	noioctl,
	net_print,
	net_cleanup
};

static struct uri_scheme {
	const char *scheme;
	int proto;
} uri_schemes[] = {
	{ "tftp:/", NET_TFTP },
	{ "nfs:/", NET_NFS },
};

static int
net_init(void)
{

	return (0);
}

/*
 * Called by devopen after it sets f->f_dev to our devsw entry.
 * This opens the low-level device and sets f->f_devdata.
 * This is declared with variable arguments...
 */
static int
net_open(struct open_file *f, ...)
{
	struct iodesc *d;
	va_list args;
	struct devdesc *dev;
	const char *devname;	/* Device part of file name (or NULL). */
	int error = 0;

	va_start(args, f);
	dev = va_arg(args, struct devdesc *);
	va_end(args);

	devname = dev->d_dev->dv_name;
	/* Before opening another interface, close the previous one first. */
	if (netdev_sock >= 0 && strcmp(devname, netdev_name) != 0)
		net_cleanup();

	/* On first open, do netif open, mount, etc. */
	if (netdev_opens == 0) {
		/* Find network interface. */
		if (netdev_sock < 0) {
			netdev_sock = netif_open(dev);
			if (netdev_sock < 0) {
				printf("%s: netif_open() failed\n", __func__);
				return (ENXIO);
			}
			netdev_name = strdup(devname);
#ifdef	NETIF_DEBUG
			if (debug)
				printf("%s: netif_open() succeeded\n",
				    __func__);
#endif
		}
		/*
		 * If network params were not set by netif_open(), try to get
		 * them via bootp, rarp, etc.
		 */
		if (rootip.s_addr == 0) {
			/* Get root IP address, and path, etc. */
			error = net_getparams(netdev_sock);
			if (error) {
				/* getparams makes its own noise */
				free(netdev_name);
				netif_close(netdev_sock);
				netdev_sock = -1;
				return (error);
			}
		}
		/*
		 * Set the variables required by the kernel's nfs_diskless
		 * mechanism.  This is the minimum set of variables required to
		 * mount a root filesystem without needing to obtain additional
		 * info from bootp or other sources.
		 */
		d = socktodesc(netdev_sock);
		setenv("boot.netif.hwaddr", ether_sprintf(d->myea), 1);
		setenv("boot.netif.ip", inet_ntoa(myip), 1);
		setenv("boot.netif.netmask", intoa(netmask), 1);
		setenv("boot.netif.gateway", inet_ntoa(gateip), 1);
		setenv("boot.netif.server", inet_ntoa(rootip), 1);
		if (netproto == NET_TFTP) {
			setenv("boot.tftproot.server", inet_ntoa(rootip), 1);
			setenv("boot.tftproot.path", rootpath, 1);
		} else {
			setenv("boot.nfsroot.server", inet_ntoa(rootip), 1);
			setenv("boot.nfsroot.path", rootpath, 1);
		}
		if (intf_mtu != 0) {
			char mtu[16];
			snprintf(mtu, sizeof (mtu), "%u", intf_mtu);
			setenv("boot.netif.mtu", mtu, 1);
		}
	}
	netdev_opens++;
	f->f_devdata = &netdev_sock;
	return (error);
}

static int
net_close(struct open_file *f)
{

#ifdef	NETIF_DEBUG
	if (debug)
		printf("%s: opens=%d\n", __func__, netdev_opens);
#endif

	f->f_devdata = NULL;

	return (0);
}

static void
net_cleanup(void)
{

	if (netdev_sock >= 0) {
#ifdef	NETIF_DEBUG
		if (debug)
			printf("%s: calling netif_close()\n", __func__);
#endif
		rootip.s_addr = 0;
		free(netdev_name);
		netif_close(netdev_sock);
		netdev_sock = -1;
	}
}

static int
net_strategy(void *devdata __unused, int rw __unused, daddr_t blk __unused,
    size_t size __unused, char *buf __unused, size_t *rsize __unused)
{

	return (EIO);
}

/*
 * Get info for NFS boot: our IP address, our hostname,
 * server IP address, and our root path on the server.
 * There are two ways to do this:  The old, Sun way,
 * and the more modern, BOOTP/DHCP way. (RFC951, RFC1048)
 */

extern n_long ip_convertaddr(char *p);

static int
net_getparams(int sock)
{
	char buf[MAXHOSTNAMELEN];
	n_long rootaddr, smask;

	/*
	 * Try to get boot info using BOOTP/DHCP.  If we succeed, then
	 * the server IP address, gateway, and root path will all
	 * be initialized.  If any remain uninitialized, we will
	 * use RARP and RPC/bootparam (the Sun way) to get them.
	 */
	bootp(sock);
	if (myip.s_addr != 0)
		goto exit;
#ifdef	NETIF_DEBUG
	if (debug)
		printf("%s: BOOTP failed, trying RARP/RPC...\n", __func__);
#endif

	/*
	 * Use RARP to get our IP address.  This also sets our
	 * netmask to the "natural" default for our address.
	 */
	if (rarp_getipaddress(sock)) {
		printf("%s: RARP failed\n", __func__);
		return (EIO);
	}
	printf("%s: client addr: %s\n", __func__, inet_ntoa(myip));

	/* Get our hostname, server IP address, gateway. */
	if (bp_whoami(sock)) {
		printf("%s: bootparam/whoami RPC failed\n", __func__);
		return (EIO);
	}
#ifdef	NETIF_DEBUG
	if (debug)
		printf("%s: client name: %s\n", __func__, hostname);
#endif

	/*
	 * Ignore the gateway from whoami (unreliable).
	 * Use the "gateway" parameter instead.
	 */
	smask = 0;
	gateip.s_addr = 0;
	if (bp_getfile(sock, "gateway", &gateip, buf) == 0) {
		/* Got it!  Parse the netmask. */
		smask = ip_convertaddr(buf);
	}
	if (smask) {
		netmask = smask;
#ifdef	NETIF_DEBUG
		if (debug)
			printf("%s: subnet mask: %s\n", __func__,
			    intoa(netmask));
#endif
	}
#ifdef	NETIF_DEBUG
	if (gateip.s_addr && debug)
		printf("%s: net gateway: %s\n", __func__, inet_ntoa(gateip));
#endif

	/* Get the root server and pathname. */
	if (bp_getfile(sock, "root", &rootip, rootpath)) {
		printf("%s: bootparam/getfile RPC failed\n", __func__);
		return (EIO);
	}
exit:
	if ((rootaddr = net_parse_rootpath()) != INADDR_NONE)
		rootip.s_addr = rootaddr;

#ifdef	NETIF_DEBUG
	if (debug) {
		printf("%s: server addr: %s\n", __func__,
		    inet_ntoa(rootip));
		printf("%s: server path: %s\n", __func__, rootpath);
	}
#endif

	return (0);
}

static int
net_print(int verbose)
{
	struct netif_driver *drv;
	int i, d, cnt;
	int ret = 0;

	if (netif_drivers[0] == NULL)
		return (ret);

	printf("%s devices:", netdev.dv_name);
	if ((ret = pager_output("\n")) != 0)
		return (ret);

	cnt = 0;
	for (d = 0; netif_drivers[d]; d++) {
		drv = netif_drivers[d];
		for (i = 0; i < drv->netif_nifs; i++) {
			printf("\t%s%d:", netdev.dv_name, cnt++);
			if (verbose) {
				printf(" (%s%d)", drv->netif_bname,
				    drv->netif_ifs[i].dif_unit);
			}
			if ((ret = pager_output("\n")) != 0)
				return (ret);
		}
	}
	return (ret);
}

/*
 * Parses the rootpath if present
 *
 * The rootpath format can be in the form
 * <scheme>://IPv4/path
 * <scheme>:/path
 *
 * For compatibility with previous behaviour it also accepts as an NFS scheme
 * IPv4:/path
 * /path
 *
 * If an IPv4 address has been specified, it will be stripped out and passed
 * out as the return value of this function in network byte order.
 *
 * If no rootpath is present then we will default to TFTP.
 *
 * If no global default scheme has been specified and no scheme has been
 * specified, we will assume that this is an NFS URL.
 *
 * The pathname will be stored in the global variable rootpath.
 */
uint32_t
net_parse_rootpath(void)
{
	n_long addr = htonl(INADDR_NONE);
	size_t i;
	char ip[FNAME_SIZE];
	char *ptr, *val;

	netproto = NET_NONE;

	for (i = 0; i < nitems(uri_schemes); i++) {
		if (strncmp(rootpath, uri_schemes[i].scheme,
		    strlen(uri_schemes[i].scheme)) != 0)
			continue;

		netproto = uri_schemes[i].proto;
		break;
	}
	ptr = rootpath;
	/* Fallback for compatibility mode */
	if (netproto == NET_NONE) {
		if (strcmp(rootpath, "/") == 0) {
			netproto = NET_TFTP;
		} else {
			netproto = NET_NFS;
			(void) strsep(&ptr, ":");
			if (ptr != NULL) {
				addr = inet_addr(rootpath);
				bcopy(ptr, rootpath, strlen(ptr) + 1);
			}
		}
	} else {
		ptr += strlen(uri_schemes[i].scheme);
		if (*ptr == '/') {
			/*
			 * We are in the form <scheme>://, we do expect an ip.
			 */
			ptr++;
			/*
			 * XXX when http will be there we will need to check for
			 * a port, but right now we do not need it yet.
			 * Also will need rework for IPv6.
			 */
			val = strchr(ptr, '/');
			if (val == NULL) {
				/* If no pathname component, default to / */
				strlcat(rootpath, "/", sizeof (rootpath));
				val = strchr(ptr, '/');
			}
			if (val != NULL) {
				snprintf(ip, sizeof (ip), "%.*s",
				    (int)((uintptr_t)val - (uintptr_t)ptr),
				    ptr);
				addr = inet_addr(ip);
				if (addr == htonl(INADDR_NONE)) {
					printf("Bad IP address: %s\n", ip);
				}
				bcopy(val, rootpath, strlen(val) + 1);
			}
		} else {
			ptr--;
			bcopy(ptr, rootpath, strlen(ptr) + 1);
		}
	}

	return (addr);
}
