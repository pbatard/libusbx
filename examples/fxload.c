/*
 * Copyright © 2001 Stephen Williams (steve@icarus.com)
 * Copyright © 2001-2002 David Brownell (dbrownell@users.sourceforge.net)
 * Copyright © 2008 Roger Williams (rawqux@users.sourceforge.net)
 * Copyright © 2012 Pete Batard (pete@akeo.ie)
 *
 *    This source code is free software; you can redistribute it
 *    and/or modify it in source code form under the terms of the GNU
 *    General Public License as published by the Free Software
 *    Foundation; either version 2 of the License, or (at your option)
 *    any later version.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with this program; if not, write to the Free Software
 *    Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA
 */

/*
 * This program supports loading firmware into a target USB device
 * that is discovered and referenced by the hotplug usb agent. It can
 * also do other useful things, like set the permissions of the device
 * and create a symbolic link for the benefit of applications that are
 * looking for the device.
 *
 *     -I <path>       -- Download this firmware (intel hex)
 *     -t <type>       -- uController type: an21, fx, fx2, fx2lp
 *     -s <path>       -- use this second stage loader
 *     -c <byte>       -- Download to EEPROM, with this config byte
 *
 *     -D <vid:pid>    -- Use this device, instead of $DEVICE
 *
 *     -V              -- Print version ID for program
 *
 * This program is intended to be started by hotplug scripts in
 * response to a device appearing on the bus. It therefore also
 * expects these environment variables which are passed by hotplug to
 * its sub-scripts:
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <stdarg.h>
#include <getopt.h>

#include <libusb.h>
#include "ezusb.h"

#if !defined(_WIN32)
#include <syslog.h>
static bool dosyslog = false;
#endif

#ifndef FXLOAD_VERSION
#define FXLOAD_VERSION (__DATE__ " (development)")
#endif

void logerror(const char *format, ...)
	__attribute__ ((format (__printf__, 1, 2)));

void logerror(const char *format, ...)
{
	va_list ap;
	va_start(ap, format);

#if !defined(_WIN32)
	if (dosyslog)
		vsyslog(LOG_ERR, format, ap);
	else
#else
		vfprintf(stderr, format, ap);
#endif
	va_end(ap);
}

int main(int argc, char*argv[])
{
	const char *ihex_path = 0;
	const char *device_path = getenv("DEVICE");
	const char *type = 0;
	const char *stage1 = 0;
	int opt, status, fx2;
	int config = -1;
	unsigned vid = 0, pid = 0;
	libusb_device_handle *device = NULL;

	while ((opt = getopt(argc, argv, "vV?D:I:c:s:t:")) != EOF)
		switch (opt) {

		case 'D':
			device_path = optarg;
			break;

		case 'I':
			ihex_path = optarg;
			break;

		case 'V':
			puts(FXLOAD_VERSION);
			return 0;

		case 'c':
			config = strtoul(optarg, NULL, 0);
			if (config < 0 || config > 255) {
				logerror("illegal config byte: %s\n", optarg);
				goto usage;
			}
			break;

		case 's':
			stage1 = optarg;
			break;

		case 't':
			if (strcmp(optarg, "an21")		/* original AnchorChips parts */
				&& strcmp(optarg, "fx")		/*  updated Cypress versions */
				&& strcmp(optarg, "fx2")	/* Cypress USB 2.0 versions */
				&& strcmp(optarg, "fx2lp")	/* updated FX2 */
				) {
					logerror("illegal microcontroller type: %s\n", optarg);
					goto usage;
			}
			type = optarg;
			break;

		case 'v':
			verbose++;
			break;

		case '?':
		default:
			goto usage;

	}

	if (config >= 0) {
		if (type == 0) {
			logerror("must specify microcontroller type %s",
				"to write EEPROM!\n");
			goto usage;
		}
		if (!stage1 || !ihex_path) {
			logerror("need 2nd stage loader and firmware %s",
				"to write EEPROM!\n");
			goto usage;
		}
	}

	if ((!device_path) || (!ihex_path)) {
		logerror("no %s specified!\n", (!device_path)?"device":"firmware");
usage:
		fputs("usage: ", stderr);
		fputs(argv[0], stderr);
		fputs(" [-vV] [-t type] [-D devpath]\n", stderr);
		fputs("\t\t[-I firmware_hexfile] ", stderr);
		fputs("[-s loader] [-c config_byte]\n", stderr);
		fputs("... [-D vid:pid] overrides DEVICE= in env\n", stderr);
		fputs("... device types:  one of an21, fx, fx2, fx2lp\n", stderr);
		return -1;
	}

	if (sscanf(device_path, "%x:%x" , &vid, &pid) != 2 ) {
		fputs ("please specify VID & PID as \"vid:pid\" in hexadecimal format\n", stderr);
		return -1;
	}

	status = libusb_init(NULL);
	if (status < 0) {
		logerror("libusb_init() failed: %s\n", libusb_error_name(status));
		return -1;
	}
	libusb_set_debug(NULL, verbose);
	device = libusb_open_device_with_vid_pid(NULL, (uint16_t)vid, (uint16_t)pid);
	if (device == NULL) {
		logerror("libusb_open_device() failed\n");
		libusb_exit(NULL);
		return -1;
	}
	// We need to claim the first interface
	status = libusb_claim_interface(device, 0);
#if defined(__linux__)
	if (status != LIBUSB_SUCCESS) {
		// Maybe we need to detach the driver
		libusb_detach_kernel_driver(device, 0);
		status = libusb_claim_interface(device, 0);
	}
#endif
	if (status != LIBUSB_SUCCESS) {
		logerror("libusb_claim_interface failed: %s\n", libusb_error_name(status));
		libusb_exit(NULL);
		return -1;
	}

	if (type == NULL) {
		type = "fx";	/* an21-compatible for most purposes */
		fx2 = 0;
	} else if (strcmp (type, "fx2lp") == 0)
		fx2 = 2;
	else
		fx2 = (strcmp (type, "fx2") == 0);

	if (verbose)
		logerror("microcontroller type: %s\n", type);

	if (stage1) {
		/* first stage:  put loader into internal memory */
		if (verbose)
			logerror("1st stage:  load 2nd stage loader\n");
		status = ezusb_load_ram(device, stage1, fx2, 0);
		if (status != 0)
			goto out;

		/* second stage ... write either EEPROM, or RAM.  */
		if (config >= 0)
			status = ezusb_load_eeprom(device, ihex_path, type, config);
		else
			status = ezusb_load_ram(device, ihex_path, fx2, 1);
	} else {
		/* single stage, put into internal memory */
		if (verbose)
			logerror("single stage:  load on-chip memory\n");
		status = ezusb_load_ram(device, ihex_path, fx2, 0);
	}

out:
	libusb_release_interface(device, 0);
	libusb_close(device);
	libusb_exit(NULL);
	return status;
}
