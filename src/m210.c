/*
  libm210 - API for Pegasus Mobile NoteTaker M210
  Copyright © 2010 Tuomas Räsänen (tuos) <tuos@codegrove.org>

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>

#include <linux/hidraw.h>
#include <linux/input.h>  /* BUS_USB */
#include <linux/limits.h> /* PATH_MAX */

#include <libudev.h>

#include "m210.h"

#define WAIT_INTERVAL 1000000 /* Microseconds. */

#define M210_PACKET_DATA_LEN 62

struct m210_packet {
    uint16_t num;
    uint8_t data[M210_PACKET_DATA_LEN];
} __attribute__((packed));

static const struct hidraw_devinfo DEVINFO_M210 = {
    BUS_USB,
    0x0e20,
    0x0101,
};

struct m210 {
    int fds[M210_IFACE_COUNT];
};

/*
  Bytes:  0    1    2        3      4          rpt_size
  Values: 0x00 0x02 rpt_size rpt[0] rpt[1] ... rpt[rpt_size - 1]
*/
static m210_err_t m210_write_rpt(struct m210 *m210,
                                 const uint8_t *rpt, size_t rpt_size)
{
    m210_err_t err;
    uint8_t *request;
    size_t request_size = rpt_size + 3;

    request = (uint8_t *) malloc(request_size);
    if (request == NULL)
        return err_sys;

    request[0] = 0x00;     /* Without this, response is not sent. Why?? */
    request[1] = 0x02;     /* report id */
    request[2] = rpt_size;

    /* Copy report paylod to the end of the request. */
    memcpy(request + 3, rpt, rpt_size);

    /* Send request to the interface 0. */
    if (write(m210->fds[0], request, request_size) == -1) {
        err = err_sys;
        goto err;
    }

    err = err_ok;
  err:
    free(request);
    return err;
}

#define M210_RESPONSE_SIZE 64
static m210_err_t m210_read_rpt(struct m210 *m210, void *response,
                                size_t response_size)
{
    fd_set readfds;
    int fd = m210->fds[0];
    static struct timeval select_interval;
    uint8_t buf[M210_RESPONSE_SIZE];

    memset(buf, 0, M210_RESPONSE_SIZE);

    memset(&select_interval, 0, sizeof(struct timeval));
    FD_ZERO(&readfds);
    FD_SET(fd, &readfds);

    select_interval.tv_usec = WAIT_INTERVAL;

    switch (select(fd + 1, &readfds, NULL, NULL, &select_interval)) {
    case 0:
        return err_timeout;
    case -1:
        return err_sys;
    default:
        break;
    }

    if (read(m210->fds[0], buf, M210_RESPONSE_SIZE) == -1)
        return err_sys;

    memset(response, 0, response_size);
    if (response_size > M210_RESPONSE_SIZE)
        memcpy(response, buf, M210_RESPONSE_SIZE);
    else
        memcpy(response, buf, response_size);

    return err_ok;
}

static m210_err_t m210_find_hidraw_devnode(uint8_t *found, int iface,
                                           char *path, size_t path_size)
{
    int err = err_sys;
    struct udev_list_entry *list_entry = NULL;
    struct udev_enumerate *enumerate = NULL;
    struct udev *udev = NULL;

    udev = udev_new();
    if (udev == NULL)
        goto out;

    enumerate = udev_enumerate_new(udev);
    if (enumerate == NULL)
        goto out;

    if (udev_enumerate_add_match_subsystem(enumerate, "hidraw"))
        goto out;

    if (udev_enumerate_scan_devices(enumerate))
        goto out;

    list_entry = udev_enumerate_get_list_entry(enumerate);

    while (list_entry != NULL) {
        int ifn;
        uint16_t vendor;
        uint16_t product;
        const char *syspath = udev_list_entry_get_name(list_entry);
        struct udev_device *dev = udev_device_new_from_syspath(udev, syspath);
        const char *devnode = udev_device_get_devnode(dev);
        struct udev_device *parent = udev_device_get_parent(dev);

        parent = udev_device_get_parent(parent); /* Second parent: usb */
        ifn = atoi(udev_device_get_sysattr_value(parent, "bInterfaceNumber"));
        parent = udev_device_get_parent(parent);
        vendor = strtol(udev_device_get_sysattr_value(parent, "idVendor"),
                        NULL, 16);
        product = strtol(udev_device_get_sysattr_value(parent, "idProduct"),
                         NULL, 16);
        if (vendor == DEVINFO_M210.vendor
            && product == DEVINFO_M210.product
            && iface == ifn) {
            err = err_ok;
            *found = 1;
            strncpy(path, devnode, path_size);
            udev_device_unref(dev);
            goto out;
        }
        list_entry = udev_list_entry_get_next(list_entry);
        udev_device_unref(dev);
    }
    err = err_ok;
    *found = 0;
  out:
    if (enumerate)
        udev_enumerate_unref(enumerate);
    if (udev)
        udev_unref(udev);
    return err;
}

