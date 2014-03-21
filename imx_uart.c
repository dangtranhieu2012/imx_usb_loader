/*
 * imx_usb:
 *
 * Program to download and execute an image over the USB boot protocol
 * on i.MX series processors.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */
#include <stdio.h>
#include <sys/types.h>
#include <time.h>

#include <unistd.h>
#include <ctype.h>
#include <sys/io.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#include <fcntl.h>
#include <termios.h>

#include <sys/ioctl.h>
#include <linux/serial.h>

#include "imx_sdp.h"

#define get_min(a, b) (((a) < (b)) ? (a) : (b))

int transfer_uart(struct sdp_dev *dev, int report, unsigned char *p, unsigned cnt, int* last_trans)
{
	int err = 0;
	int fd = *(int *)dev->priv;

	if (report < 3) {
		*last_trans = write(fd, p, cnt);
	} else {
		*last_trans = read(fd, p, cnt);
	}

	return err;
}

int connect_uart(int *uart_fd, int argc, char const *const argv[])
{
	int err = 0;
	int count, i;
	int flags = O_RDWR | O_NOCTTY | O_SYNC;
	struct termios key;
	struct serial_struct ser_info; 
	char magic[] = { 0x23, 0x45, 0x45, 0x23 };
	char magic_response[4];
	memset(&key,0,sizeof(key));
	memset(&magic_response,0,sizeof(magic_response));

	*uart_fd = open(argv[1], flags);
	if (*uart_fd < 0) {
		fprintf(stdout, "open() failed: %s\n", strerror(errno));
		return *uart_fd;
	}

	/* 8 data bits */
	key.c_cflag |= CS8;
	key.c_cflag |= CLOCAL | CREAD;
	key.c_cflag |= B115200;

	/* Enable blocking read, 0.5s timeout.. */
	key.c_cc[VMIN] = 1;
	key.c_cc[VTIME] = 5;

	err = tcsetattr(*uart_fd, TCSAFLUSH, &key);
	if (err < 0) {
		fprintf(stdout, "tcsetattr() failed: %s\n", strerror(errno));
		close(*uart_fd);
		return err;
	}

	err = tcflush(*uart_fd, TCIOFLUSH);

	// Association phase, send and receive 0x23454523
	write(*uart_fd, magic, sizeof(magic));
	
	//err = tcflush(*uart_fd, TCIOFLUSH);
	count = read(*uart_fd, &magic_response, sizeof(magic_response));

	if (count < 0) {
		fprintf(stderr, "magic timeout, make sure the device is in "
			       "recovery mode\n");
		return -1;
	}

	printf("count: %d\n", count);
	for (i = 0; i < sizeof(magic); i++) {
		if (magic[i] != magic_response[i]) {
			fprintf(stderr, "magic missmatch, response was 0x%08x\n",
					*(uint32_t *)magic_response);
			return -1;
		}
	}

	fprintf(stderr, "association phase succeeded, response was 0x%08x\n",
				*(uint32_t *)magic_response);

	return err;
}

#define ARRAY_SIZE(w) sizeof(w)/sizeof(w[0])

int main(int argc, char const *const argv[])
{
	struct sdp_dev *p_id;
	int err;
	int ret=1;
	ssize_t cnt;
	int config = 0;
	int verify = 0;
	struct sdp_work w[10];
	struct sdp_work *curr;
	int i = 3;
	int w_index = -1;
	int uart_fd;

	if (argc < 3) {
		fprintf(stderr, "usage: imx_uart [uart] [config]\n");
		goto out;
	}

	// Get list of machines...
	err = connect_uart(&uart_fd, argc, argv);
	if (err < 0)
		goto out;

	// Get machine specific configuration file..
	p_id = parse_conf(argv[2], argc, argv);
	if (!p_id)
		goto out;
	p_id->transfer = &transfer_uart;

	// UART private pointer is TTY file descriptor...
	p_id->priv = &uart_fd;

	err = do_status(p_id);
	if (err) {
		printf("status failed\n");
		goto out;
	}

	curr = p_id->work;
	while (argc > i) {
		const char *p = argv[i];
		if (*p == '-') {
			char c;
			p++;
			c = *p++;
			if (c == 'v') {
				verify = 1;
				i++;
				continue;
			}
			if (w_index < 0) {
				printf("specify file first\n");
				exit(1);
			}
			if (!*p) {
				i++;
				p = argv[i];
			}
			if (c == 's') {
				w[w_index].load_size = get_val(&p, 10);
				if (!w[w_index].load_addr)
					w[w_index].load_addr = 0x10800000;
				w[w_index].plug = 0;
				w[w_index].jump_mode = 0;
				i++;
				continue;
			}
			if (c == 'l') {
				w[w_index].load_addr = get_val(&p, 16);
				w[w_index].plug = 0;
				w[w_index].jump_mode = 0;
				i++;
				continue;
			}
			printf("Unknown option %s\n", p);
			exit(1);

		}
		if (w_index >= 0) {
			w[w_index].jump_mode = 0;
			w[w_index].next = &w[w_index + 1];
		}
		w_index++;
		if (w_index > ARRAY_SIZE(w)) {
			printf("too many files\n");
			exit(1);
		}
		memset(&w[w_index], 0, sizeof(struct sdp_work));
		if (w_index == 0) {
			w[w_index].dcd = 1;
			w[w_index].plug = 1;
		}
		w[w_index].jump_mode = J_HEADER;
		strncpy(w[w_index].filename, argv[i], sizeof(w[w_index].filename) - 1);
		i++;
	}
	if (w_index >= 0)
		curr = &w[0];
	while (curr) {
		if (curr->mem)
			perform_mem_work(p_id, curr->mem);
//		printf("jump_mode %x\n", curr->jump_mode);
		if (curr->filename[0]) {
			err = DoIRomDownload(p_id, curr, verify);
		}
		if (err) {
			err = do_status(p_id);
			break;
		}
		if (!curr->next && (!curr->plug || (w_index != 0)))
			break;
		err = do_status(p_id);
		printf("jump_mode %x plug=%i err=%i\n", curr->jump_mode, curr->plug, err);
		if (err) {
			goto out;
			//int retry;
			/* Rediscovers device */
			/*
			libusb_release_interface(h, 0);
			libusb_close(h);
			libusb_exit(NULL);
			for (retry = 0; retry < 10; retry++) {
				printf("sleeping\n");
				sleep(3);
				printf("done sleeping\n");
				h = open_vid_pid(mach, p_id);
				if (h)
					break;
			}
			if (!h)
				goto out;
				*/
		}
		if ((w_index == 0) && curr->plug) {
			curr->plug = 0;
			continue;
		}
		curr = curr->next;
	}
	ret = 0;
exit:
	//libusb_release_interface(h, 0);
out:
	/*
	if (h)
		libusb_close(h);
	libusb_exit(NULL);
	*/
	return ret;
}