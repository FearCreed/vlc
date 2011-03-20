/**
 * @file linux.c
 * @brief Linux DVB API version 5
 */
/*****************************************************************************
 * Copyright © 2011 Rémi Denis-Courmont
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1
 * of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 ****************************************************************************/

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <vlc_common.h>

#include <errno.h>
#include <assert.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/dvb/frontend.h>
#include <linux/dvb/dmx.h>

#include "dtv/dtv.h"

#ifndef O_SEARCH
# define O_SEARCH O_RDONLY
#endif

/** Opens the device directory for the specified DVB adapter */
static int dvb_open_adapter (uint8_t adapter)
{
    char dir[20];

    snprintf (dir, sizeof (dir), "/dev/dvb/adapter%"PRIu8, adapter);
    return open (dir, O_SEARCH|O_DIRECTORY|O_CLOEXEC);
}

/** Opens the DVB device node of the specified type */
static int dvb_open_node (int dirfd, uint8_t dev, const char *type, int flags)
{
    int fd;
    char path[strlen (type) + 4];

    snprintf (path, sizeof (path), "%s%"PRIu8, type, dev);
    fd = openat (dirfd, path, flags|O_CLOEXEC);
    if (fd != -1)
        fcntl (fd, F_SETFL, fcntl (fd, F_GETFL) | O_NONBLOCK);
    return fd;
}

typedef struct
{
    int vlc;
    int linux_;
} dvb_int_map_t;

static int icmp (const void *a, const void *b)
{
    int key = (intptr_t)a;
    const dvb_int_map_t *entry = b;
    return key - entry->vlc;
}

/** Maps a VLC config integer to a Linux DVB enum value */
static int dvb_parse_int (int i, const dvb_int_map_t *map, size_t n, int def)
{
    const void *k = (const void *)(intptr_t)i;
    const dvb_int_map_t *p = bsearch (k, map, n, sizeof (*map), icmp);
    return (p != NULL) ? p->linux_ : def;
}

typedef const struct
{
    char vlc[8];
    int linux_;
} dvb_str_map_t;

static int scmp (const void *a, const void *b)
{
    const char *key = a;
    dvb_str_map_t *entry = b;
    return strcmp (key, entry->vlc);
}

/** Maps a VLC config string to a Linux DVB enum value */
static int dvb_parse_str (const char *str, const dvb_str_map_t *map, size_t n,
                          int def)
{
    if (str != NULL)
    {
        const dvb_str_map_t *p = bsearch (str, map, n, sizeof (*map), scmp);
        if (p != NULL)
            def = p->linux_;
    }
    return def;
}

/*** Modulations ***/
static int dvb_parse_modulation (const char *str, int def)
{
    static const dvb_str_map_t mods[] =
    {
        { "128QAM",  QAM_128  },
        { "16APSK", APSK_16   },
        { "16QAM",   QAM_16   },
        { "16VSB",   VSB_16   },
        { "256QAM",  QAM_256  },
        { "32APSK", APSK_32   },
        { "32QAM",   QAM_32   },
        { "64QAM",   QAM_64   },
        { "8PSK",    PSK_8    }, 
        { "8VSB",    VSB_8    },
        { "DQPSK", DQPSK      },
        { "QAM",     QAM_AUTO },
        { "QPSK",   QPSK      },
    };
    return dvb_parse_str (str, mods, sizeof (mods) / sizeof (*mods), def);
}

static int dvb_parse_fec (const char *str)
{
    static const dvb_str_map_t rates[] =
    {
        { "",     FEC_AUTO },
        { "1/2",  FEC_1_2  },
        // TODO: 1/3
        // TODO: 1/4
        { "2/3",  FEC_2_3  },
        { "3/4",  FEC_3_4  },
        { "4/5",  FEC_4_5  },
        { "5/6",  FEC_5_6  },
        { "6/7",  FEC_6_7  },
        { "7/8",  FEC_7_8  },
        { "8/9",  FEC_8_9  },
        { "9/10", FEC_9_10 },
    };
    return dvb_parse_str (str, rates, sizeof (rates) / sizeof (*rates),
                          FEC_AUTO);
}


