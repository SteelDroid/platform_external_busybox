/* vi: set sw=4 ts=4: */
/*
 * iplink.c "ip link".
 *
 * Authors: Alexey Kuznetsov, <kuznet@ms2.inr.ac.ru>
 *
 * Licensed under GPLv2 or later, see file LICENSE in this tarball for details.
 */

#include "libbb.h"

#include <sys/ioctl.h>
#include <sys/socket.h>

#include <net/if.h>
#include <net/if_packet.h>
#include <netpacket/packet.h>

#include <net/ethernet.h>

#include "rt_names.h"
#include "utils.h"
#include "ip_common.h"

/* taken from linux/sockios.h */
#define SIOCSIFNAME	0x8923		/* set interface name */

static void on_off(const char *msg) ATTRIBUTE_NORETURN;
static void on_off(const char *msg)
{
	bb_error_msg_and_die("error: argument of \"%s\" must be \"on\" or \"off\"", msg);
}

/* Exits on error */
static int get_ctl_fd(void)
{
	int fd;

	fd = socket(PF_INET, SOCK_DGRAM, 0);
	if (fd >= 0)
		return fd;
	fd = socket(PF_PACKET, SOCK_DGRAM, 0);
	if (fd >= 0)
		return fd;
	fd = socket(PF_INET6, SOCK_DGRAM, 0);
	if (fd >= 0)
		return fd;
	bb_perror_msg_and_die("cannot create control socket");
}

/* Exits on error */
static void do_chflags(char *dev, uint32_t flags, uint32_t mask)
{
	struct ifreq ifr;
	int fd;

	strncpy(ifr.ifr_name, dev, sizeof(ifr.ifr_name));
	fd = get_ctl_fd();
	if (ioctl(fd, SIOCGIFFLAGS, &ifr)) {
		bb_perror_msg_and_die("SIOCGIFFLAGS");
	}
	if ((ifr.ifr_flags ^ flags) & mask) {
		ifr.ifr_flags &= ~mask;
		ifr.ifr_flags |= mask & flags;
		if (ioctl(fd, SIOCSIFFLAGS, &ifr))
			bb_perror_msg_and_die("SIOCSIFFLAGS");
	}
	close(fd);
}

/* Exits on error */
static void do_changename(char *dev, char *newdev)
{
	struct ifreq ifr;
	int fd;
	int err;

	strncpy(ifr.ifr_name, dev, sizeof(ifr.ifr_name));
	strncpy(ifr.ifr_newname, newdev, sizeof(ifr.ifr_newname));
	fd = get_ctl_fd();
	err = ioctl(fd, SIOCSIFNAME, &ifr);
	if (err) {
		bb_perror_msg_and_die("SIOCSIFNAME");
	}
	close(fd);
}

/* Exits on error */
static void set_qlen(char *dev, int qlen)
{
	struct ifreq ifr;
	int s;

	s = get_ctl_fd();
	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, dev, sizeof(ifr.ifr_name));
	ifr.ifr_qlen = qlen;
	if (ioctl(s, SIOCSIFTXQLEN, &ifr) < 0) {
		bb_perror_msg_and_die("SIOCSIFXQLEN");
	}
	close(s);
}

/* Exits on error */
static void set_mtu(char *dev, int mtu)
{
	struct ifreq ifr;
	int s;

	s = get_ctl_fd();
	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, dev, sizeof(ifr.ifr_name));
	ifr.ifr_mtu = mtu;
	if (ioctl(s, SIOCSIFMTU, &ifr) < 0) {
		bb_perror_msg_and_die("SIOCSIFMTU");
	}
	close(s);
}

/* Exits on error */
static int get_address(char *dev, int *htype)
{
	struct ifreq ifr;
	struct sockaddr_ll me;
	socklen_t alen;
	int s;

	s = socket(PF_PACKET, SOCK_DGRAM, 0);
	if (s < 0) {
		bb_perror_msg_and_die("socket(PF_PACKET)");
	}

	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, dev, sizeof(ifr.ifr_name));
	if (ioctl(s, SIOCGIFINDEX, &ifr) < 0) {
		bb_perror_msg_and_die("SIOCGIFINDEX");
	}

	memset(&me, 0, sizeof(me));
	me.sll_family = AF_PACKET;
	me.sll_ifindex = ifr.ifr_ifindex;
	me.sll_protocol = htons(ETH_P_LOOP);
	if (bind(s, (struct sockaddr*)&me, sizeof(me)) == -1) {
		bb_perror_msg_and_die("bind");
	}

	alen = sizeof(me);
	if (getsockname(s, (struct sockaddr*)&me, &alen) == -1) {
		bb_perror_msg_and_die("getsockname");
	}
	close(s);
	*htype = me.sll_hatype;
	return me.sll_halen;
}

