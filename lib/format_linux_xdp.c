#include "config.h"
#include "libtrace.h"
#include "libtrace_int.h"
#include "format_linux_xdp.h"

#include <bpf/libbpf.h>
#include <bpf/xsk.h>
#include <bpf/bpf.h>

#include <poll.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <net/if.h>
#include <assert.h>
#include <sys/resource.h>
#include <sys/mman.h>
#include <sys/param.h>

#include <linux/ethtool.h>
#include <linux/if_xdp.h>
#include <sys/ioctl.h>
#include <linux/sockios.h>

#include <linux/if_link.h>

#define FORMAT_DATA ((xdp_format_data_t *)(libtrace->format_data))
#define PACKET_META ((libtrace_xdp_meta_t *)(packet->header))

#define FRAME_HEADROOM     sizeof(libtrace_xdp_meta_t)
#define NUM_FRAMES         4096
#define FRAME_SIZE         XSK_UMEM__DEFAULT_FRAME_SIZE
#define RX_BATCH_SIZE      64
#define INVALID_UMEM_FRAME UINT64_MAX

typedef struct libtrace_xdp_meta {
    uint64_t timestamp;
    uint32_t packet_len;
} PACKED libtrace_xdp_meta_t;

struct xsk_config {
    __u32 xdp_flags;
    __u32 libbpf_flags;
    int ifindex;
    char ifname[IF_NAMESIZE];

    char *bpf_filename;
    char *bpf_progname;

    __u16 xsk_bind_flags;

    struct bpf_object *bpf_obj;

    struct bpf_program *bpf_prg;
    __u32 bpf_prg_fd;

    struct bpf_map *xsks_map;
    int xsks_map_fd;

    struct bpf_map *libtrace_map;
    int libtrace_map_fd;

    struct bpf_map *libtrace_ctrl_map;
    int libtrace_ctrl_map_fd;
};

struct xsk_umem_info {
    struct xsk_ring_cons cq;
    struct xsk_ring_prod fq;
    struct xsk_umem *umem;
    int xsk_if_queue;
    void *buffer;
};

struct xsk_socket_info {
    struct xsk_ring_cons rx;
    struct xsk_ring_prod tx;
    struct xsk_umem_info *umem;
    struct xsk_socket *xsk;
};

struct xsk_per_stream {
    struct xsk_umem_info *umem;
    struct xsk_socket_info *xsk;

    /* keep track of the number of previously processed packets */
    unsigned int prev_rcvd;

    /* previous timestamp for this stream */
    uint64_t prev_sys_time;
};

typedef struct xdp_format_data {
    struct xsk_config cfg;
    libtrace_list_t *per_stream;

    enum hasher_types hasher_type;
    xdp_state state;

} xdp_format_data_t;

static struct bpf_object *load_bpf_and_xdp_attach(struct xsk_config *cfg);
static int xdp_link_detach(struct xsk_config *cfg);
static int linux_xdp_prepare_packet(libtrace_t *libtrace,
                                    libtrace_packet_t *packet,
                                    void *buffer,
                                    libtrace_rt_types_t rt_type,
                                    uint32_t flags);
static int linux_xdp_start_stream(struct xsk_config *cfg,
                                  struct xsk_per_stream *stream,
                                  int ifqueue,
                                  int dir);
static void xsk_populate_fill_ring(struct xsk_umem_info *umem);
static int linux_xdp_send_ioctl_ethtool(struct ethtool_channels *channels,
                                        char *ifname);
static int linux_xdp_get_max_queues(char *ifname);
static int linux_xdp_get_current_queues(char *ifname);
static int linux_xdp_set_current_queues(char *ifname, int queues);

static bool linux_xdp_can_write(libtrace_packet_t *packet) {
    /* Get the linktype */
        libtrace_linktype_t ltype = trace_get_link_type(packet);

        if (ltype == TRACE_TYPE_CONTENT_INVALID) {
                return false;
        }
        if (ltype == TRACE_TYPE_NONDATA) {
                return false;
        }
        if (ltype == TRACE_TYPE_PCAPNG_META) {
                return false;
        }
        if (ltype == TRACE_TYPE_ERF_META) {
                return false;
        }

        return true;
}

static int linux_xdp_send_ioctl_ethtool(struct ethtool_channels *channels,
                                        char *ifname) {

    struct ifreq ifr = {};
    int fd, err, ret;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -errno;

    ifr.ifr_data = (void *)channels;
    memcpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    ifr.ifr_name[IFNAMSIZ - 1] = '\0';
    err = ioctl(fd, SIOCETHTOOL, &ifr);
    if (err && errno != EOPNOTSUPP) {
        ret = -errno;
        goto out;
    }

    /* return 1 on error, error usually occurs when the nic only
     * supports a single queue. */
    if (err) {
        ret = 1;
    } else {
        ret = 0;
    }

out:
    close(fd);
    return ret;
}

static int linux_xdp_get_max_queues(char *ifname) {

    struct ethtool_channels channels = { .cmd = ETHTOOL_GCHANNELS };
    int ret;

    if ((ret = linux_xdp_send_ioctl_ethtool(&channels, ifname)) == 0) {
        ret = MAX(channels.max_rx, channels.max_tx);
        ret = MAX(ret, (int)channels.max_combined);
    }

    return ret;
}

static int linux_xdp_get_current_queues(char *ifname) {
    struct ethtool_channels channels = { .cmd = ETHTOOL_GCHANNELS };
    int ret;

    if ((ret = linux_xdp_send_ioctl_ethtool(&channels, ifname)) == 0) {
        ret = MAX(channels.rx_count, channels.tx_count);
        ret = MAX(ret, (int)channels.combined_count);
    }

    return ret;
}

static int linux_xdp_set_current_queues(char *ifname, int queues) {
    struct ethtool_channels channels = { .cmd = ETHTOOL_GCHANNELS };
    __u32 org_combined;
    int ret;

    /* get the current settings */
    if ((ret = linux_xdp_send_ioctl_ethtool(&channels, ifname)) == 0) {

        org_combined = channels.combined_count;
        channels.cmd = ETHTOOL_SCHANNELS;
        channels.combined_count = queues;
        /* try update */
        if ((ret = linux_xdp_send_ioctl_ethtool(&channels, ifname)) == 0) {
            /* success */
            return channels.combined_count;
        }

        /* try set rx and tx individually */
        channels.rx_count = queues;
        channels.tx_count = queues;
        channels.combined_count = org_combined;
        /* try again */
        if ((ret = linux_xdp_send_ioctl_ethtool(&channels, ifname)) == 0) {
            /* success */
            return channels.rx_count;
        }
    }

    /* could not set the number of queues */
    return ret;
}