struct dvb_device
{
    vlc_object_t *obj;
    int frontend;
    int demux;
#ifndef USE_DMX
# define MAX_PIDS 256
    int dir;
    uint8_t dev_id;
    struct
    {
        int fd;
        uint16_t pid;
    } pids[MAX_PIDS];
#endif
    int ca;
    struct dvb_frontend_info info;
    bool budget;
    //size_t buffer_size;
};

/**
 * Opens the DVB tuner
 */
dvb_device_t *dvb_open (vlc_object_t *obj, bool tune)
{
    uint8_t adapter = var_InheritInteger (obj, "dvb-adapter");
    uint8_t device  = var_InheritInteger (obj, "dvb-device");

    int dirfd = dvb_open_adapter (adapter);
    if (dirfd == -1)
    {
        msg_Err (obj, "cannot access adapter %"PRIu8": %m", adapter);
        return NULL;
    }

    dvb_device_t *d = malloc (sizeof (*d));
    if (unlikely(d == NULL))
    {
        close (dirfd);
        return NULL;
    }

    d->obj = obj;
    d->frontend = -1;
    d->ca = -1;
    d->budget = var_InheritBool (obj, "dvb-budget-mode");

#ifndef USE_DMX
    if (d->budget)
#endif
    {
       d->demux = dvb_open_node (dirfd, device, "demux", O_RDONLY);
       if (d->demux == -1)
       {
           msg_Err (obj, "cannot access demultiplexer: %m");
           free (d);
           close (dirfd);
           return NULL;
       }

       if (ioctl (d->demux, DMX_SET_BUFFER_SIZE, 1 << 20) < 0)
           msg_Warn (obj, "cannot expand demultiplexing buffer: %m");

       /* We need to filter at least one PID. The tap for TS demultiplexing
        * cannot be configured otherwise. So add the PAT. */
        struct dmx_pes_filter_params param;

        param.pid = d->budget ? 0x2000 : 0x000;
        param.input = DMX_IN_FRONTEND;
        param.output = DMX_OUT_TSDEMUX_TAP;
        param.pes_type = DMX_PES_OTHER;
        param.flags = DMX_IMMEDIATE_START;
        if (ioctl (d->demux, DMX_SET_PES_FILTER, &param) < 0)
        {
            msg_Err (obj, "cannot setup TS demultiplexer: %m");
            goto error;
        }
#ifndef USE_DMX
    }
    else
    {
        d->dir = fcntl (dirfd, F_DUPFD_CLOEXEC);
        d->dev_id = device;

        for (size_t i = 0; i < MAX_PIDS; i++)
            d->pids[i].pid = d->pids[i].fd = -1;
        d->demux = dvb_open_node (d->dir, device, "dvr", O_RDONLY);
        if (d->demux == -1)
        {
            msg_Err (obj, "cannot access DVR: %m");
            free (d);
            close (dirfd);
            return NULL;
        }
#endif
    }

    if (tune)
    {
        d->frontend = dvb_open_node (dirfd, device, "frontend", O_RDWR);
        if (d->frontend == -1)
        {
            msg_Err (obj, "cannot access frontend %"PRIu8
                     " of adapter %"PRIu8": %m", device, adapter);
            goto error;
        }

        if (ioctl (d->frontend, FE_GET_INFO, &d->info) < 0)
        {
            msg_Err (obj, "cannot get frontend info: %m");
            goto error;
        }

        msg_Dbg (obj, "using frontend: %s", d->info.name);
        msg_Dbg (obj, " type %u, capabilities 0x%08X", d->info.type,
                 d->info.caps);

        d->ca = dvb_open_node (dirfd, device, "ca", O_RDWR);
        if (d->ca == -1)
            msg_Dbg (obj, "conditional access module not available (%m)");

    }
    close (dirfd);
    return d;

error:
    close (dirfd);
    dvb_close (d);
    return NULL;
}

void dvb_close (dvb_device_t *d)
{
#ifndef USE_DMX
    if (!d->budget)
    {
        close (d->dir);
        for (size_t i = 0; i < MAX_PIDS; i++)
            if (d->pids[i].fd != -1)
                close (d->pids[i].fd);
    }
#endif
    if (d->ca != -1)
        close (d->ca);
    if (d->frontend != -1)
        close (d->frontend);
    close (d->demux);
    free (d);
}