/* Exits on error */
static void parse_address(char *dev, int hatype, int halen, char *lla, struct ifreq *ifr)
{
	int alen;

	memset(ifr, 0, sizeof(*ifr));
	strncpy(ifr->ifr_name, dev, sizeof(ifr->ifr_name));
	ifr->ifr_hwaddr.sa_family = hatype;
	alen = ll_addr_a2n((unsigned char *)(ifr->ifr_hwaddr.sa_data), 14, lla);
	if (alen < 0)
		exit(1);
	if (alen != halen) {
		bb_error_msg_and_die("wrong address (%s) length: expected %d bytes", lla, halen);
	}
}

/* Exits on error */
static void set_address(struct ifreq *ifr, int brd)
{
	int s;

	s = get_ctl_fd();
	if (ioctl(s, brd ? SIOCSIFHWBROADCAST  :SIOCSIFHWADDR, ifr) < 0) {
		bb_perror_msg_and_die(brd ? "SIOCSIFHWBROADCAST" : "SIOCSIFHWADDR");
	}
	close(s);
}


/* Return value becomes exitcode. It's okay to not return at all */
static int do_set(int argc, char **argv)
{
	char *dev = NULL;
	uint32_t mask = 0;
	uint32_t flags = 0;
	int qlen = -1;
	int mtu = -1;
	char *newaddr = NULL;
	char *newbrd = NULL;
	struct ifreq ifr0, ifr1;
	char *newname = NULL;
	int htype, halen;

	while (argc > 0) {
		if (strcmp(*argv, "up") == 0) {
			mask |= IFF_UP;
			flags |= IFF_UP;
		} else if (strcmp(*argv, "down") == 0) {
			mask |= IFF_UP;
			flags &= ~IFF_UP;
		} else if (strcmp(*argv, "name") == 0) {
			NEXT_ARG();
			newname = *argv;
		} else if (strcmp(*argv, "mtu") == 0) {
			NEXT_ARG();
			if (mtu != -1)
				duparg("mtu", *argv);
			if (get_integer(&mtu, *argv, 0))
				invarg(*argv, "mtu");
		} else if (strcmp(*argv, "multicast") == 0) {
			NEXT_ARG();
			mask |= IFF_MULTICAST;
			if (strcmp(*argv, "on") == 0) {
				flags |= IFF_MULTICAST;
			} else if (strcmp(*argv, "off") == 0) {
				flags &= ~IFF_MULTICAST;
			} else
				on_off("multicast");
		} else if (strcmp(*argv, "arp") == 0) {
			NEXT_ARG();
			mask |= IFF_NOARP;
			if (strcmp(*argv, "on") == 0) {
				flags &= ~IFF_NOARP;
			} else if (strcmp(*argv, "off") == 0) {
				flags |= IFF_NOARP;
			} else
				on_off("noarp");
		} else if (strcmp(*argv, "addr") == 0) {
			NEXT_ARG();
			newaddr = *argv;
		} else {
			if (strcmp(*argv, "dev") == 0) {
				NEXT_ARG();
			}
			if (dev)
				duparg2("dev", *argv);
			dev = *argv;
		}
		argc--;
		argv++;
	}

	if (!dev) {
		bb_error_msg_and_die(bb_msg_requires_arg, "\"dev\"");
	}

	if (newaddr || newbrd) {
		halen = get_address(dev, &htype);
		if (newaddr) {
			parse_address(dev, htype, halen, newaddr, &ifr0);
		}
		if (newbrd) {
			parse_address(dev, htype, halen, newbrd, &ifr1);
		}
	}

	if (newname && strcmp(dev, newname)) {
		do_changename(dev, newname);
		dev = newname;
	}
	if (qlen != -1) {
		set_qlen(dev, qlen);
	}
	if (mtu != -1) {
		set_mtu(dev, mtu);
	}
	if (newaddr || newbrd) {
		if (newbrd) {
			set_address(&ifr1, 1);
		}
		if (newaddr) {
			set_address(&ifr0, 0);
		}
	}
	if (mask)
		do_chflags(dev, flags, mask);
	return 0;
}

static int ipaddr_list_link(int argc, char **argv)
{
	preferred_family = AF_PACKET;
	return ipaddr_list_or_flush(argc, argv, 0);
}

/* Return value becomes exitcode. It's okay to not return at all */
int do_iplink(int argc, char **argv)
{
	if (argc <= 0)
		return ipaddr_list_link(0, NULL);

	if (matches(*argv, "set") == 0)
		return do_set(argc-1, argv+1);

	if (matches(*argv, "show") == 0 ||
	    matches(*argv, "lst") == 0 ||
	    matches(*argv, "list") == 0)
		return ipaddr_list_link(argc-1, argv+1);

	bb_error_msg_and_die("command \"%s\" is unknown", *argv);
}