static struct xsk_umem_info *configure_xsk_umem(void *buffer, uint64_t size,
    int interface_queue) {

    struct xsk_umem_info *umem;
    struct xsk_umem_config umem_cfg;
    int ret = 1;

    umem = calloc(1, sizeof(*umem));
    if (umem == NULL) {
        return NULL;
    }

    umem_cfg.fill_size = XSK_RING_PROD__DEFAULT_NUM_DESCS;
    umem_cfg.comp_size = XSK_RING_PROD__DEFAULT_NUM_DESCS;
    umem_cfg.frame_size = FRAME_SIZE;
    umem_cfg.frame_headroom = FRAME_HEADROOM;
    umem_cfg.flags = XSK_UMEM__DEFAULT_FLAGS;

    ret = xsk_umem__create(&umem->umem,
                           buffer,
                           size,
                           &umem->fq,
                           &umem->cq,
                           &umem_cfg);

    if (ret) {
        errno = -ret;
        return NULL;
    }

    umem->buffer = buffer;
    umem->xsk_if_queue = interface_queue;

    /* populate fill ring */
    xsk_populate_fill_ring(umem);

    return umem;
}

static struct xsk_socket_info *xsk_configure_socket(struct xsk_config *cfg,
                                                    struct xsk_umem_info *umem,
                                                    int dir) {

    struct xsk_socket_config xsk_cfg;
    struct xsk_socket_info *xsk_info;
    uint32_t prog_id = 0;
    int ret = 1;

    xsk_info = calloc(1, sizeof(*xsk_info));
    if (xsk_info == NULL) {
        return NULL;
    }

    xsk_info->umem = umem;
    xsk_cfg.rx_size = XSK_RING_CONS__DEFAULT_NUM_DESCS;
    xsk_cfg.tx_size = XSK_RING_PROD__DEFAULT_NUM_DESCS;
    xsk_cfg.libbpf_flags = cfg->libbpf_flags;
    xsk_cfg.xdp_flags = cfg->xdp_flags;
    xsk_cfg.bind_flags = cfg->xsk_bind_flags;

    /* inbound */
    if (dir == 0) {
        ret = xsk_socket__create(&xsk_info->xsk,
                                 cfg->ifname,
                                 umem->xsk_if_queue,
                                 umem->umem,
                                 &xsk_info->rx,
                                 NULL,
                                 &xsk_cfg);
    /* outbound */
    } else if (dir == 1) {
        ret = xsk_socket__create(&xsk_info->xsk,
                                 cfg->ifname,
                                 umem->xsk_if_queue,
                                 umem->umem,
                                 NULL,
                                 &xsk_info->tx,
                                 &xsk_cfg);
    }

    if (ret) {
        errno = -ret;
        return NULL;
    }

    ret = bpf_get_link_xdp_id(cfg->ifindex, &prog_id, cfg->xdp_flags);
    if (ret) {
        errno = -ret;
        return NULL;
    }

    return xsk_info;
}

static void xsk_populate_fill_ring(struct xsk_umem_info *umem) {

    int ret, i;
    uint32_t idx;

    ret = xsk_ring_prod__reserve(&umem->fq,
                                 XSK_RING_PROD__DEFAULT_NUM_DESCS,
                                 &idx);
    if (ret != XSK_RING_PROD__DEFAULT_NUM_DESCS) {
        fprintf(stderr," error allocating xdp fill queue\n");
        return;
    }

    for (i = 0; i < XSK_RING_PROD__DEFAULT_NUM_DESCS; i++) {
        *xsk_ring_prod__fill_addr(&umem->fq, idx++) =
            i * FRAME_SIZE;
    }

    xsk_ring_prod__submit(&umem->fq, XSK_RING_PROD__DEFAULT_NUM_DESCS);

}

static void linux_xdp_complete_tx(struct xsk_socket_info *xsk) {

    unsigned int rcvd;
    uint32_t idx;

    /* does the socket need a wakeup? */
    if (xsk_ring_prod__needs_wakeup(&xsk->tx)) {
        sendto(xsk_socket__fd(xsk->xsk), NULL, 0, MSG_DONTWAIT, NULL, 0);
    }

    /* free completed TX buffers */
    rcvd = xsk_ring_cons__peek(&xsk->umem->cq, XSK_RING_CONS__DEFAULT_NUM_DESCS, &idx);
    if (rcvd > 0) {
        /* release the number of sent frames */
        xsk_ring_cons__release(&xsk->umem->cq, rcvd);
    }
}

static uint64_t linux_xdp_get_time(struct xsk_per_stream *stream) {

    uint64_t sys_time;
    struct timespec ts = {0};
    clock_gettime(CLOCK_REALTIME, &ts);

    sys_time = ((uint64_t) ts.tv_sec * 1000000000ull + (uint64_t) ts.tv_nsec);

    if (stream->prev_sys_time >= sys_time) {
        sys_time = stream->prev_sys_time + 1;
    }

    stream->prev_sys_time = sys_time;

    return sys_time;
}

static int linux_xdp_init_control_map(libtrace_t *libtrace) {

    libtrace_ctrl_map_t ctrl_map;
    int key = 0;

    if (FORMAT_DATA->cfg.libtrace_ctrl_map_fd <= 0) {
       return -1;
    }

    switch (FORMAT_DATA->hasher_type) {
        case HASHER_CUSTOM:
        case HASHER_BALANCE:
            ctrl_map.hasher = XDP_BALANCE;
            break;
        case HASHER_UNIDIRECTIONAL:
            ctrl_map.hasher = XDP_UNIDIRECTIONAL;
            break;
        case HASHER_BIDIRECTIONAL:
            ctrl_map.hasher = XDP_BIDIRECTIONAL;
            break;
    }

    ctrl_map.max_queues = libtrace->perpkt_thread_count;
    ctrl_map.state = XDP_NOT_STARTED;

    if (bpf_map_update_elem(FORMAT_DATA->cfg.libtrace_ctrl_map_fd,
                            &key,
                            &ctrl_map,
                            BPF_ANY) != 0) {
        return -1;
    }

    return 0;
}