/**
 * Reads TS data from the tuner.
 * @return number of bytes read, 0 on EOF, -1 if no data (yet).
 */
ssize_t dvb_read (dvb_device_t *d, void *buf, size_t len)
{
    struct pollfd ufd[2];
    int n;

    ufd[0].fd = d->demux;
    ufd[0].events = POLLIN;
    if (d->frontend != -1)
    {
        ufd[1].fd = d->frontend;
        ufd[1].events = POLLIN;
        n = 2;
    }
    else
        n = 1;

    if (poll (ufd, n, 500 /* FIXME */) < 0)
        return -1;

    if (d->frontend != -1 && ufd[1].revents)
    {
        struct dvb_frontend_event ev;

        if (ioctl (d->frontend, FE_GET_EVENT, &ev) < 0)
        {
            if (errno == EOVERFLOW)
            {
                msg_Err (d->obj, "cannot dequeue events fast enough!");
                return -1;
            }
            msg_Err (d->obj, "cannot dequeue frontend event: %m");
            return 0;
        }

        msg_Dbg (d->obj, "frontend status: 0x%02X", (unsigned)ev.status);
    }

    if (ufd[0].revents)
    {
        ssize_t val = read (d->demux, buf, len);
        if (val == -1 && (errno != EAGAIN && errno != EINTR))
        {
            if (errno == EOVERFLOW)
            {
                msg_Err (d->obj, "cannot demux data fast enough!");
                return -1;
            }
            msg_Err (d->obj, "cannot demux: %m");
            return 0;
        }
        return val;
    }

    return -1;
}

int dvb_add_pid (dvb_device_t *d, uint16_t pid)
{
    if (d->budget)
        return 0;
#ifdef USE_DMX
    if (pid == 0 || ioctl (d->demux, DMX_ADD_PID, &pid) >= 0)
        return 0;
#else
    for (size_t i = 0; i < MAX_PIDS; i++)
    {
        if (d->pids[i].pid == pid)
            return 0;
        if (d->pids[i].fd != -1)
            continue;

        int fd = dvb_open_node (d->dir, d->dev_id, "demux", O_RDONLY);
        if (fd == -1)
            goto error;

       /* We need to filter at least one PID. The tap for TS demultiplexing
        * cannot be configured otherwise. So add the PAT. */
        struct dmx_pes_filter_params param;

        param.pid = pid;
        param.input = DMX_IN_FRONTEND;
        param.output = DMX_OUT_TS_TAP;
        param.pes_type = DMX_PES_OTHER;
        param.flags = DMX_IMMEDIATE_START;
        if (ioctl (fd, DMX_SET_PES_FILTER, &param) < 0)
        {
            close (fd);
            goto error;
        }
        d->pids[i].fd = fd;
        d->pids[i].pid = pid;
        return 0;
    }
    errno = EMFILE;
error:
#endif
    msg_Err (d->obj, "cannot add PID 0x%04"PRIu16": %m", pid);
    return -1;
}

void dvb_remove_pid (dvb_device_t *d, uint16_t pid)
{
    if (d->budget)
        return;
#ifdef USE_DMX
    if (pid != 0)
        ioctl (d->demux, DMX_REMOVE_PID, &pid);
#else
    for (size_t i = 0; i < MAX_PIDS; i++)
    {
        if (d->pids[i].pid == pid)
        {
            close (d->pids[i].fd);
            d->pids[i].pid = d->pids[i].fd = -1;
            return;
        }
    }
#endif
}

const delsys_t *dvb_guess_system (dvb_device_t *d)
{
    assert (d->frontend != -1);

    //bool v2 = d->info.caps & FE_CAN_2G_MODULATION;

    switch (d->info.type)
    {
        case FE_QPSK: return /*v2 ? &dvbs2 :*/ &dvbs;
        case FE_QAM:  return &dvbc;
        case FE_OFDM: return &dvbt;
        case FE_ATSC: return &atsc;
    }
    return NULL;
}