/*
  Get info request:
  Bytes:  0
  Values: 0x95

  Info response:
  Bytes:  0    1    2    3   4   5   6   7   8   9    10
  Values: 0x80 0xa9 0x28 fvh fvl avh avl pvh pvl 0x0e mode

  fvh = Firmware version high
  fvl = Firmware version low
  avh = Analog version high
  avl = Analog version low
  pvh = Pad version high
  pvl = Pad version low
*/
#define m210_wait_ready m210_get_info
m210_err_t m210_get_info(struct m210 *m210, struct m210_info *info)
{
    uint8_t rpt[] = {0x95};
    uint8_t resp[11];

    while (1) {
        m210_err_t err = err_sys;

        memset(&resp, 0, sizeof(resp));

        err = m210_write_rpt(m210, rpt, sizeof(rpt));
        if (err)
            return err;

        err = m210_read_rpt(m210, &resp, sizeof(resp));
        if (err == err_timeout)
            continue;
        else if (err == err_ok)
            break;
        else
            return err;
    }

    /* Check that the received packet is correct. */
    if (resp[0] != 0x80
        || resp[1] != 0xa9
        || resp[2] != 0x28
        || resp[9] != 0x0e)
        return err_badmsg;

    if (info != NULL) {
        /* Fill in only if caller is interested in the info. */
        uint16_t firmware_version;
        uint16_t analog_version;
        uint16_t pad_version;

        memcpy(&firmware_version, resp + 3, 2);
        memcpy(&analog_version, resp + 5, 2);
        memcpy(&pad_version, resp + 7, 2);

        info->firmware_version = be16toh(firmware_version);
        info->analog_version = be16toh(analog_version);
        info->pad_version = be16toh(pad_version);
        info->mode = resp[10];
    }

    return err_ok;
}

static m210_err_t m210_upload_accept(m210_t *m210)
{
    uint8_t rpt[] = {0xb6};

    return m210_write_rpt(m210, rpt, sizeof(rpt));
}

static m210_err_t m210_upload_reject(m210_t *m210)
{
    uint8_t rpt[] = {0xb7};
    m210_err_t err;

    err = m210_write_rpt(m210, rpt, sizeof(rpt));
    if (err)
        return err;

    return m210_wait_ready(m210, NULL);
}

static m210_err_t m210_open_from_hidraw_paths(struct m210 **m210, char **hidraw_paths)
{
    int err = err_sys;
    int i;
    int original_errno;

    *m210 = (struct m210 *) calloc(1, sizeof(struct m210));
    if (*m210 == NULL)
        return err_sys;

    for (i = 0; i < M210_IFACE_COUNT; ++i) {
        (*m210)->fds[i] = -1;
    }

    for (i = 0; i < M210_IFACE_COUNT; ++i) {
        int fd;
        const char *path = hidraw_paths[i];
        struct hidraw_devinfo devinfo;
        memset(&devinfo, 0, sizeof(struct hidraw_devinfo));

        if ((fd = open(path, O_RDWR)) == -1)
            goto err;

        if (ioctl(fd, HIDIOCGRAWINFO, &devinfo))
            goto err;

        if (memcmp(&devinfo, &DEVINFO_M210,
                   sizeof(struct hidraw_devinfo)) != 0) {
            err = err_baddev;
            goto err;
        }

        (*m210)->fds[i] = fd;
    }

    return err_ok;

  err:
    original_errno = errno;
    switch (m210_free(*m210)) {
    case err_sys:
        free(*m210);
        break;
    default:
        break;
    }
    errno = original_errno;
    return err;
}

