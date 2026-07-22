#ifndef BCEDRIVER_TRANSFER_H
#define BCEDRIVER_TRANSFER_H

#include <linux/wait.h>
#include <linux/usb.h>
#include "queue.h"
#include "command.h"

struct bce_vhci_list_message {
    struct list_head list;
    struct bce_vhci_message msg;
};
enum bce_vhci_pause_source {
    BCE_VHCI_PAUSE_INTERNAL_WQ = 1,
    BCE_VHCI_PAUSE_FIRMWARE = 2,
    BCE_VHCI_PAUSE_SUSPEND = 4,
    BCE_VHCI_PAUSE_SHUTDOWN = 8
};
struct bce_vhci_transfer_queue {
    struct bce_vhci *vhci;
    struct usb_host_endpoint *endp;
    enum bce_vhci_endpoint_state state;
    u32 max_active_requests, remaining_active_requests;
    bool active, stalled;
    u32 paused_by;
    bce_vhci_device_t dev_addr;
    u8 endp_addr;
    struct t2bce_core_queue_cq *cq;
    struct t2bce_core_queue_sq *sq_in;
    struct t2bce_core_queue_sq *sq_out;
    struct list_head evq;
    struct spinlock urb_lock;
    struct mutex pause_lock;
    struct list_head giveback_urb_list;
    wait_queue_head_t sq_out_wait_queue;
    atomic_t sq_out_pending;

    struct work_struct w_reset;
    struct work_struct w_resume;
};
enum bce_vhci_urb_state {
    BCE_VHCI_URB_INIT_PENDING,
    BCE_VHCI_URB_WAITING_FOR_TRANSFER_REQUEST,
    BCE_VHCI_URB_WAITING_FOR_COMPLETION,
    BCE_VHCI_URB_DATA_TRANSFER_COMPLETE,
    BCE_VHCI_URB_CONTROL_WAITING_FOR_SETUP_REQUEST,
    BCE_VHCI_URB_CONTROL_WAITING_FOR_SETUP_COMPLETION,
    BCE_VHCI_URB_CONTROL_COMPLETE,
    BCE_VHCI_URB_CANCELLED
};
struct bce_vhci_urb {
    struct urb *urb;
    struct bce_vhci_transfer_queue *q;
    enum dma_data_direction dir;
    bool is_control;
    enum bce_vhci_urb_state state;
    int received_status;
    u32 send_offset;
    u32 receive_offset;
    struct t2bce_core_dma_segment *sg_segments;
    dma_addr_t sg_segments_dma;
    size_t sg_segments_size;
    unsigned int sg_segment_count;
    bool sg_segments_for_device;
};

void bce_vhci_create_transfer_queue(struct bce_vhci *vhci, struct bce_vhci_transfer_queue *q,
        struct usb_host_endpoint *endp, bce_vhci_device_t dev_addr, enum dma_data_direction dir);
void bce_vhci_destroy_transfer_queue(struct bce_vhci *vhci, struct bce_vhci_transfer_queue *q);
void bce_vhci_transfer_queue_event(struct bce_vhci_transfer_queue *q, struct bce_vhci_message *msg);
int bce_vhci_transfer_queue_do_pause(struct bce_vhci_transfer_queue *q);
int bce_vhci_transfer_queue_do_resume(struct bce_vhci_transfer_queue *q);
int bce_vhci_transfer_queue_pause(struct bce_vhci_transfer_queue *q, enum bce_vhci_pause_source src);
int bce_vhci_transfer_queue_resume(struct bce_vhci_transfer_queue *q, enum bce_vhci_pause_source src);
int bce_vhci_transfer_queue_suspend_pause(struct bce_vhci_transfer_queue *q);
void bce_vhci_transfer_queue_request_reset(struct bce_vhci_transfer_queue *q);

int bce_vhci_urb_create(struct bce_vhci_transfer_queue *q, struct urb *urb);
int bce_vhci_urb_request_cancel(struct bce_vhci_transfer_queue *q, struct urb *urb, int status);

#endif //BCEDRIVER_TRANSFER_H