float dvb_get_signal_strength (dvb_device_t *d)
{
    uint16_t strength;

    if (ioctl (d->frontend, FE_READ_SIGNAL_STRENGTH, &strength) < 0)
        return 0.;
    return strength / 65535.;
}

float dvb_get_snr (dvb_device_t *d)
{
    uint16_t snr;

    if (ioctl (d->frontend, FE_READ_SNR, &snr) < 0)
        return 0.;
    return snr / 65535.;
}

static int dvb_vset_props (dvb_device_t *d, size_t n, va_list ap)
{
    struct dtv_property buf[n], *prop = buf;
    struct dtv_properties props = { .num = n, .props = buf };

    memset (prop, 0, sizeof (prop));

    while (n > 0)
    {
        prop->cmd = va_arg (ap, uint32_t);
        prop->u.data = va_arg (ap, uint32_t);
        msg_Dbg (d->obj, "setting property %"PRIu32" to %"PRIu32,
                 prop->cmd, prop->u.data);
        prop++;
        n--;
    }

    if (ioctl (d->frontend, FE_SET_PROPERTY, &props) < 0)
    {
        msg_Err (d->obj, "cannot set frontend tuning parameters: %m");
        return -1;
    }
    return 0;
}

static int dvb_set_props (dvb_device_t *d, size_t n, ...)
{
    va_list ap;
    int ret;

    va_start (ap, n);
    ret = dvb_vset_props (d, n, ap);
    va_end (ap);
    return ret;
}

static int dvb_set_prop (dvb_device_t *d, uint32_t prop, uint32_t val)
{
    return dvb_set_props (d, 1, prop, val);
}

int dvb_set_inversion (dvb_device_t *d, int v)
{
    switch (v)
    {
        case 0:  v = INVERSION_OFF;  break;
        case 1:  v = INVERSION_ON;   break;
        default: v = INVERSION_AUTO; break;
    }
    return dvb_set_prop (d, DTV_INVERSION, v);
}

int dvb_tune (dvb_device_t *d)
{
    return dvb_set_prop (d, DTV_TUNE, 0 /* dummy */);
}


/*** DVB-C ***/
int dvb_set_dvbc (dvb_device_t *d, uint32_t freq, const char *modstr,
                  uint32_t srate, const char *fecstr)
{
    unsigned mod = dvb_parse_modulation (modstr, QAM_AUTO);
    unsigned fec = dvb_parse_fec (fecstr);

    return dvb_set_props (d, 6, DTV_CLEAR, 0,
                          DTV_DELIVERY_SYSTEM, SYS_DVBC_ANNEX_AC,
                          DTV_FREQUENCY, freq, DTV_MODULATION, mod,
                          DTV_SYMBOL_RATE, srate, DTV_INNER_FEC, fec);
}


/*** DVB-S ***/
int dvb_set_dvbs (dvb_device_t *d, uint64_t freq,
                  uint32_t srate, const char *fecstr)
{
    unsigned f = freq / 1000;
    unsigned fec = dvb_parse_fec (fecstr);

    return dvb_set_props (d, 5, DTV_CLEAR, 0, DTV_DELIVERY_SYSTEM, SYS_DVBS,
                          DTV_FREQUENCY, f, DTV_SYMBOL_RATE, srate,
                          DTV_INNER_FEC, fec);
}

int dvb_set_dvbs2 (dvb_device_t *d, uint64_t freq, const char *modstr,
                   uint32_t srate, const char *fecstr, int pilot, int rolloff)
{
    unsigned f = freq / 1000;
    unsigned mod = dvb_parse_modulation (modstr, QPSK);
    unsigned fec = dvb_parse_fec (fecstr);

    switch (pilot)
    {
        case 0:  pilot = PILOT_OFF;  break;
        case 1:  pilot = PILOT_ON;   break;
        default: pilot = PILOT_AUTO; break;
    }

    switch (rolloff)
    {
        case 20: rolloff = ROLLOFF_20;  break;
        case 25: rolloff = ROLLOFF_25;  break;
        case 35: rolloff = ROLLOFF_35;  break;
        default: rolloff = PILOT_AUTO; break;
    }

    return dvb_set_props (d, 8, DTV_CLEAR, 0, DTV_DELIVERY_SYSTEM, SYS_DVBS2,
                          DTV_FREQUENCY, f, DTV_MODULATION, mod,
                          DTV_SYMBOL_RATE, srate, DTV_INNER_FEC, fec,
                          DTV_PILOT, pilot, DTV_ROLLOFF, rolloff);
}