static int linux_xdp_set_control_map_state(libtrace_t *libtrace, xdp_state state) {

    libtrace_ctrl_map_t ctrl_map;
    int key = 0;

    if (FORMAT_DATA->cfg.libtrace_ctrl_map_fd <= 0) {
        return -1;
    }

    /* get data from libtrace control map */
    if ((bpf_map_lookup_elem(FORMAT_DATA->cfg.libtrace_ctrl_map_fd,
                             &key,
                             &ctrl_map)) != 0) {
        return -1;
    }

    /* update state */
    ctrl_map.state = state;

    /* push changes back to the control map */
    if (bpf_map_update_elem(FORMAT_DATA->cfg.libtrace_ctrl_map_fd,
                            &key,
                            &ctrl_map,
                            BPF_ANY) != 0) {
        return -1;
    }

    return 0;
}

static int linux_xdp_init_input(libtrace_t *libtrace) {

    struct rlimit r = {RLIM_INFINITY, RLIM_INFINITY};
    char *scan, *scan2 = NULL;

    if (setrlimit(RLIMIT_MEMLOCK, &r)) {
        trace_set_err(libtrace, TRACE_ERR_INIT_FAILED, "Unable "
            "to setrlimit(RLIMIT_MEMLOCK) in linux_xdp_init_input");
        return -1;
    }

    // allocate space for the format data
    libtrace->format_data = (xdp_format_data_t *)calloc(1,
        sizeof(xdp_format_data_t));
    if (libtrace->format_data == NULL) {
        trace_set_err(libtrace, TRACE_ERR_INIT_FAILED, "Unable "
            "to allocate memory for format data in linux_xdp_init_input()");
        return -1;
    }
    FORMAT_DATA->hasher_type = HASHER_BALANCE;
    FORMAT_DATA->state = XDP_NOT_STARTED;

    /* setup XDP config */
    scan = strchr(libtrace->uridata, ':');
    if (scan == NULL) {
        /* if no : was found we just have interface name. */
        memcpy(FORMAT_DATA->cfg.ifname, libtrace->uridata, strlen(libtrace->uridata));

        /* try to locate the libtrace bpf program */
        if (access(libtrace_xdp_kern, F_OK) != -1) {
            FORMAT_DATA->cfg.bpf_filename = strdup(libtrace_xdp_kern);
            FORMAT_DATA->cfg.bpf_progname = strdup(libtrace_xdp_prog);
        } else {
            FORMAT_DATA->cfg.bpf_filename = NULL;
            FORMAT_DATA->cfg.bpf_progname = NULL;
            fprintf(stderr, "Unable to locate Libtrace BPF program, loading Libbpf program\n");
        }

    } else {
        /* user supplied bpf program must be structured like:
         * kern:prog:interface
         */
        scan2 = strchr(scan + 1, ':');
        if (scan2 == NULL) {
            trace_set_err(libtrace, TRACE_ERR_INIT_FAILED, "Invalid "
                "URI: Usage xdp:bpf_kern:bpf_prog:interface\n");
            return -1;
        } else {
            FORMAT_DATA->cfg.bpf_filename = strndup(libtrace->uridata,
                (size_t)(scan - libtrace->uridata));
            FORMAT_DATA->cfg.bpf_progname = strndup(scan + 1,
                (size_t)(scan2 - (scan + 1)));
            memcpy(FORMAT_DATA->cfg.ifname, scan2 + 1, strlen(scan2 + 1));
        }
    }

    /* check interface */
    FORMAT_DATA->cfg.ifindex = if_nametoindex(FORMAT_DATA->cfg.ifname);
    if (FORMAT_DATA->cfg.ifindex == 0) {
        trace_set_err(libtrace, TRACE_ERR_INIT_FAILED, "Invalid XDP input interface "
            "name: %s", FORMAT_DATA->cfg.ifname);
        return -1;
    }

    /* load XDP program if supplied */
    if (FORMAT_DATA->cfg.bpf_filename != NULL) {
        // load custom BPF program
        load_bpf_and_xdp_attach(&FORMAT_DATA->cfg);

        // load the xsk map
        FORMAT_DATA->cfg.xsks_map = bpf_object__find_map_by_name(FORMAT_DATA->cfg.bpf_obj, "xsks_map");
        FORMAT_DATA->cfg.xsks_map_fd = bpf_map__fd(FORMAT_DATA->cfg.xsks_map);
        if (FORMAT_DATA->cfg.xsks_map_fd < 0) {
            trace_set_err(libtrace, TRACE_ERR_INIT_FAILED, "Unable to load xsks map from BPF");
            return -1;
        }

        // load libtrace map
        FORMAT_DATA->cfg.libtrace_map = bpf_object__find_map_by_name(FORMAT_DATA->cfg.bpf_obj, "libtrace_map");
        FORMAT_DATA->cfg.libtrace_map_fd = bpf_map__fd(FORMAT_DATA->cfg.libtrace_map);
        if (FORMAT_DATA->cfg.libtrace_map_fd < 0) {
            /* unable to load libtrace_map. Is this a custom bpf program? */
        }

        // load libtrace control map
        FORMAT_DATA->cfg.libtrace_ctrl_map =
            bpf_object__find_map_by_name(FORMAT_DATA->cfg.bpf_obj, "libtrace_ctrl_map");
        FORMAT_DATA->cfg.libtrace_ctrl_map_fd =
            bpf_map__fd(FORMAT_DATA->cfg.libtrace_ctrl_map);
        if (FORMAT_DATA->cfg.libtrace_ctrl_map_fd >= 0) {
            /* init the libtrace control map */
            if (linux_xdp_init_control_map(libtrace) == -1) {
                /* failed to init the control map */
                trace_set_err(libtrace, TRACE_ERR_INIT_FAILED, "Unable to init libtrace XDP control map");
                return -1;
            }
        }
    }

    /* setup list to hold the streams */
    FORMAT_DATA->per_stream = libtrace_list_init(sizeof(struct xsk_per_stream));
    if (FORMAT_DATA->per_stream == NULL) {
        trace_set_err(libtrace, TRACE_ERR_INIT_FAILED, "Unable to create list "
            "for stream data in linux_xdp_init_input()");
        return -1;
    }

    return 0;
}

