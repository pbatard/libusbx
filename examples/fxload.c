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
#include <stdarg.h>
#include <sys/types.h>
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

#ifndef ARRAYSIZE
#define ARRAYSIZE(A) (sizeof(A)/sizeof((A)[0]))
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
	fx_known_device known_device[] = FX_KNOWN_DEVICES;
	const char *firmware_path = NULL, *loader_path = NULL;
	const char *device_id = getenv("DEVICE");
	const char *type = NULL;
	const char *fx_name[FX_TYPE_MAX] = FX_TYPE_NAMES;
	int opt, status, fx_type = FX_TYPE_UNDEFINED;
	int i, j, config = -1;
	unsigned vid = 0, pid = 0;
	libusb_device *dev, **devs;
	libusb_device_handle *device = NULL;
	struct libusb_device_descriptor desc;

	while ((opt = getopt(argc, argv, "vV?D:I:c:s:t:")) != EOF)
		switch (opt) {

		case 'D':
			device_id = optarg;
			break;

		case 'I':
			firmware_path = optarg;
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
			loader_path = optarg;
			break;

		case 't':
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
			logerror("must specify microcontroller type to write EEPROM!\n");
			goto usage;
		}
		if (!loader_path || !firmware_path) {
			logerror("need 2nd stage loader and firmware to write EEPROM!\n");
			goto usage;
		}
	}

	if (firmware_path == NULL) {
		logerror("no firmware specified!\n");
usage:
		fprintf(stderr, "\nusage: %s [-vV] [-t type] [-D vid:pid] -I firmware\n", argv[0]);
		fprintf(stderr, "      [-s loader] [-c config_byte]\n");
		fprintf(stderr, "      type: one of an21, fx, fx2, fx2lp, fx3\n");
		return -1;
	}

	if ((device_id != NULL) && (sscanf(device_id, "%x:%x" , &vid, &pid) != 2 )) {
		fputs ("please specify VID & PID as \"vid:pid\" in hexadecimal format\n", stderr);
		return -1;
	}

	/* determine the target type */
	if (type != NULL) {
		for (i=0; i<FX_TYPE_MAX; i++) {
			if (strcmp(type, fx_name[i]) == 0) {
				fx_type = i;
				break;
			}
		}
		if (i >= FX_TYPE_MAX) {
			logerror("illegal microcontroller type: %s\n", type);
			goto usage;
		}
	}

	/* open the device using libusbx */
	status = libusb_init(NULL);
	if (status < 0) {
		logerror("libusb_init() failed: %s\n", libusb_error_name(status));
		return -1;
	}
	libusb_set_debug(NULL, verbose);

	/* try to pick up missing parameters from known devices */
	if ((type == NULL) || (device_id == NULL)) {
		if (libusb_get_device_list(NULL, &devs) < 0) {
			logerror("libusb_get_device_list() failed: %s\n", libusb_error_name(status));
			libusb_exit(NULL);
			return -1;
		}
		for (i=0; (dev=devs[i]) != NULL; i++) {
			status = libusb_get_device_descriptor(dev, &desc);
			if (status >= 0) {
				if (verbose >= 2)
					logerror("trying to match against %04x:%04x\n", desc.idVendor, desc.idProduct);
				for (j=0; j<ARRAYSIZE(known_device); j++) {
					if ((desc.idVendor == known_device[j].vid)
						&& (desc.idProduct == known_device[j].pid)) {
						if ((type == NULL) && (device_id == NULL)) {
							fx_type = known_device[j].type;
							vid = desc.idVendor;
							pid = desc.idProduct;
							break;
						} else if ((type == NULL) && (vid == desc.idVendor)
							&& (pid == desc.idProduct)) {
							fx_type = known_device[j].type;
							break;
						} else if ((device_id == NULL)
							&& (fx_type == known_device[j].type)) {
							vid = desc.idVendor;
							pid = desc.idProduct;
							break;
						}
					}
				}
				if (j < ARRAYSIZE(known_device)) {
					if (verbose)
						logerror("found device '%s' [%04x:%04x]\n",
							known_device[j].designation, vid, pid);
					break;
				}
			}
		}
		if (dev == NULL) {
			libusb_free_device_list(devs, 1);
			logerror("could not find a known device - please specify type and/or vid:pid\n");
			goto usage;
		}
		status = libusb_open(dev, &device);
		if (status < 0) {
			logerror("libusb_open() failed: %s\n", libusb_error_name(status));
			libusb_exit(NULL);
			return -1;
		}
		libusb_free_device_list(devs, 1);
	} else {
		device = libusb_open_device_with_vid_pid(NULL, (uint16_t)vid, (uint16_t)pid);
		if (device == NULL) {
			logerror("libusb_open() failed\n");
			libusb_exit(NULL);
			return -1;
		}
	}
	/* We need to claim the first interface */
	status = libusb_claim_interface(device, 0);
#if defined(__linux__)
	if (status != LIBUSB_SUCCESS) {
		/* Maybe we need to detach the driver */
		libusb_detach_kernel_driver(device, 0);
		status = libusb_claim_interface(device, 0);
	}
#endif
	if (status != LIBUSB_SUCCESS) {
		logerror("libusb_claim_interface failed: %s\n", libusb_error_name(status));
		libusb_exit(NULL);
		return -1;
	}

	if (verbose)
		logerror("microcontroller type: %s\n", fx_name[fx_type]);

	if (loader_path) {
		/* first stage: put loader into internal memory */
		if (verbose)
			logerror("1st stage: load 2nd stage loader\n");
		status = ezusb_load_ram(device, loader_path, fx_type, 0);
		if (status != 0)
			goto out;

		/* second stage ... write either EEPROM, or RAM.  */
		if (config >= 0)
			status = ezusb_load_eeprom(device, firmware_path, fx_type, config);
		else
			status = ezusb_load_ram(device, firmware_path, fx_type, 1);
	} else {
		/* single stage, put into internal memory */
		if (verbose)
			logerror("single stage: load on-chip memory\n");
		status = ezusb_load_ram(device, firmware_path, fx_type, 0);
	}

out:
	libusb_release_interface(device, 0);
	libusb_close(device);
	libusb_exit(NULL);
	return status;
}