/*** DVB-T ***/
static int dvb_parse_transmit_mode (int i)
{
    static const dvb_int_map_t tab[] = {
        { -1, TRANSMISSION_MODE_AUTO },
        {  2, TRANSMISSION_MODE_2K   },
        {  4, TRANSMISSION_MODE_4K   },
        {  8, TRANSMISSION_MODE_8K   },
#if 0
        { 16, TRANSMISSION_MODE_16K  },
        { 32, TRANSMISSION_MODE_32K  },
#endif
    };
    return dvb_parse_int (i, tab, sizeof (tab) / sizeof (*tab),
                          TRANSMISSION_MODE_AUTO);
}

static int dvb_parse_guard (const char *str)
{
    static const dvb_str_map_t tab[] = {
        { "",       GUARD_INTERVAL_AUTO },
      /*{ "1/128",  GUARD_INTERVAL_1_128 },*/
        { "1/16",   GUARD_INTERVAL_1_16 },
        { "1/32",   GUARD_INTERVAL_1_32 },
        { "1/4",    GUARD_INTERVAL_1_4 },
        { "1/8",    GUARD_INTERVAL_1_8 },
      /*{ "19/128", GUARD_INTERVAL_19_128 },*/
      /*{ "9/256",  GUARD_INTERVAL_9_256 },*/
    };
    return dvb_parse_str (str, tab, sizeof (tab) / sizeof (*tab),
                          GUARD_INTERVAL_AUTO);
}

static int dvb_parse_hierarchy (int i)
{
    static const dvb_int_map_t tab[] = {
        { HIERARCHY_AUTO, -1 },
        { HIERARCHY_NONE,  0 },
        { HIERARCHY_1,     1 },
        { HIERARCHY_2,     2 },
        { HIERARCHY_4,     4 },
    };
    return dvb_parse_int (i, tab, sizeof (tab) / sizeof (*tab),
                          HIERARCHY_AUTO);
}

int dvb_set_dvbt (dvb_device_t *d, uint32_t freq, const char *modstr,
                  const char *fechstr, const char *feclstr, uint32_t bandwidth,
                  int transmit_val, const char *guardstr, int hierarchy_val)
{
    uint32_t mod = dvb_parse_modulation (modstr, QAM_AUTO);
    uint32_t fec_hp = dvb_parse_fec (fechstr);
    uint32_t fec_lp = dvb_parse_fec (feclstr);
    bandwidth *= 1000000;
    uint32_t transmit_mode = dvb_parse_transmit_mode (transmit_val);
    uint32_t guard_it = dvb_parse_guard (guardstr);
    uint32_t hierarchy = dvb_parse_hierarchy (hierarchy_val);

    return dvb_set_props (d, 10, DTV_CLEAR, 0, DTV_DELIVERY_SYSTEM, SYS_DVBT,
                          DTV_FREQUENCY, freq, DTV_MODULATION, mod,
                          DTV_CODE_RATE_HP, fec_hp, DTV_CODE_RATE_LP, fec_lp,
                          DTV_BANDWIDTH_HZ, bandwidth,
                          DTV_TRANSMISSION_MODE, transmit_mode,
                          DTV_GUARD_INTERVAL, guard_it,
                          DTV_HIERARCHY, hierarchy);
}


/*** ATSC ***/
int dvb_set_atsc (dvb_device_t *d, uint32_t freq, const char *modstr)
{
    unsigned mod = dvb_parse_modulation (modstr, VSB_8);

    return dvb_set_props (d, 4, DTV_CLEAR, 0, DTV_DELIVERY_SYSTEM, SYS_ATSC,
                          DTV_FREQUENCY, freq, DTV_MODULATION, mod);
}