static int linux_xdp_init_output(libtrace_out_t *libtrace) {

    struct rlimit r = {RLIM_INFINITY, RLIM_INFINITY};
    char *scan = NULL;

    if (setrlimit(RLIMIT_MEMLOCK, &r)) {
        trace_set_err_out(libtrace, TRACE_ERR_INIT_FAILED, "Unable "
            "to setrlimit(RLIMIT_MEMLOCK) in linux_xdp_init_output()");
        return -1;
    }

    // allocate space for the format data
    libtrace->format_data = (xdp_format_data_t *)calloc(1,
        sizeof(xdp_format_data_t));
    if (libtrace->format_data == NULL) {
        trace_set_err_out(libtrace, TRACE_ERR_INIT_FAILED, "Unable "
            "to allocate memory for format data in linux_xdp_init_output()");
        return -1;
    }

    /* setup XDP config */
    scan = strchr(libtrace->uridata, ':');
    if (scan == NULL) {
        memcpy(FORMAT_DATA->cfg.ifname, libtrace->uridata, strlen(libtrace->uridata));
    } else {
        memcpy(FORMAT_DATA->cfg.ifname, scan + 1, strlen(scan + 1));
    }
    FORMAT_DATA->cfg.ifindex = if_nametoindex(FORMAT_DATA->cfg.ifname);
    if (FORMAT_DATA->cfg.ifindex == 0) {
        trace_set_err_out(libtrace, TRACE_ERR_INIT_FAILED, "Invalid XDP output interface "
            "name.");
        return -1;
    }

    /* setup list to hold the streams */
    FORMAT_DATA->per_stream = libtrace_list_init(sizeof(struct xsk_per_stream));
    if (FORMAT_DATA->per_stream == NULL) {
        trace_set_err_out(libtrace, TRACE_ERR_INIT_FAILED, "Unable to create list "
            "for stream data in linux_xdp_init_input()");
        return -1;
    }

    return 0;
}

static int linux_xdp_pstart_input(libtrace_t *libtrace) {

    int i;
    struct xsk_per_stream empty_stream = {NULL,NULL,0,0};
    struct xsk_per_stream *stream;
    int max_nic_queues;
    int ret;

    /* was the input previously paused? */
    if (FORMAT_DATA->state == XDP_PAUSED) {
        /* update state and return */
        linux_xdp_set_control_map_state(libtrace, XDP_RUNNING);
        FORMAT_DATA->state = XDP_RUNNING;

        return 0;
    }

    /* get the maximum number of supported nic queues */
    max_nic_queues = linux_xdp_get_max_queues(FORMAT_DATA->cfg.ifname);

    /* if the number of processing threads is greater than the max supported NIC
     * queues reduce the number of threads to match */
    if (libtrace->perpkt_thread_count > max_nic_queues) {
        //fprintf(stderr, "Decreasing number of processing threads to match the number "
        //    "of available NIC queues %d\n", max_nic_queues);
        libtrace->perpkt_thread_count = max_nic_queues;
    }

    /* set the number of nic queues to match number of threads */
    if (linux_xdp_set_current_queues(FORMAT_DATA->cfg.ifname, libtrace->perpkt_thread_count) !=
        libtrace->perpkt_thread_count) {

        trace_set_err(libtrace, TRACE_ERR_INIT_FAILED, "Unable to set number of NIC queues "
            "to match the number of processing threads %d", libtrace->perpkt_thread_count);
        return -1;
    }

    /* create a stream for each processing thread */
    for (i = 0; i < libtrace->perpkt_thread_count; i++) {
        libtrace_list_push_back(FORMAT_DATA->per_stream, &empty_stream);

        stream = libtrace_list_get_index(FORMAT_DATA->per_stream, i)->data;

        /* start the stream */
        if ((ret = linux_xdp_start_stream(&FORMAT_DATA->cfg, stream, i, 0)) != 0) {
            trace_set_err(libtrace, TRACE_ERR_INIT_FAILED,
                "Unable to start input stream: %s", strerror(ret));
            return -1;
        }
    }

    /* update state to running */
    linux_xdp_set_control_map_state(libtrace, XDP_RUNNING);
    FORMAT_DATA->state = XDP_RUNNING;

    return 0;
}

static int linux_xdp_start_input(libtrace_t *libtrace) {

    struct xsk_per_stream empty_stream = {NULL,NULL,0,0};
    struct xsk_per_stream *stream;
    int c_nic_queues;
    int ret;

    /* was the input previously paused? */
    if (FORMAT_DATA->state == XDP_PAUSED) {
        /* update state and return */
        linux_xdp_set_control_map_state(libtrace, XDP_RUNNING);
        FORMAT_DATA->state = XDP_RUNNING;

        return 0;
    }

    /* single threaded operation, make sure the number of nic queues is 1 or
     * packets will be lost */
    c_nic_queues = linux_xdp_get_current_queues(FORMAT_DATA->cfg.ifname);

    if (c_nic_queues != 1) {
        /* set the number of nic queues to 1 */
        if (linux_xdp_set_current_queues(FORMAT_DATA->cfg.ifname, 1) < 0) {
            trace_set_err(libtrace, TRACE_ERR_INIT_FAILED, "Unable to set number "
                "of NIC queues to 1");
            return -1;
        }
    }

    /* insert empty stream into the list */
    libtrace_list_push_back(FORMAT_DATA->per_stream, &empty_stream);

    /* get the stream from the list */
    stream = libtrace_list_get_index(FORMAT_DATA->per_stream, 0)->data;

    /* start the stream */
    if ((ret = linux_xdp_start_stream(&FORMAT_DATA->cfg, stream, 0, 0)) != 0) {
        trace_set_err(libtrace, TRACE_ERR_INIT_FAILED,
            "Unable to start input stream: %s", strerror(ret));
        return -1;
    }

    /* update state to running */
    linux_xdp_set_control_map_state(libtrace, XDP_RUNNING);
    FORMAT_DATA->state = XDP_RUNNING;

    return 0;
}

static int linux_xdp_pause_input(libtrace_t * libtrace) {

    int ret;

    if (FORMAT_DATA->state == XDP_NOT_STARTED) {
        trace_set_err(libtrace, TRACE_ERR_BAD_STATE, "Call trace_start() before "
            "calling trace_pause()");
        return -1;
    }

    ret = linux_xdp_set_control_map_state(libtrace, XDP_PAUSED);
    FORMAT_DATA->state = XDP_PAUSED;

    /* linux_xdp_set_control_map_state will return 0 on success.
     * If the control map cannot be found -1 is returned.
     */

    return ret;
}