m210_err_t m210_open(struct m210 **m210, char** hidraw_paths)
{
    if (hidraw_paths == NULL) {
        int i;
        char iface0_path[PATH_MAX];
        char iface1_path[PATH_MAX];
        char *paths[M210_IFACE_COUNT] = {iface0_path, iface1_path};
        for (i = 0; i < M210_IFACE_COUNT; ++i) {
            m210_err_t err = err_sys;
            uint8_t found = 0;

            memset(paths[i], 0, PATH_MAX);

            err = m210_find_hidraw_devnode(&found, i, paths[i], PATH_MAX);
            switch (err) {
            case err_ok:
                break;
            default:
                return err;
            }

            if (!found)
                return err_nodev;
        }
        return m210_open_from_hidraw_paths(m210, paths);
    }
    return m210_open_from_hidraw_paths(m210, hidraw_paths);
}

m210_err_t m210_free(struct m210 *m210)
{
    int i;

    for (i = 0; i < M210_IFACE_COUNT; ++i) {
        if (m210->fds[i] != -1) {
            if (close(m210->fds[i]) == -1)
                return err_sys;
            m210->fds[i] = -1;
        }
    }
    free(m210);
    return err_ok;
}

m210_err_t m210_delete_notes(struct m210 *m210)
{
    m210_err_t err;
    uint8_t rpt[] = {0xb0};

    err = m210_write_rpt(m210, rpt, sizeof(rpt));
    if (err)
        return err;

    return m210_wait_ready(m210, NULL);
}

/*
  Download request can be used for two purposes:

  1: Request just packet count.

  HOST             DEVICE
  =============================
  GET_PACKET_COUNT >
                   < PACKET_COUNT
  REJECT           >

  2: Download packets.

  HOST              DEVICE
  ==============================
  GET_PACKET_COUNT  >
                    < PACKET_COUNT
  ACCEPT            >
                    < PACKET #1
                    < PACKET #2
                    .
                    .
                    .
                    < PACKET #N
  RESEND #X         >
                    < PACKET #X
  RESEND #Y         >
                    < PACKET #Y
  ACCEPT            >

  Packet count request:
  Bytes:  0
  Values: 0xb5

  Packet count response:
  Bytes:  0    1    2    3    4    5          6         7    8
  Values: 0xaa 0xaa 0xaa 0xaa 0xaa count_high count_low 0x55 0x55
*/
m210_err_t m210_upload_begin(m210_t *m210, uint16_t *packetc_ptr)
{
    static const uint8_t sig1[] = {0xaa, 0xaa, 0xaa, 0xaa, 0xaa};
    static const uint8_t sig2[] = {0x55, 0x55};
    static const uint8_t rpt[] = {0xb5};
    uint8_t resp[9];
    m210_err_t err = err_sys;
    int i;

    memset(resp, 0, sizeof(resp));

    err = m210_write_rpt(m210, rpt, sizeof(rpt));
    if (err)
        return err;

    err = m210_read_rpt(m210, resp, sizeof(resp));
    switch (err) {
    case err_timeout:
        /*
          It seems, that a M210 device with zero notes does not send
          any response.
        */
        *packetc_ptr = 0;
        return err_ok;
    case err_ok:
        break;
    default:
        return err;
    }

    /* Check that the packet we received is correct. */
    if (memcmp(resp, sig1, sizeof(sig1))
        || memcmp(resp + sizeof(sig1) + 2, sig2, sizeof(sig2))) {
        for (i = 0; i < 9; ++i) {
            printf("%d %d\n", i, resp[i]);
        }
        return err_badmsg;
    }

    /* Packet count is reported in big-endian format. */
    memcpy(packetc_ptr, resp + sizeof(sig1), 2);
    *packetc_ptr = be16toh(*packetc_ptr);

    return err_ok;
}

