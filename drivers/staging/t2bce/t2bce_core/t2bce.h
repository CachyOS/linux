#ifndef APPLE_BCE_H
#define APPLE_BCE_H

#include <linux/pci.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/srcu.h>
#include "mailbox.h"
#include "t2bce_dma_queue.h"

#define BC_PROTOCOL_VERSION 0x20001

struct t2bce_device {
    struct pci_dev *pci, *pci0;
    dev_t devt;
    struct device *dev;
    void __iomem *reg_mem_mb;
    void __iomem *reg_mem_dma;
    struct t2bce_dma_engine dma;
    struct bce_mailbox mbox;
    struct bce_xhci_pm xhci_pm;
    bool is_being_removed;

    dma_addr_t saved_data_dma_addr;
    void *saved_data_dma_ptr;
    size_t saved_data_dma_size;
    u32 fw_version;
    bool stateful_suspend_valid;
    bool no_state_fallback;
    bool no_state_resume;
    bool mailbox_channel_active;
    struct mutex pm_lock;
    struct mutex clients_lock;
    struct list_head clients;
    struct srcu_struct clients_srcu;
    bool clients_srcu_ready;
};

extern struct t2bce_device *global_bce;

void t2bce_core_notify_resume_complete(struct t2bce_device *bce);
void t2bce_core_clients_shutdown(struct t2bce_device *bce);
void t2bce_core_clients_pm_reset(struct t2bce_device *bce);
void t2bce_core_clients_pm_prepare(struct t2bce_device *bce);
void t2bce_core_clients_pm_prepare_no_state(struct t2bce_device *bce);
void t2bce_core_clients_pm_mark_no_state_resume(struct t2bce_device *bce);
bool t2bce_core_clients_pm_has_no_state_resume(struct t2bce_device *bce);
void t2bce_core_clients_pm_complete(struct t2bce_device *bce);

#endif //APPLE_BCE_H