static int linux_xdp_start_output(libtrace_out_t *libtrace) {

    struct xsk_per_stream empty_stream = {NULL,NULL,0,0};
    struct xsk_per_stream *stream;
    int ret;

    /* insert empty stream into the list */
    libtrace_list_push_back(FORMAT_DATA->per_stream, &empty_stream);

    /* get the stream from the list */
    stream = libtrace_list_get_index(FORMAT_DATA->per_stream, 0)->data;

    /* start the stream */
    if ((ret = linux_xdp_start_stream(&FORMAT_DATA->cfg, stream, 0, 1)) != 0) {
        trace_set_err_out(libtrace, TRACE_ERR_INIT_FAILED,
            "Unable to start output stream: %s", strerror(ret));
        return -1;
    }

    return 0;
}

static int linux_xdp_start_stream(struct xsk_config *cfg,
                                  struct xsk_per_stream *stream,
                                  int ifqueue,
                                  int dir) {

    uint64_t pkt_buf_size;
    void *pkt_buf;

    /* Allocate memory for NUM_FRAMES of default XDP frame size */
    pkt_buf_size = NUM_FRAMES * FRAME_SIZE;
    pkt_buf = mmap(NULL, pkt_buf_size,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    /* setup umem */
    stream->umem = configure_xsk_umem(pkt_buf, pkt_buf_size, ifqueue);
    if (stream->umem == NULL) {
        return errno;
    }

    /* configure socket */
    stream->xsk = xsk_configure_socket(cfg, stream->umem, dir);
    if (stream->xsk == NULL) {
        return errno;
    }

    return 0;
}

static int linux_xdp_read_stream(libtrace_t *libtrace,
                                 libtrace_packet_t *packet[],
                                 struct xsk_per_stream *stream,
                                 size_t nb_packets) {

    unsigned int rcvd = 0;
    uint32_t idx_rx = 0;
    uint32_t pkt_len;
    uint64_t pkt_addr;
    uint8_t *pkt_buffer;
    unsigned int i;
    libtrace_xdp_meta_t *meta;
    struct pollfd fds;
    int ret;

    if (libtrace->format_data == NULL) {
        trace_set_err(libtrace, TRACE_ERR_BAD_FORMAT, "Trace format data missing, "
            "call trace_create() before calling trace_read_packet()");
        return -1;
    }

    /* free previously used frames */
    if (stream->prev_rcvd != 0) {
        xsk_ring_prod__submit(&stream->umem->fq, stream->prev_rcvd);
        xsk_ring_cons__release(&stream->xsk->rx, stream->prev_rcvd);
    }

    /* check nb_packets (request packets) is not larger than the max RX_BATCH_SIZE */
    if (nb_packets > RX_BATCH_SIZE) {
        nb_packets = RX_BATCH_SIZE;
    }

    fds.fd = xsk_socket__fd(stream->xsk->xsk);
    fds.events = POLLIN;

    /* try get nb_packets */
    while (rcvd < 1) {
        rcvd = xsk_ring_cons__peek(&stream->xsk->rx, nb_packets, &idx_rx);

        /* check if libtrace has halted */
        if ((ret = is_halted(libtrace)) != -1) {
            return ret;
        }

        /* was a packet received? if not poll for a short amount of time */
        if (rcvd < 1) {
            /* poll will return 0 on timeout or a positive on a event */
            ret = poll(&fds, 1, 500);

            /* poll encountered a error */
            if (ret < 0) {
                trace_set_err(libtrace, errno, "poll error() XDP");
                return -1;
            }
        }
    }

    for (i = 0; i < rcvd; i++) {

        /* got a packet. Get the address and length from the rx descriptor */
        pkt_addr = xsk_ring_cons__rx_desc(&stream->xsk->rx, idx_rx)->addr;
        pkt_len = xsk_ring_cons__rx_desc(&stream->xsk->rx, idx_rx)->len;

        /* get pointer to its contents, this gives us pointer to packet payload
         * and not the start of the headroom allocated?? */
        pkt_buffer = xsk_umem__get_data(stream->xsk->umem->buffer, pkt_addr);

        /* prepare the packet */
        packet[i]->buf_control = TRACE_CTRL_EXTERNAL;
        packet[i]->type = TRACE_RT_DATA_XDP;
        packet[i]->buffer = (uint8_t *)pkt_buffer - FRAME_HEADROOM;
        packet[i]->header = (uint8_t *)pkt_buffer - FRAME_HEADROOM;
        packet[i]->payload = pkt_buffer;
        packet[i]->trace = libtrace;
        packet[i]->error = 1;

        meta = (libtrace_xdp_meta_t *)packet[i]->buffer;
        meta->timestamp = linux_xdp_get_time(stream);
        meta->packet_len = pkt_len;

        /* next packet */
        idx_rx++;
    }

    /* set number of received packets on this batch */
    stream->prev_rcvd = rcvd;

    return rcvd;
}

static int linux_xdp_read_packet(libtrace_t *libtrace, libtrace_packet_t *packet) {

    struct xsk_per_stream *stream;
    libtrace_list_node_t *node;

    node = libtrace_list_get_index(FORMAT_DATA->per_stream, 0);
    if (node == NULL) {
        trace_set_err(libtrace, TRACE_ERR_INIT_FAILED, "Unable to get XDP "
            "input stream in linux_xdp_read_packet()");
        return -1;
    }

    stream = (struct xsk_per_stream *)node->data;

    return linux_xdp_read_stream(libtrace, &packet, stream, 1);

}

static int linux_xdp_pread_packets(libtrace_t *libtrace,
                                   libtrace_thread_t *thread,
                                   libtrace_packet_t **packets,
                                   size_t nb_packets) {

    int nb_rx;
    struct xsk_per_stream *stream = thread->format_data;

    nb_rx = linux_xdp_read_stream(libtrace, packets, stream, nb_packets);

    return nb_rx;
}

static int linux_xdp_write_packet(libtrace_out_t *libtrace,
                                  libtrace_packet_t *packet) {

    struct xsk_per_stream *stream;
    libtrace_list_node_t *node;
    uint32_t idx;
    struct xdp_desc *tx_desc;
    void *offset;
    uint32_t cap_len;

    /* can xdp write this type of packet? */
    if (!linux_xdp_can_write(packet)) {
        return 0;
    }

    if (libtrace->format_data == NULL) {
        trace_set_err_out(libtrace, TRACE_ERR_BAD_FORMAT, "Trace format data missing, "
            "call trace_create_output() before calling trace_write_packet()");
        return -1;
    }

    /* get stream data */
    node = libtrace_list_get_index(FORMAT_DATA->per_stream, 0);
    if (node == NULL) {
        trace_set_err_out(libtrace, TRACE_ERR_INIT_FAILED, "Unable to get XDP "
            "output stream in linux_xdp_write_packet()");
        return -1;
    }
    stream = (struct xsk_per_stream *)node->data;


    /* is there a free frame for the packet */
    while (xsk_ring_prod__reserve(&stream->xsk->tx, 1, &idx) != 1) {
        /* try free up some frames */
        linux_xdp_complete_tx(stream->xsk);
    }

    /* get the tx descriptor */
    tx_desc = xsk_ring_prod__tx_desc(&stream->xsk->tx, idx);

    cap_len = trace_get_capture_length(packet);

    /* get the offset to write packet to within the umem */
    offset = xsk_umem__get_data(stream->xsk->umem->buffer, tx_desc->addr);

    /* copy the packet */
    memcpy(offset, (char *)packet->payload, cap_len);
    /* set packet length */
    tx_desc->len = cap_len;

    /* submit the frame */
    xsk_ring_prod__submit(&stream->xsk->tx, 1);

    /* complete the transaction */
    linux_xdp_complete_tx(stream->xsk);

    return cap_len;
}

static int linux_xdp_prepare_packet(libtrace_t *libtrace UNUSED, libtrace_packet_t *packet,
    void *buffer, libtrace_rt_types_t rt_type, uint32_t flags) {

    if (packet->buffer != buffer && packet->buf_control == TRACE_CTRL_PACKET) {
        free(packet->buffer);
    }

    if ((flags & TRACE_PREP_OWN_BUFFER) == TRACE_PREP_OWN_BUFFER) {
        packet->buf_control = TRACE_CTRL_PACKET;
    } else {
        packet->buf_control = TRACE_CTRL_EXTERNAL;
    }

    packet->buf_control = TRACE_CTRL_EXTERNAL;
    packet->type = rt_type;
    packet->buffer = buffer;
    packet->header = buffer;
    packet->payload = (uint8_t *)buffer + FRAME_HEADROOM;

    return 0;
}

/* read a single packet if available */
static libtrace_eventobj_t linux_xdp_event(libtrace_t *libtrace,
                                           libtrace_packet_t *packet) {

    libtrace_eventobj_t event = {0,0,0.0,0};
    unsigned int rcvd = 0;
    uint32_t pkt_len;
    uint64_t pkt_addr;
    uint8_t *pkt_buffer;
    uint32_t idx_rx = 0;
    libtrace_xdp_meta_t *meta;
    struct xsk_per_stream *stream;
    libtrace_list_node_t *node;

    /* get stream data */
    node = libtrace_list_get_index(FORMAT_DATA->per_stream, 0);
    if (node == NULL) {
        trace_set_err(libtrace, TRACE_ERR_INIT_FAILED, "Unable to get XDP "
            "input stream in linux_xdp_read_packet()");

        /* cannot find stream data. this should never occur but just incase */
        event.type = TRACE_EVENT_SLEEP;
        event.seconds = 0.0001;
        event.size = 0;

        return event;
    }

    stream = (struct xsk_per_stream *)node->data;

    /* release any previously packets */
    if (stream->prev_rcvd != 0) {
        xsk_ring_prod__submit(&stream->umem->fq, 1);
        xsk_ring_cons__release(&stream->xsk->rx, 1);
    }

    /* is there a packet available? */
    rcvd = xsk_ring_cons__peek(&stream->xsk->rx, 1, &idx_rx);
    if (rcvd > 0) {

        /* got a packet. Get the address and length from the rx descriptor */
        pkt_addr = xsk_ring_cons__rx_desc(&stream->xsk->rx, idx_rx)->addr;
        pkt_len = xsk_ring_cons__rx_desc(&stream->xsk->rx, idx_rx)->len;

        /* get pointer to its contents, this gives us pointer to packet payload
         * and not the start of the headroom allocated?? */
        pkt_buffer = xsk_umem__get_data(stream->xsk->umem->buffer, pkt_addr);

        /* prepare the packet */
        packet->buf_control = TRACE_CTRL_EXTERNAL;
        packet->type = TRACE_RT_DATA_XDP;
        packet->buffer = (uint8_t *)pkt_buffer - FRAME_HEADROOM;
        packet->header = (uint8_t *)pkt_buffer - FRAME_HEADROOM;
        packet->payload = pkt_buffer;
        packet->trace = libtrace;
        packet->error = 1;

        meta = (libtrace_xdp_meta_t *)packet->buffer;
        meta->timestamp = linux_xdp_get_time(stream);
        meta->packet_len = pkt_len;

        event.type = TRACE_EVENT_PACKET;
        event.size = pkt_len;

        stream->prev_rcvd = 1;

    } else {
        /* We only want to sleep for a very short time - we are non-blocking */
        event.type = TRACE_EVENT_SLEEP;
        event.seconds = 0.0001;
        event.size = 0;

    }

    return event;
}

static int linux_xdp_destroy_streams(libtrace_list_t *streams) {

    size_t i;
    struct xsk_per_stream *stream;

    for (i = 0; i < libtrace_list_get_size(streams); i++) {
        stream = libtrace_list_get_index(streams, i)->data;

        if (stream->xsk != NULL) {
            xsk_socket__delete(stream->xsk->xsk);
        }

        if (stream->umem != NULL) {
            xsk_umem__delete(stream->umem->umem);
            free(stream->umem);
        }
    }

    return 0;
}

static int linux_xdp_fin_input(libtrace_t *libtrace) {

    if (FORMAT_DATA != NULL) {

        linux_xdp_destroy_streams(FORMAT_DATA->per_stream);

        /* destroy per stream list */
        libtrace_list_deinit(FORMAT_DATA->per_stream);

        /* unload the XDP program */
        xdp_link_detach(&FORMAT_DATA->cfg);

        if (FORMAT_DATA->cfg.bpf_filename != NULL) {
            free(FORMAT_DATA->cfg.bpf_filename);
        }

        if (FORMAT_DATA->cfg.bpf_progname != NULL) {
            free(FORMAT_DATA->cfg.bpf_progname);
        }

        free(FORMAT_DATA);
    }

    return 0;
}

static int linux_xdp_fin_output(libtrace_out_t *libtrace) {

    if (FORMAT_DATA != NULL) {
        linux_xdp_destroy_streams(FORMAT_DATA->per_stream);
        libtrace_list_deinit(FORMAT_DATA->per_stream);
        free(FORMAT_DATA);
    }

    return 0;
}


/* link per stream data with each threads format data */
static int linux_xdp_pregister_thread(libtrace_t *libtrace,
                               libtrace_thread_t *t,
                               bool reading) {

    /* test if they nic supports multiple queues? if not use a hasher thread to disperse
     * packets across processing threads?? */

    if (reading) {
        if (t->type == THREAD_PERPKT) {
            t->format_data = libtrace_list_get_index(FORMAT_DATA->per_stream, t->perpkt_num)->data;
            if (t->format_data == NULL) {
                trace_set_err(libtrace, TRACE_ERR_INIT_FAILED, "Too many threads registered");
                return -1;
            }
        }
    }

    return 0;
}

static libtrace_linktype_t linux_xdp_get_link_type(const libtrace_packet_t *packet UNUSED) {
    return TRACE_TYPE_ETH;
}

static struct timeval linux_xdp_get_timeval(const libtrace_packet_t *packet) {

    struct timeval tv;

    tv.tv_sec = PACKET_META->timestamp / (uint64_t) 1000000000;
    tv.tv_usec = (PACKET_META->timestamp % (uint64_t) 1000000000) / 1000;

    return tv;

}

static struct timespec linux_xdp_get_timespec(const libtrace_packet_t *packet) {

    struct timespec ts;

    ts.tv_sec = PACKET_META->timestamp / (uint64_t) 1000000000;
    ts.tv_nsec = PACKET_META->timestamp % (uint64_t) 1000000000;

    return ts;
}

static int linux_xdp_get_framing_length(const libtrace_packet_t *packet UNUSED) {
    return FRAME_SIZE;
}

static int linux_xdp_get_wire_length(const libtrace_packet_t *packet) {

    return PACKET_META->packet_len;
}

static int linux_xdp_get_capture_length(const libtrace_packet_t *packet) {

    return PACKET_META->packet_len;

}

static void linux_xdp_get_stats(libtrace_t *libtrace, libtrace_stat_t *stats) {

    /* check the libtrace_map was found within the XDP program, otherwise
     * let libtrace handle this */
    if (FORMAT_DATA->cfg.libtrace_map_fd <= 0) {
        return;
    }

    int map_fd = FORMAT_DATA->cfg.libtrace_map_fd;
    int ncpus = libbpf_num_possible_cpus();
    libtrace_xdp_t xdp[ncpus];
    int thread_count = libtrace->perpkt_thread_count;

    /* init stats */
    stats->accepted = 0;
    stats->dropped = 0;
    stats->received = 0;

    for (int i = 0; i < thread_count; i++) {

        if ((bpf_map_lookup_elem(map_fd, &i, xdp)) != 0) {
            return;
        }

        for (int j = 0; j < ncpus; j++) {

            /* add up stats from each cpu */
            stats->accepted += xdp[j].accepted_packets;
            stats->dropped += xdp[j].dropped_packets;
            stats->received += xdp[j].received_packets;
        }
    }

    return;
}

static void linux_xdp_get_thread_stats(libtrace_t *libtrace,
                                       libtrace_thread_t *thread,
                                       libtrace_stat_t *stats) {

    /* check the libtrace_map was found within the XDP program, otherwise
     * let libtrace handle this */
    if (FORMAT_DATA->cfg.libtrace_map_fd <= 0) {
        return;
    }

    int ncpus = libbpf_num_possible_cpus();
    int ifqueue;
    int map_fd;
    libtrace_xdp_t xdp[ncpus];
    struct xsk_per_stream *stream_data;

    /* get the nic queue number from the threads per stream data */
    stream_data = (struct xsk_per_stream *)thread->format_data;
    ifqueue = stream_data->umem->xsk_if_queue;

    map_fd = FORMAT_DATA->cfg.libtrace_map_fd;
    if (!map_fd) {
        return;
    }

    /* get the xdp libtrace map for this threads nic queue */
    if ((bpf_map_lookup_elem(map_fd, &ifqueue, &xdp)) != 0) {
        return;
    }

    /* init stats */
    stats->accepted = 0;
    stats->dropped = 0;
    stats->received = 0;

    /* add up stats from each cpu */
    for (int i = 0; i < ncpus; i++) {
        /* populate stats structure */
        stats->accepted += xdp[i].accepted_packets;
        stats->dropped += xdp[i].dropped_packets;
        stats->received += xdp[i].received_packets;
    }

    return;
}

static int linux_xdp_config_input(libtrace_t *libtrace,
                                  trace_option_t options,
                                  void *data) {

    switch (options) {
        case TRACE_OPTION_SNAPLEN:
        case TRACE_OPTION_PROMISC:
        case TRACE_OPTION_HASHER:
            switch (*((enum hasher_types *)data)) {
                case HASHER_BALANCE:
                case HASHER_UNIDIRECTIONAL:
                case HASHER_BIDIRECTIONAL:
                    FORMAT_DATA->hasher_type = *(enum hasher_types*)data;
                    return 0;
                case HASHER_CUSTOM:
                    /* libtrace can handle custom hashers */
                    return -1;
            }
            break;
        case TRACE_OPTION_FILTER:
        case TRACE_OPTION_META_FREQ:
        case TRACE_OPTION_DISCARD_META:
        case TRACE_OPTION_EVENT_REALTIME:
        case TRACE_OPTION_REPLAY_SPEEDUP:
        case TRACE_OPTION_CONSTANT_ERF_FRAMING:
            break;
    }

    return -1;
}

static void linux_xdp_help(void) {
    printf("XDP format module\n");
    printf("Supported input URIs:\n");
    printf("\txdp:interface\n");
    printf("\txdp:bpfprog:interface\n");
    printf("Supported output URIs:\n");
    printf("\txdp:interface\n");
    printf("\n");
}

static struct libtrace_format_t xdp = {
    "xdp",
    "$Id$",
    TRACE_FORMAT_XDP,
    NULL,                           /* probe filename */
    NULL,                           /* probe magic */
    linux_xdp_init_input,           /* init_input */
    linux_xdp_config_input,         /* config_input */
    linux_xdp_start_input,          /* start_input */
    linux_xdp_pause_input,          /* pause */
    linux_xdp_init_output,          /* init_output */
    NULL,                           /* config_output */
    linux_xdp_start_output,         /* start_output */
    linux_xdp_fin_input,            /* fin_input */
    linux_xdp_fin_output,           /* fin_output */
    linux_xdp_read_packet,          /* read_packet */
    linux_xdp_prepare_packet,       /* prepare_packet */
    NULL,                           /* fin_packet */
    linux_xdp_write_packet,         /* write_packet */
    NULL,                           /* flush_output */
    linux_xdp_get_link_type,        /* get_link_type */
    NULL,                           /* get_direction */
    NULL,                           /* set_direction */
    NULL,                           /* get_erf_timestamp */
    linux_xdp_get_timeval,          /* get_timeval */
    linux_xdp_get_timespec,         /* get_timespec */
    NULL,                           /* get_seconds */
	NULL,                           /* get_meta_section */
    NULL,                           /* seek_erf */
    NULL,                           /* seek_timeval */
    NULL,                           /* seek_seconds */
    linux_xdp_get_capture_length,   /* get_capture_length */
    linux_xdp_get_wire_length,      /* get_wire_length */
    linux_xdp_get_framing_length,   /* get_framing_length */
    NULL,                           /* set_capture_length */
    NULL,                           /* get_received_packets */
	NULL,                           /* get_filtered_packets */
    NULL,                           /* get_dropped_packets */
    linux_xdp_get_stats,            /* get_statistics */
    NULL,                           /* get_fd */
    linux_xdp_event,                /* trace_event */
    linux_xdp_help,                 /* help */
    NULL,                           /* next pointer */
    {true, -1},                     /* Live, no thread limit */
    linux_xdp_pstart_input,		    /* pstart_input */
    linux_xdp_pread_packets,	    /* pread_packets */
    linux_xdp_pause_input,          /* ppause */
    linux_xdp_fin_input,		    /* p_fin */
    linux_xdp_pregister_thread,	    /* register thread */
    NULL,				            /* unregister thread */
    linux_xdp_get_thread_stats      /* get thread stats */
};

void linux_xdp_constructor(void) {
    register_format(&xdp);
}

static int xdp_link_detach(struct xsk_config *cfg) {

    /* detach/unload the XDP program */
    if (bpf_set_link_xdp_fd(cfg->ifindex, -1, cfg->xdp_flags) < 0) {
        return EXIT_FAIL_XDP;
    }

    return EXIT_OK;
}

static struct bpf_object *load_bpf_object_file(struct xsk_config *cfg, int ifindex) {

    int first_prog_fd = -1;
    int err;

    /* This struct allow us to set ifindex, this features is used for
     * hardware offloading XDP programs (note this sets libbpf
     * bpf_program->prog_ifindex and foreach bpf_map->map_ifindex).
     */
    struct bpf_prog_load_attr prog_load_attr = {
        .prog_type = BPF_PROG_TYPE_XDP,
        .ifindex   = ifindex,
    };
    prog_load_attr.file = cfg->bpf_filename;

    /* Use libbpf for extracting BPF byte-code from BPF-ELF object, and
     * loading this into the kernel via bpf-syscall
     */
    err = bpf_prog_load_xattr(&prog_load_attr, &cfg->bpf_obj, &first_prog_fd);
    if (err) {
        fprintf(stderr, "ERR: loading BPF-OBJ file(%s) (%d): %s\n",
            cfg->bpf_filename, err, strerror(-err));
        return NULL;
    }

    return cfg->bpf_obj;
}

static int xdp_link_attach(struct xsk_config *cfg, int prog_fd) {

    int err;

    /* libbpf provide the XDP net_device link-level hook attach helper */
    err = bpf_set_link_xdp_fd(cfg->ifindex, prog_fd, cfg->xdp_flags);
    if (err == -EEXIST && !(cfg->xdp_flags & XDP_FLAGS_UPDATE_IF_NOEXIST)) {
        /* Force mode didn't work, probably because a program of the
         * opposite type is loaded. Let's unload that and try loading
         * again.
         */

        __u32 old_flags = cfg->xdp_flags;

        cfg->xdp_flags &= ~XDP_FLAGS_MODES;
        cfg->xdp_flags |= (old_flags & XDP_FLAGS_SKB_MODE) ? XDP_FLAGS_DRV_MODE : XDP_FLAGS_SKB_MODE;

        /* unload */
        err = bpf_set_link_xdp_fd(cfg->ifindex, -1, cfg->xdp_flags);
        if (!err)
            err = bpf_set_link_xdp_fd(cfg->ifindex, prog_fd, old_flags);
    }
    if (err < 0) {
        fprintf(stderr, "ERR: "
            "ifindex(%d) link set xdp fd failed (%d): %s\n",
            cfg->ifindex, -err, strerror(-err));
        return EXIT_FAIL_XDP;
    }

    return EXIT_OK;
}

static struct bpf_object *load_bpf_and_xdp_attach(struct xsk_config *cfg) {

    int err;
    int prog_fd;

    /* Load the BPF-ELF object file and get back libbpf bpf_object. Supply
     * ifindex to try offload to the NIC */
    cfg->bpf_obj = load_bpf_object_file(cfg, cfg->ifindex);
    if (!cfg->bpf_obj) {
        fprintf(stderr, "XDP hardware offload failed, trying without\n");

        /* hardware offload failed try again without */
        cfg->bpf_obj = load_bpf_object_file(cfg, 0);
        if (cfg->bpf_obj == NULL) {
            fprintf(stderr, "ERR: loading file: %s\n", cfg->bpf_filename);
            exit(EXIT_FAIL_BPF);
        }
    }

    /* At this point: All XDP/BPF programs from the bpf_filename have been
     * loaded into the kernel, and evaluated by the verifier. Only one of
     * these gets attached to XDP hook, the others will get freed once this
     * process exit.
     */
    cfg->bpf_prg = bpf_object__find_program_by_title(cfg->bpf_obj, cfg->bpf_progname);
    if (!cfg->bpf_prg) {
        fprintf(stderr, "ERR: couldn't find a program in ELF section '%s'\n", cfg->bpf_filename);
        exit(EXIT_FAIL_BPF);
    }

    prog_fd = bpf_program__fd(cfg->bpf_prg);
    if (prog_fd <= 0) {
        fprintf(stderr, "ERR: bpf_program__fd failed\n");
        exit(EXIT_FAIL_BPF);
    }
    cfg->bpf_prg_fd = prog_fd;

    /* At this point: BPF-progs are (only) loaded by the kernel, and prog_fd
     * is our select file-descriptor handle. Next step is attaching this FD
     * to a kernel hook point, in this case XDP net_device link-level hook.
     */
    err = xdp_link_attach(cfg, cfg->bpf_prg_fd);
    if (err) {
        exit(err);
    }

    return cfg->bpf_obj;
}
