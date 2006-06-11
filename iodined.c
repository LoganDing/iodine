/*
 * Copyright (c) 2006 Bjorn Andersson <flex@kryo.se>, Erik Ekman <yarrick@kryo.se>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <err.h>
#include <pwd.h>
#include <arpa/inet.h>
#include <zlib.h>

#include "tun.h"
#include "dns.h"

#ifndef MAX
#define MAX(a,b) ((a)>(b)?(a):(b))
#endif

int running = 1;

static void
sigint(int sig) {
	running = 0;
}

static int
tunnel(int tun_fd, int dns_fd)
{
	int i;
	int read;
	fd_set fds;
	struct timeval tv;
	char in[64*1024];
	long outlen;
	char out[64*1024];
	
	while (running) {
		if (dnsd_hasack()) {
			tv.tv_sec = 0;
			tv.tv_usec = 50000;
		} else {
			tv.tv_sec = 1;
			tv.tv_usec = 0;
		}

		FD_ZERO(&fds);
		if(!dnsd_haspacket()) 
			FD_SET(tun_fd, &fds);
		FD_SET(dns_fd, &fds);

		i = select(MAX(tun_fd, dns_fd) + 1, &fds, NULL, NULL, &tv);
		
		if(i < 0) {
			if (running) 
				warn("select");
			return 1;
		}
	
		if (i==0) {	
			if (dnsd_hasack()) 
				dnsd_forceack(dns_fd);
		} else {
			if(FD_ISSET(tun_fd, &fds)) {
				read = read_tun(tun_fd, in, sizeof(in));
				if (read <= 0)
					continue;
				
				outlen = sizeof(out);
				compress2(out, &outlen, in, read, 9);
				dnsd_queuepacket(out, outlen);
			}
			if(FD_ISSET(dns_fd, &fds)) {
				read = dnsd_read(dns_fd, in, sizeof(in));
				if (read <= 0) {
					if (read == GOTHELLO) {
						read = snprintf(in, sizeof(in), "%s-%d", "172.30.5.2", 1023);
						dnsd_queuepacket(in, read);
					}
					continue;
				}

				outlen = sizeof(out);
				uncompress(out, &outlen, in, read);

				write_tun(tun_fd, out, outlen);
			} 
		}
	}

	return 0;
}

extern char *__progname;

static void
usage() {
	printf("Usage: %s [-v] [-h] [-f] [-u user] [-t chrootdir] [-m mtu] "
			"tunnel_ip topdomain\n", __progname);
	exit(2);
}

static void
help() {
	printf("iodine IP over DNS tunneling server\n");
	printf("Usage: %s [-v] [-h] [-f] [-u user] [-t chrootdir] [-m mtu] "
		   "tunnel_ip topdomain\n", __progname);
	printf("  -f to keep running in foreground\n");
	printf("  -u name to drop privileges and run as user 'name'\n");
	printf("  -t dir to chroot to directory dir\n");
	printf("tunnel_ip is the IP number of the local tunnel interface.\n");
	printf("topdomain is the FQDN that is delegated to this server.\n");
	exit(0);
}

static void
version() {
	char *svnver = "$Rev$ from $Date$";
	printf("iodine IP over DNS tunneling server\n");
	printf("SVN version: %s\n", svnver);
	exit(0);
}

int
main(int argc, char **argv)
{
	int choice;
	int tun_fd;
	int dnsd_fd;
	char *newroot;
	char *username;
	int foreground;
	int mtu;
	struct passwd *pw;

	username = NULL;
	newroot = NULL;
	foreground = 0;
	mtu = 1024;
	
	while ((choice = getopt(argc, argv, "vfhu:t:m:")) != -1) {
		switch(choice) {
		case 'v':
			version();
			break;
		case 'f':
			foreground = 1;
			break;
		case 'h':
			help();
			break;
		case 'u':
			username = optarg;
			break;
		case 't':
			newroot = optarg;
			break;
		case 'm':
			mtu = atoi(optarg);
			break;
		default:
			usage();
			break;
		}
	}

	argc -= optind;
	argv += optind;
	
	if (geteuid() != 0) {
		printf("Run as root and you'll be happy.\n");
		usage();
	}

	if (argc != 2) 
		usage();

	if (username) {
		pw = getpwnam(username);
		if (!pw) {
			printf("User %s does not exist!\n", username);
			usage();
		}
	}

	if (mtu == 0) {
		printf("Bad MTU given.\n");
		usage();
	}

	if ((tun_fd = open_tun()) == -1)
		goto cleanup0;
	if (tun_setip(argv[0]) != 0 || tun_setmtu(mtu) != 0)
		goto cleanup1;
	if ((dnsd_fd = open_dnsd(argv[1])) == -1) 
		goto cleanup2;

	if (newroot) {
		if (chroot(newroot) != 0 || chdir("/") != 0)
			err(1, "%s", newroot);
		seteuid(geteuid());
		setuid(getuid());
	}
	
	if (!foreground) {
		daemon(0, 0);
		umask(0);
		alarm(0);
	}

	signal(SIGINT, sigint);
	if (username) {
		if (setgid(pw->pw_gid) < 0 || setuid(pw->pw_uid) < 0) {
			printf("Could not switch to user %s!\n", username);
			usage();
		}
	}
	
	tunnel(tun_fd, dnsd_fd);

cleanup2:
	close_dnsd(dnsd_fd);
cleanup1:
	close_tun(tun_fd);	
cleanup0:

	return 0;
}