/*
  Data packet:
  +-----+------------+-+-+-+-+-+-+-+-+
  |Byte#|Description |7|6|5|4|3|2|1|0|
  +-----+------------+-+-+-+-+-+-+-+-+
  |  1  |Packet# HIGH|N|N|N|N|N|N|N|N|
  +-----+------------+-+-+-+-+-+-+-+-+
  |  2  |Packet# LOW |n|n|n|n|n|n|n|n|
  +-----+------------+-+-+-+-+-+-+-+-+
  |  3  |Data        |x|x|x|x|x|x|x|x|
  +-----+------------+-+-+-+-+-+-+-+-+
  |  .  |Data        |x|x|x|x|x|x|x|x|
  +-----+------------+-+-+-+-+-+-+-+-+
  |  .  |Data        |x|x|x|x|x|x|x|x|
  +-----+------------+-+-+-+-+-+-+-+-+
  |  64 |Data        |x|x|x|x|x|x|x|x|
  +-----+------------+-+-+-+-+-+-+-+-+

*/
m210_err_t m210_upload_read(m210_t *m210, struct m210_packet *packet)
{
    m210_err_t err;

    err = m210_read_rpt(m210, packet, sizeof(struct m210_packet));
    if (err)
        return err;

    packet->num = be16toh(packet->num);

    return err_ok;
}
/*
  Resend packet:
  +-----+------------+-+-+-+-+-+-+-+-+
  |Byte#|Description |7|6|5|4|3|2|1|0|
  +-----+------------+-+-+-+-+-+-+-+-+
  |  1  |NACK        |1|0|1|1|0|1|1|1|
  +-----+------------+-+-+-+-+-+-+-+-+
  |  2  |Packet# HIGH|N|N|N|N|N|N|N|N|
  +-----+------------+-+-+-+-+-+-+-+-+
  |  3  |Packet# LOW |n|n|n|n|n|n|n|n|
  +-----+------------+-+-+-+-+-+-+-+-+
 */
m210_err_t m210_upload_resend(m210_t *m210, uint16_t packet_num)
{
    uint8_t rpt[] = {0xb7, htobe16(packet_num)};

    return m210_write_rpt(m210, rpt, sizeof(rpt));
}

m210_err_t m210_get_packet_count(m210_t *m210, uint16_t *packet_count)
{
    m210_err_t err;

    err = m210_upload_begin(m210, packet_count);
    if (err)
        return err;

    return m210_upload_reject(m210);
}

m210_err_t m210_fwrite_packets(m210_t *m210, FILE *f)
{
    int i;
    m210_err_t err;
    uint16_t *lost_packet_numv;
    uint16_t lost_packet_numc = 0;
    int original_errno;
    uint16_t packetc;

    err = m210_upload_begin(m210, &packetc);
    if (err)
        return err;

    lost_packet_numv = (uint16_t *)calloc(packetc, sizeof(uint16_t));
    if (lost_packet_numv == NULL) {
        original_errno = errno;
        m210_upload_reject(m210);
        errno = original_errno;
        return err_sys;
    }

    err = m210_upload_accept(m210);
    if (err)
        goto err;

    for (i = 0; i < packetc; ++i) {
        struct m210_packet packet;
        uint16_t expected_packet_number = i + 1;

        err = m210_upload_read(m210, &packet);
        if (err) {
            if (err == err_timeout) {
                /*
                  Timeout because there is not any packets left to
                  read. However, the M210 has promised to send more,
                  so we mark all the rest packet numbers as lost and
                  proceed with resending.
                 */
                int j;
                for (j = i; j < packetc; ++j) {
                    uint16_t lost_packet_num = j + 1;
                    lost_packet_numv[lost_packet_numc++] = lost_packet_num;
                }
                err = err_ok; /* This error has been handled. */
                goto resend;
            } else {
                goto err;
            }
        }

        if (packet.num != expected_packet_number)
            lost_packet_numv[lost_packet_numc++] = expected_packet_number;

        if (!lost_packet_numc) {
            if (fwrite(packet.data, sizeof(packet.data), 1, f) != 1) {
                err = err_sys;
                goto err;
            }
        }
    }

  resend:
    while (lost_packet_numc > 0) {
        struct m210_packet packet;

        err = m210_upload_resend(m210, lost_packet_numv[0]);
        if (err)
            goto err;

        err = m210_upload_read(m210, &packet);
        if (err)
            goto err;

        if (packet.num == lost_packet_numv[0]) {
            lost_packet_numv[0] = lost_packet_numv[--lost_packet_numc];
            if (fwrite(packet.data, sizeof(packet.data), 1, f) != 1) {
                err = err_sys;
                goto err;
            }
        }
    }

    /*
      All packets have been received, time to thank the device for
      cooperation.
    */
    err = m210_upload_accept(m210);

  err:
    free(lost_packet_numv);
    return m210_wait_ready(m210, NULL);
}

/*
  Note data:

  Position data:
  +-----+-----------+-+-+-+-+-+-+-+-+
  |Byte#|Description|7|6|5|4|3|2|1|0|
  +-----+-----------+-+-+-+-+-+-+-+-+
  |  1  |X LOW      |x|x|x|x|x|x|x|x|
  +-----+-----------+-+-+-+-+-+-+-+-+
  |  2  |X HIGH     |X|X|X|X|X|X|X|X|
  +-----+-----------+-+-+-+-+-+-+-+-+
  |  3  |Y LOW      |y|y|y|y|y|y|y|y|
  +-----+-----------+-+-+-+-+-+-+-+-+
  |  4  |Y HIGH     |Y|Y|Y|Y|Y|Y|Y|Y|
  +-----+-----------+-+-+-+-+-+-+-+-+

  Pen up data:
  +-----+-----------+-+-+-+-+-+-+-+-+
  |Byte#|Description|7|6|5|4|3|2|1|0|
  +-----+-----------+-+-+-+-+-+-+-+-+
  |  1  |           |0|0|0|0|0|0|0|0|
  +-----+-----------+-+-+-+-+-+-+-+-+
  |  2  |           |0|0|0|0|0|0|0|0|
  +-----+-----------+-+-+-+-+-+-+-+-+
  |  3  |           |0|0|0|0|0|0|0|0|
  +-----+-----------+-+-+-+-+-+-+-+-+
  |  4  |           |1|0|0|0|0|0|0|0|
  +-----+-----------+-+-+-+-+-+-+-+-+
 */
int m210_note_data_is_pen_up(const struct m210_note_data *data)
{
    return (data->x == 0x0000) && (data->y == 0x8000);
}

/* m210_err_t m210_get_notes(m210_t *m210, struct m210_note **notev, uint8_t *notec) */
/* { */
/*     int i; */
/*     m210_err_t err; */
/*     uint16_t packetc; */
/*     struct m210_packet *packetv; */

/*     err = m210_get_packet_count(m210, &packetc); */
/*     if (err) */
/*         return err; */

/*     if (packetc == 0) { */
/*         *notec = 0; */
/*         return err_ok; */
/*     } */

/*     packetv = (struct m210_packet *)calloc(packetc, sizeof(struct m210_packet)); */
/*     if (packetv == NULL) */
/*         return err_sys; */

/*     err = m210_get_packets(m210, packetv, packetc); */
/*     if (err) */
/*         goto err; */

/*     for (i = 0; i < packetc; ++i) { */
/*         struct m210_packet *packet = packetv[i]; */
/*     } */

/*     err = err_ok; */

/*   err: */
/*     free(packetv); */
/*     return err; */
/* } */
