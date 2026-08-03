#include "vhci.h"
#include "command.h"
#include <linux/delay.h>
#include <linux/usb.h>
#include <linux/usb/hcd.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/err.h>
#include <linux/slab.h>

/*
 * bridgeOS exposes several T2-internal devices through a virtual USB 2.0 hub.
 * This HCD translates usbcore hub, device and endpoint operations into the
 * firmware command/event protocol carried over BCE queues.
 */

static dev_t bce_vhci_chrdev;
static struct class *bce_vhci_class;
static const struct hc_driver bce_vhci_driver;
static u16 bce_vhci_port_mask = U16_MAX;
static struct bce_vhci *global_vhci;

enum bce_vhci_port_status_bits {
    BCE_VHCI_PORT_STATUS_OVERCURRENT   = 0x2,
    BCE_VHCI_PORT_STATUS_CONNECTED     = 0x4,
    BCE_VHCI_PORT_STATUS_RESET         = 0x8,
    BCE_VHCI_PORT_STATUS_ENABLED       = 0x10,
    BCE_VHCI_PORT_STATUS_SUSPENDED_LEGACY = 0x60,
    BCE_VHCI_PORT_STATUS_SUSPENDED     = 0x80,
    BCE_VHCI_PORT_STATUS_HIGH_SPEED    = 0x200,
    BCE_VHCI_PORT_STATUS_C_CONNECTION  = 0x40000,
};

static int bce_vhci_create_event_queues(struct bce_vhci *vhci);
static void bce_vhci_destroy_event_queues(struct bce_vhci *vhci);
static int bce_vhci_create_message_queues(struct bce_vhci *vhci);
static void bce_vhci_destroy_message_queues(struct bce_vhci *vhci);
static void bce_vhci_handle_firmware_events_w(struct work_struct *ws);
static void bce_vhci_add_hcd_w(struct work_struct *ws);
static void bce_vhci_firmware_event_completion(struct t2bce_core_queue_sq *sq);
static int bce_vhci_start_controller(struct bce_vhci *vhci);
static void bce_vhci_forget_devices(struct bce_vhci *vhci);
static int __bce_vhci_add_hcd(struct bce_vhci *vhci);
static void bce_vhci_shutdown_client(void *userdata);
static void bce_vhci_pm_reset_client(void *userdata);
static void bce_vhci_pm_prepare_client(void *userdata);
static void bce_vhci_pm_prepare_no_state_client(void *userdata);
static void bce_vhci_pm_mark_no_state_resume_client(void *userdata);
static bool bce_vhci_pm_is_no_state_resume_client(void *userdata);
static void bce_vhci_pm_complete_client(void *userdata);

static const struct t2bce_core_client_pm_ops bce_vhci_pm_ops = {
        .shutdown = bce_vhci_shutdown_client,
        .pm_reset = bce_vhci_pm_reset_client,
        .pm_prepare = bce_vhci_pm_prepare_client,
        .pm_prepare_no_state = bce_vhci_pm_prepare_no_state_client,
        .pm_mark_no_state_resume = bce_vhci_pm_mark_no_state_resume_client,
        .pm_is_no_state_resume = bce_vhci_pm_is_no_state_resume_client,
        .pm_complete = bce_vhci_pm_complete_client,
};

int bce_vhci_create(struct device *parent, struct bce_vhci *vhci)
{
    int status;

    spin_lock_init(&vhci->hcd_spinlock);

    vhci->vdevt = bce_vhci_chrdev;
    vhci->vdev = device_create(bce_vhci_class, parent, vhci->vdevt, NULL, "t2bce_vhci");
    if (IS_ERR_OR_NULL(vhci->vdev)) {
        status = PTR_ERR(vhci->vdev);
        goto fail_dev;
    }
    vhci->client = t2bce_core_client_get(vhci->vdev);
    if (IS_ERR(vhci->client)) {
        status = PTR_ERR(vhci->client);
        vhci->client = NULL;
        goto fail_client;
    }
    t2bce_core_client_set_pm_ops(vhci->client, &bce_vhci_pm_ops, vhci);

    if ((status = bce_vhci_create_message_queues(vhci)))
        goto fail_mq;
    if ((status = bce_vhci_create_event_queues(vhci)))
        goto fail_eq;

    vhci->tq_state_wq = alloc_ordered_workqueue("t2bce_vhci-tq-state", 0);
    INIT_WORK(&vhci->w_fw_events, bce_vhci_handle_firmware_events_w);
    INIT_WORK(&vhci->w_add_hcd, bce_vhci_add_hcd_w);
    vhci->port_change_pending = 0;
    vhci->stateful_suspended_bounce = 0;
    vhci->stateful_resume = false;

    vhci->hcd = usb_create_hcd(&bce_vhci_driver, vhci->vdev, "t2bce_vhci");
    if (!vhci->hcd) {
        status = -ENOMEM;
        goto fail_hcd;
    }
    vhci->hcd->self.sysdev = t2bce_core_client_dma_dev(vhci->client);
#if LINUX_VERSION_CODE < KERNEL_VERSION(5,4,0)
    vhci->hcd->self.uses_dma = 1;
#endif
    *((struct bce_vhci **) vhci->hcd->hcd_priv) = vhci;
    vhci->hcd->speed = HCD_USB2;

    if ((status = __bce_vhci_add_hcd(vhci)))
        goto fail_hcd;

    pr_info("t2bce_vhci: initialized\n");
    return 0;

fail_hcd:
    bce_vhci_destroy_event_queues(vhci);
fail_eq:
    bce_vhci_destroy_message_queues(vhci);
fail_mq:
    t2bce_core_client_set_pm_ops(vhci->client, NULL, NULL);
    t2bce_core_client_put(vhci->client);
    vhci->client = NULL;
fail_client:
    device_destroy(bce_vhci_class, vhci->vdevt);
fail_dev:
    if (!status)
        status = -EINVAL;
    return status;
}

void bce_vhci_destroy(struct bce_vhci *vhci)
{
    cancel_work_sync(&vhci->w_add_hcd);
    bce_vhci_remove_hcd(vhci);
    bce_vhci_destroy_event_queues(vhci);
    bce_vhci_destroy_message_queues(vhci);
    t2bce_core_client_set_pm_ops(vhci->client, NULL, NULL);
    t2bce_core_client_put(vhci->client);
    vhci->client = NULL;
    device_destroy(bce_vhci_class, vhci->vdevt);
}

static int __bce_vhci_add_hcd(struct bce_vhci *vhci)
{
    int status;

    status = usb_add_hcd(vhci->hcd, 0, 0);
    if (!status)
        vhci->hcd_registered = true;
    return status;
}

int bce_vhci_add_hcd(struct bce_vhci *vhci)
{
    if (vhci->hcd_registered)
        return 0;
    return __bce_vhci_add_hcd(vhci);
}

void bce_vhci_remove_hcd(struct bce_vhci *vhci)
{
    cancel_work_sync(&vhci->w_add_hcd);
    if (!vhci->hcd_registered)
        return;
    usb_remove_hcd(vhci->hcd);
    vhci->hcd_registered = false;
}

void bce_vhci_shutdown(struct bce_vhci *vhci)
{
    cancel_work_sync(&vhci->w_add_hcd);
    if (vhci->hcd_registered) {
        bce_vhci_remove_hcd(vhci);
        return;
    }

    /*
     * Shutdown is not remove: the USB core may already have detached the HCD,
     * but the T2-side controller can still be left enabled. Send the controller
     * disable directly as a final lights-out command while queues still exist.
     */
    bce_vhci_cmd_controller_disable(&vhci->cq);
}

void bce_vhci_pm_reset(struct bce_vhci *vhci)
{
    vhci->no_state_resume = false;
}

void bce_vhci_pm_prepare(struct bce_vhci *vhci)
{
    cancel_work_sync(&vhci->w_add_hcd);
    vhci->port_change_pending = 0;
}

void bce_vhci_pm_prepare_no_state(struct bce_vhci *vhci)
{
    bce_vhci_remove_hcd(vhci);
}

void bce_vhci_pm_mark_no_state_resume(struct bce_vhci *vhci)
{
    vhci->no_state_resume = true;
}

bool bce_vhci_pm_is_no_state_resume(struct bce_vhci *vhci)
{
    return vhci->no_state_resume;
}

void bce_vhci_pm_complete(struct bce_vhci *vhci)
{
    if (!vhci->no_state_resume)
        return;

    /* Re-add the VHCI HCD after the PM core completed resume ordering. */
    pr_debug("t2bce_vhci: scheduling HCD re-add after no-state wake\n");
    queue_work(vhci->tq_state_wq, &vhci->w_add_hcd);
}

static void bce_vhci_pm_reset_client(void *userdata)
{
    bce_vhci_pm_reset(userdata);
}

static void bce_vhci_pm_prepare_client(void *userdata)
{
    bce_vhci_pm_prepare(userdata);
}

static void bce_vhci_shutdown_client(void *userdata)
{
    bce_vhci_shutdown(userdata);
}

static void bce_vhci_pm_prepare_no_state_client(void *userdata)
{
    bce_vhci_pm_prepare_no_state(userdata);
}

static void bce_vhci_pm_mark_no_state_resume_client(void *userdata)
{
    bce_vhci_pm_mark_no_state_resume(userdata);
}

static bool bce_vhci_pm_is_no_state_resume_client(void *userdata)
{
    return bce_vhci_pm_is_no_state_resume(userdata);
}

static void bce_vhci_pm_complete_client(void *userdata)
{
    bce_vhci_pm_complete(userdata);
}

static void bce_vhci_add_hcd_w(struct work_struct *ws)
{
    struct bce_vhci *vhci = container_of(ws, struct bce_vhci, w_add_hcd);
    int status;

    pr_debug("t2bce_vhci: deferred HCD add after no-state wake\n");
    status = bce_vhci_add_hcd(vhci);
    if (status) {
        pr_err("t2bce_vhci: deferred HCD add failed: %d\n", status);
        return;
    }

    vhci->no_state_resume = false;
}

struct bce_vhci *bce_vhci_from_hcd(struct usb_hcd *hcd)
{
    return *((struct bce_vhci **) hcd->hcd_priv);
}

int bce_vhci_start(struct usb_hcd *hcd)
{
    struct bce_vhci *vhci = bce_vhci_from_hcd(hcd);
    return bce_vhci_start_controller(vhci);
}

void bce_vhci_stop(struct usb_hcd *hcd)
{
    struct bce_vhci *vhci = bce_vhci_from_hcd(hcd);
    bce_vhci_cmd_controller_disable(&vhci->cq);
}

static int bce_vhci_start_controller(struct bce_vhci *vhci)
{
    int status;
    u16 port_mask = 0;
    bce_vhci_port_t port_no = 0;

    if ((status = bce_vhci_cmd_controller_enable(&vhci->cq, 1, &port_mask)))
        return status;
    vhci->port_mask = port_mask;
    vhci->port_power_mask = 0;
    if ((status = bce_vhci_cmd_controller_start(&vhci->cq)))
        return status;
    while (port_mask) {
        port_no += 1;
        port_mask >>= 1;
    }
    vhci->port_count = port_no;
    return 0;
}

static void bce_vhci_forget_devices(struct bce_vhci *vhci)
{
    int i, j;
    bce_vhci_device_t devid;
    struct bce_vhci_device *dev;

    for (i = 0; i < 16; i++) {
        devid = vhci->port_to_device[i];
        if (!devid)
            continue;

        dev = vhci->devices[devid];
        if (!dev) {
            vhci->port_to_device[i] = 0;
            continue;
        }

        for (j = 0; j < 32; j++) {
            if (!(dev->tq_mask & BIT(j)))
                continue;
            if (dev->tq[j].endp)
                dev->tq[j].endp->hcpriv = NULL;
            bce_vhci_destroy_transfer_queue(vhci, &dev->tq[j]);
        }

        dev->tq_mask = 0;
        vhci->devices[devid] = NULL;
        vhci->port_to_device[i] = 0;
        kfree(dev);
    }
}

static int bce_vhci_hub_status_data(struct usb_hcd *hcd, char *buf)
{
    struct bce_vhci *vhci = bce_vhci_from_hcd(hcd);
    unsigned long pending;

    /* Only ports reported by firmware as changed should wake the hub state machine. */
    pending = READ_ONCE(vhci->port_change_pending);
    if (!pending)
        return 0;

    memset(buf, 0, 2);
    buf[0] = (char)((pending << 1) & 0xff);
    buf[1] = (char)((pending >> 7) & 0xff);
    return buf[0] || buf[1];
}

static int bce_vhci_reset_device(struct bce_vhci *vhci, int index, u16 timeout);

static int bce_vhci_hub_control(struct usb_hcd *hcd, u16 typeReq, u16 wValue, u16 wIndex, char *buf, u16 wLength)
{
    struct bce_vhci *vhci = bce_vhci_from_hcd(hcd);
    int status;
    struct usb_hub_descriptor *hd;
    struct usb_hub_status *hs;
    struct usb_port_status *ps;
    u32 port_status;
    u32 raw_port_status;
    if (typeReq == GetHubDescriptor && wLength >= sizeof(struct usb_hub_descriptor)) {
        hd = (struct usb_hub_descriptor *) buf;
        memset(hd, 0, sizeof(*hd));
        hd->bDescLength = sizeof(struct usb_hub_descriptor);
        hd->bDescriptorType = USB_DT_HUB;
        hd->bNbrPorts = (u8) vhci->port_count;
        hd->wHubCharacteristics = HUB_CHAR_INDV_PORT_LPSM | HUB_CHAR_INDV_PORT_OCPM;
        hd->bPwrOn2PwrGood = 0;
        hd->bHubContrCurrent = 0;
        return 0;
    } else if (typeReq == GetHubStatus && wLength >= sizeof(struct usb_hub_status)) {
        hs = (struct usb_hub_status *) buf;
        memset(hs, 0, sizeof(*hs));
        hs->wHubStatus = 0;
        hs->wHubChange = 0;
        return 0;
    } else if (typeReq == GetPortStatus && wLength >= 4 /* usb 2.0 */) {
        ps = (struct usb_port_status *) buf;
        ps->wPortStatus = 0;
        ps->wPortChange = 0;

        if (vhci->port_power_mask & BIT(wIndex))
            ps->wPortStatus |= USB_PORT_STAT_POWER;

        if (!(bce_vhci_port_mask & BIT(wIndex)))
            return 0;

        if ((status = bce_vhci_cmd_port_status(&vhci->cq, (u8) wIndex, 0, &port_status)))
            return status;
        raw_port_status = port_status;

        /*
         * During stateful resume the T2 can briefly report a disconnect bounce
         * for an already known virtual port. Keep usbcore from treating that
         * transient as a real unplug; the follow-up status/event decides the
         * actual port state.
         */
        if (vhci->stateful_resume &&
            (port_status & BCE_VHCI_PORT_STATUS_C_CONNECTION) &&
            !(port_status & BCE_VHCI_PORT_STATUS_CONNECTED) &&
            wIndex < ARRAY_SIZE(vhci->port_to_device) &&
            vhci->port_to_device[wIndex]) {
            port_status |= BCE_VHCI_PORT_STATUS_CONNECTED |
                           BCE_VHCI_PORT_STATUS_ENABLED |
                           BCE_VHCI_PORT_STATUS_SUSPENDED;
            pr_debug("t2bce_vhci: hub masked stateful disconnect bounce port=%u raw=%x masked_raw=%x\n",
                    wIndex, raw_port_status, port_status);
        }
        if (port_status & BCE_VHCI_PORT_STATUS_ENABLED)
            ps->wPortStatus |= USB_PORT_STAT_ENABLE;
        if (port_status & BCE_VHCI_PORT_STATUS_CONNECTED)
            ps->wPortStatus |= USB_PORT_STAT_CONNECTION;
        if (port_status & BCE_VHCI_PORT_STATUS_OVERCURRENT)
            ps->wPortStatus |= USB_PORT_STAT_OVERCURRENT;
        if (port_status & BCE_VHCI_PORT_STATUS_RESET)
            ps->wPortStatus |= USB_PORT_STAT_RESET;
        if (port_status & BCE_VHCI_PORT_STATUS_HIGH_SPEED)
            ps->wPortStatus |= USB_PORT_STAT_HIGH_SPEED;
        if (port_status & (BCE_VHCI_PORT_STATUS_SUSPENDED |
                           BCE_VHCI_PORT_STATUS_SUSPENDED_LEGACY)) {
            ps->wPortStatus |= USB_PORT_STAT_SUSPEND;
            if (vhci->stateful_resume && (port_status & BCE_VHCI_PORT_STATUS_CONNECTED))
                ps->wPortStatus |= USB_PORT_STAT_ENABLE;
        }

        /*
         * These ports are fixed T2-internal devices, not user-pluggable ports.
         * For stateful resume, suppress firmware connection-change bounces so
         * usbcore does not re-enumerate solely because the T2 toggled its
         * internal representation while restoring state.
         */
        if (vhci->stateful_resume && (port_status & BCE_VHCI_PORT_STATUS_C_CONNECTION)) {
            u32 cleared_status = 0;

            clear_bit(wIndex - 1, &vhci->port_change_pending);
            status = bce_vhci_cmd_port_status(&vhci->cq, (u8) wIndex, 0x40000, &cleared_status);
            pr_debug("t2bce_vhci: hub suppressed C_CONNECTION port=%u raw=%x clear_status=%d cleared_raw=%x\n",
                    wIndex, port_status, status, cleared_status);
        } else if (port_status & BCE_VHCI_PORT_STATUS_C_CONNECTION) {
            ps->wPortChange |= USB_PORT_STAT_C_CONNECTION;
        }
        pr_debug("t2bce_vhci: hub GetPortStatus port=%u raw=%x status=%x change=%x\n",
                wIndex, port_status, ps->wPortStatus, ps->wPortChange);
        return 0;
    } else if (typeReq == SetPortFeature) {
        if (wValue == USB_PORT_FEAT_POWER) {
            status = bce_vhci_cmd_port_power_on(&vhci->cq, (u8) wIndex);
            /* usbcore tracks logical power state separately from bridgeOS. */
            if (!status)
                vhci->port_power_mask |= BIT(wIndex);
            return status;
        }
        if (wValue == USB_PORT_FEAT_RESET) {
            pr_debug("t2bce_vhci: hub SetPortFeature RESET port=%u\n", wIndex);
            return bce_vhci_reset_device(vhci, wIndex, wValue);
        }
        if (wValue == USB_PORT_FEAT_SUSPEND) {
            /* Trace the usbcore hub state machine against the firmware port commands. */
            pr_debug("t2bce_vhci: hub SetPortFeature SUSPEND port=%u\n", wIndex);
            if (vhci->system_suspending) {
                pr_debug("t2bce_vhci: hub SetPortFeature SUSPEND port=%u skipped during system suspend\n",
                        wIndex);
                return 0;
            }
            status = bce_vhci_cmd_port_suspend(&vhci->cq, (u8) wIndex);
            pr_debug("t2bce_vhci: hub SetPortFeature SUSPEND port=%u -> %d\n", wIndex, status);
            return status;
        }
    } else if (typeReq == ClearPortFeature) {
        if (wValue == USB_PORT_FEAT_ENABLE)
            return bce_vhci_cmd_port_disable(&vhci->cq, (u8) wIndex);
        if (wValue == USB_PORT_FEAT_POWER) {
            status = bce_vhci_cmd_port_power_off(&vhci->cq, (u8) wIndex);
            if (!status)
                vhci->port_power_mask &= ~BIT(wIndex);
            return status;
        }
        if (wValue == USB_PORT_FEAT_C_CONNECTION) {
            pr_debug("t2bce_vhci: hub ClearPortFeature C_CONNECTION port=%u\n", wIndex);
            clear_bit(wIndex - 1, &vhci->port_change_pending);
            return bce_vhci_cmd_port_status(&vhci->cq, (u8) wIndex, 0x40000, &port_status);
        }
        if (wValue == USB_PORT_FEAT_C_RESET) {
            pr_debug("t2bce_vhci: hub ClearPortFeature C_RESET port=%u\n", wIndex);
            return 0;
        }
        if (wValue == USB_PORT_FEAT_SUSPEND) {
            pr_debug("t2bce_vhci: hub ClearPortFeature SUSPEND port=%u\n", wIndex);
            status = bce_vhci_cmd_port_resume(&vhci->cq, (u8) wIndex);
            pr_debug("t2bce_vhci: hub ClearPortFeature SUSPEND port=%u -> %d\n", wIndex, status);
            return status;
        }
    }
    pr_err("t2bce_vhci: bce_vhci_hub_control unhandled request: %x %i %i [bufl=%i]\n", typeReq, wValue, wIndex, wLength);
    dump_stack();
    return -EIO;
}

static int bce_vhci_enable_device(struct usb_hcd *hcd, struct usb_device *udev)
{
    struct bce_vhci *vhci = bce_vhci_from_hcd(hcd);
    struct bce_vhci_device *vdev;
    bce_vhci_device_t devid;
    pr_debug("t2bce_vhci: enable_device port=%u\n", udev->portnum);

    if (vhci->port_to_device[udev->portnum])
        return 0;

    /* bridgeOS requires a firmware device id before endpoints are created. */
    if (bce_vhci_cmd_device_create(&vhci->cq, udev->portnum, &devid))
        return -EIO;

    pr_debug("t2bce_vhci: device_create port=%u dev=%u\n", udev->portnum, devid);

    vdev = kzalloc(sizeof(struct bce_vhci_device), GFP_KERNEL);
    vhci->port_to_device[udev->portnum] = devid;
    vhci->devices[devid] = vdev;

    bce_vhci_create_transfer_queue(vhci, &vdev->tq[0], &udev->ep0, devid, DMA_BIDIRECTIONAL);
    udev->ep0.hcpriv = &vdev->tq[0];
    vdev->tq_mask |= BIT(0);

    bce_vhci_cmd_endpoint_create(&vhci->cq, devid, &udev->ep0.desc);
    return 0;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(6,8,0)
static int bce_vhci_address_device(struct usb_hcd *hcd, struct usb_device *udev)
#else
static int bce_vhci_address_device(struct usb_hcd *hcd, struct usb_device *udev, unsigned int timeout_ms)
#endif
{
    /*
     * Older kernels create the firmware device from enable_device(); newer
     * kernels use address_device() for the same bridgeOS operation.  The
     * firmware command path does not currently consume the usbcore timeout.
     */
    return bce_vhci_enable_device(hcd, udev);
}

static void bce_vhci_free_device(struct usb_hcd *hcd, struct usb_device *udev)
{
    struct bce_vhci *vhci = bce_vhci_from_hcd(hcd);
    int i;
    bce_vhci_device_t devid;
    struct bce_vhci_device *dev;
    pr_debug("t2bce_vhci: free_device port=%u\n", udev->portnum);
    if (!vhci->port_to_device[udev->portnum])
        return;
    devid = vhci->port_to_device[udev->portnum];
    dev = vhci->devices[devid];
    for (i = 0; i < 32; i++) {
        if (dev->tq_mask & BIT(i)) {
            bce_vhci_transfer_queue_pause(&dev->tq[i], BCE_VHCI_PAUSE_SHUTDOWN);
            bce_vhci_cmd_endpoint_destroy(&vhci->cq, devid, dev->tq[i].endp_addr);
            if (dev->tq[i].endp)
                dev->tq[i].endp->hcpriv = NULL;
            bce_vhci_destroy_transfer_queue(vhci, &dev->tq[i]);
        }
    }
    vhci->devices[devid] = NULL;
    vhci->port_to_device[udev->portnum] = 0;
    bce_vhci_cmd_device_destroy(&vhci->cq, devid);
    kfree(dev);
}

static int bce_vhci_reset_device(struct bce_vhci *vhci, int index, u16 timeout)
{
    struct bce_vhci_device *dev = NULL;
    bce_vhci_device_t devid;
    int i;
    int status;
    enum dma_data_direction dir;
    pr_debug("t2bce_vhci: reset_device port=%d\n", index);

    devid = vhci->port_to_device[index];
    if (devid) {
        dev = vhci->devices[devid];

        for (i = 0; i < 32; i++) {
            if (dev->tq_mask & BIT(i)) {
                bce_vhci_transfer_queue_pause(&dev->tq[i], BCE_VHCI_PAUSE_SHUTDOWN);
                bce_vhci_cmd_endpoint_destroy(&vhci->cq, devid, dev->tq[i].endp_addr);
                if (dev->tq[i].endp)
                    dev->tq[i].endp->hcpriv = NULL;
                bce_vhci_destroy_transfer_queue(vhci, &dev->tq[i]);
            }
        }
        vhci->devices[devid] = NULL;
        vhci->port_to_device[index] = 0;
        bce_vhci_cmd_device_destroy(&vhci->cq, devid);
    }
    status = bce_vhci_cmd_port_reset(&vhci->cq, (u8) index, timeout);

    if (dev) {
        if ((status = bce_vhci_cmd_device_create(&vhci->cq, index, &devid)))
            return status;
        vhci->devices[devid] = dev;
        vhci->port_to_device[index] = devid;

        for (i = 0; i < 32; i++) {
            if (dev->tq_mask & BIT(i)) {
                dir = usb_endpoint_dir_in(&dev->tq[i].endp->desc) ? DMA_FROM_DEVICE : DMA_TO_DEVICE;
                if (i == 0)
                    dir = DMA_BIDIRECTIONAL;
                bce_vhci_create_transfer_queue(vhci, &dev->tq[i], dev->tq[i].endp, devid, dir);
                dev->tq[i].endp->hcpriv = &dev->tq[i];
                bce_vhci_cmd_endpoint_create(&vhci->cq, devid, &dev->tq[i].endp->desc);
            }
        }
    }

    return status;
}

static int bce_vhci_check_bandwidth(struct usb_hcd *hcd, struct usb_device *udev)
{
    return 0;
}

static int bce_vhci_get_frame_number(struct usb_hcd *hcd)
{
    return 0;
}

static int bce_vhci_suspend_quiesce(struct usb_hcd *hcd)
{
    int i, j;
    int status;
    struct bce_vhci *vhci = bce_vhci_from_hcd(hcd);
    struct bce_vhci_transfer_queue *tq;
    struct urb *urb;
    struct bce_vhci_urb *vurb;
    unsigned long flags;

    pr_debug("t2bce_vhci: suspend started\n");

    pr_debug("t2bce_vhci: suspend endpoints\n");
    for (i = 0; i < 16; i++) {
        if (!vhci->port_to_device[i])
            continue;
        for (j = 0; j < 32; j++) {
            if (!(vhci->devices[vhci->port_to_device[i]]->tq_mask & BIT(j)))
                continue;
            tq = &vhci->devices[vhci->port_to_device[i]]->tq[j];
            if (tq->endp_addr == 0x00 || (usb_endpoint_xfer_int(&tq->endp->desc) &&
                                          usb_endpoint_dir_in(&tq->endp->desc))) {
                spin_lock_irqsave(&tq->urb_lock, flags);
                if (!list_empty(&tq->endp->urb_list)) {
                    urb = list_first_entry(&tq->endp->urb_list, struct urb, urb_list);
                    vurb = urb->hcpriv;
                    pr_debug("t2bce_vhci: suspend tq pre: dev=%u ep=%02x active=%u paused_by=%x stalled=%u has_urb=1 first_state=%x\n",
                            tq->dev_addr, tq->endp_addr, tq->active, tq->paused_by, tq->stalled,
                            vurb ? vurb->state : 0xffffffff);
                } else {
                    pr_debug("t2bce_vhci: suspend tq pre: dev=%u ep=%02x active=%u paused_by=%x stalled=%u has_urb=0\n",
                            tq->dev_addr, tq->endp_addr, tq->active, tq->paused_by, tq->stalled);
                }
                spin_unlock_irqrestore(&tq->urb_lock, flags);
            }

            bce_vhci_transfer_queue_suspend_pause(tq);

            if (tq->endp_addr == 0x00 || (usb_endpoint_xfer_int(&tq->endp->desc) &&
                                          usb_endpoint_dir_in(&tq->endp->desc))) {
                spin_lock_irqsave(&tq->urb_lock, flags);
                if (!list_empty(&tq->endp->urb_list)) {
                    urb = list_first_entry(&tq->endp->urb_list, struct urb, urb_list);
                    vurb = urb->hcpriv;
                    pr_debug("t2bce_vhci: suspend tq post: dev=%u ep=%02x active=%u paused_by=%x stalled=%u has_urb=1 first_state=%x\n",
                            tq->dev_addr, tq->endp_addr, tq->active, tq->paused_by, tq->stalled,
                            vurb ? vurb->state : 0xffffffff);
                } else {
                    pr_debug("t2bce_vhci: suspend tq post: dev=%u ep=%02x active=%u paused_by=%x stalled=%u has_urb=0\n",
                            tq->dev_addr, tq->endp_addr, tq->active, tq->paused_by, tq->stalled);
                }
                spin_unlock_irqrestore(&tq->urb_lock, flags);
            }
        }
    }

    /*
     * Only occupied virtual ports are suspended.  bridgeOS rejects holes in
     * the virtual hub, and that rejection would abort system PM.
     */
    pr_debug("t2bce_vhci: suspend occupied ports\n");
    for (i = 1; i < ARRAY_SIZE(vhci->port_to_device); i++) {
        if (!vhci->port_to_device[i])
            continue;

        status = bce_vhci_cmd_port_suspend(&vhci->cq, (u8)i);
        pr_debug("t2bce_vhci: suspend port %d -> %d\n", i, status);
        if (status)
            return status;
    }

    /* Keep Linux-side event queues quiet before the BCE state-save transition. */
    bce_vhci_event_queue_pause(&vhci->ev_commands);
    bce_vhci_event_queue_pause(&vhci->ev_system);
    bce_vhci_event_queue_pause(&vhci->ev_isochronous);
    bce_vhci_event_queue_pause(&vhci->ev_interrupt);
    bce_vhci_event_queue_pause(&vhci->ev_asynchronous);
    pr_debug("t2bce_vhci: suspend done\n");
    return 0;
}

static int bce_vhci_suspend_prepare(struct usb_hcd *hcd)
{
    /*
     * VHCI must quiesce usbcore-visible state before the core BCE mailbox
     * suspend command asks bridgeOS to save or discard queue state.
     */
    return bce_vhci_suspend_quiesce(hcd);
}

static int bce_vhci_bus_suspend(struct usb_hcd *hcd)
{
    int status;
    struct bce_vhci *vhci = bce_vhci_from_hcd(hcd);

    pr_debug("t2bce_vhci: bus_suspend entry\n");
    vhci->port_change_pending = 0;
    vhci->system_suspending = true;
    status = bce_vhci_suspend_prepare(hcd);
    if (status)
        vhci->system_suspending = false;
    pr_info("t2bce_vhci: bus_suspend exit status=%d\n", status);
    return status;
}

static int bce_vhci_resume_no_state(struct usb_hcd *hcd)
{
    int i;
    int status;
    struct bce_vhci *vhci = bce_vhci_from_hcd(hcd);

    bce_vhci_event_queue_resume(&vhci->ev_system);
    bce_vhci_event_queue_resume(&vhci->ev_isochronous);
    bce_vhci_event_queue_resume(&vhci->ev_interrupt);
    bce_vhci_event_queue_resume(&vhci->ev_asynchronous);
    bce_vhci_event_queue_resume(&vhci->ev_commands);

    pr_debug("t2bce_vhci: no-state resume, rebuilding controller state\n");
    vhci->stateful_resume = false;
    bce_vhci_forget_devices(vhci);
    status = bce_vhci_start_controller(vhci);
    if (status) {
        pr_info("t2bce_vhci: no-state resume exit status=%d\n", status);
        return status;
    }

    pr_debug("t2bce_vhci: no-state resume, powering off all ports\n");
    for (i = 1; i <= vhci->port_count; i++) {
        status = bce_vhci_cmd_port_power_off(&vhci->cq, (u8) i);
        pr_debug("t2bce_vhci: no-state resume, port %d power_off -> %d\n", i, status);
    }

    msleep(100);

    pr_debug("t2bce_vhci: no-state resume, powering on all ports\n");
    for (i = 1; i <= vhci->port_count; i++) {
        status = bce_vhci_cmd_port_power_on(&vhci->cq, (u8) i);
        pr_debug("t2bce_vhci: no-state resume, port %d power_on -> %d\n", i, status);
    }

    pr_debug("t2bce_vhci: no-state resume, notifying usbcore about lost power\n");
    usb_root_hub_lost_power(hcd->self.root_hub);
    vhci->no_state_resume = false;
    pr_info("t2bce_vhci: no-state resume exit status=%d\n", status);
    return status;
}

static int bce_vhci_resume_all_queues(struct bce_vhci *vhci)
{
    int devid;
    int endp;
    int status;

    for (devid = 1; devid < ARRAY_SIZE(vhci->devices); devid++) {
        if (!vhci->devices[devid])
            continue;

        for (endp = 0; endp < 32; endp++) {
            if (!(vhci->devices[devid]->tq_mask & BIT(endp)))
                continue;

            if (vhci->devices[devid]->tq[endp].endp_addr == 0x00)
                pr_debug("t2bce_vhci: resume_all EP0 pre dev=%d paused_by=%x state=%x active=%u\n",
                        devid, vhci->devices[devid]->tq[endp].paused_by,
                        vhci->devices[devid]->tq[endp].state,
                        vhci->devices[devid]->tq[endp].active);
            status = bce_vhci_transfer_queue_resume(&vhci->devices[devid]->tq[endp],
                                                    BCE_VHCI_PAUSE_SUSPEND);
            if (vhci->devices[devid]->tq[endp].endp_addr == 0x00)
                pr_debug("t2bce_vhci: resume_all EP0 post dev=%d status=%d paused_by=%x state=%x active=%u\n",
                        devid, status, vhci->devices[devid]->tq[endp].paused_by,
                        vhci->devices[devid]->tq[endp].state,
                        vhci->devices[devid]->tq[endp].active);
            if (status) {
                pr_err("t2bce_vhci: stateful resume queue failed: dev=%d ep=%02x status=%d\n",
                       devid, vhci->devices[devid]->tq[endp].endp_addr, status);
                return status;
            }
        }
    }

    return 0;
}

static int bce_vhci_resume_stateful(struct usb_hcd *hcd)
{
    struct bce_vhci *vhci = bce_vhci_from_hcd(hcd);
    int status;

    /* Linux preserved-state resume path. This is kept separate from the
     * no-state rebuild path below. */
    vhci->port_change_pending = 0;
    vhci->stateful_suspended_bounce = 0;
    vhci->stateful_resume = true;

    bce_vhci_event_queue_resume(&vhci->ev_system);
    bce_vhci_event_queue_resume(&vhci->ev_isochronous);
    bce_vhci_event_queue_resume(&vhci->ev_interrupt);
    bce_vhci_event_queue_resume(&vhci->ev_asynchronous);

    pr_debug("t2bce_vhci: stateful resume queues before firmware command events\n");
    status = bce_vhci_resume_all_queues(vhci);
    pr_debug("t2bce_vhci: stateful resume enabling firmware command events status=%d\n", status);
    bce_vhci_event_queue_resume(&vhci->ev_commands);
    pr_info("t2bce_vhci: stateful resume exit status=%d\n", status);
    return status;
}

static int bce_vhci_bus_resume(struct usb_hcd *hcd)
{
    struct bce_vhci *vhci = bce_vhci_from_hcd(hcd);
    int status;

    pr_debug("t2bce_vhci: bus_resume entry no_state_resume=%d\n", vhci->no_state_resume);
    vhci->port_change_pending = 0;
    vhci->system_suspending = false;
    pr_debug("t2bce_vhci: resume started\n");

    if (vhci->no_state_resume)
        status = bce_vhci_resume_no_state(hcd);
    else
        status = bce_vhci_resume_stateful(hcd);

    pr_info("t2bce_vhci: bus_resume exit status=%d no_state_resume=%d\n", status, vhci->no_state_resume);
    return status;
}

static int bce_vhci_urb_enqueue(struct usb_hcd *hcd, struct urb *urb, gfp_t mem_flags)
{
    struct bce_vhci_transfer_queue *q = urb->ep->hcpriv;
    if (!q)
        return -ENOENT;
    pr_debug("t2bce_vhci: urb_enqueue dev=%u ep=%02x\n",
            q->dev_addr, urb->ep->desc.bEndpointAddress);
    return bce_vhci_urb_create(q, urb);
}

static int bce_vhci_urb_dequeue(struct usb_hcd *hcd, struct urb *urb, int status)
{
    struct bce_vhci_transfer_queue *q = urb->ep->hcpriv;
    if (!q)
        return -ENOENT;
    pr_debug("t2bce_vhci: urb_dequeue ep=%02x\n", urb->ep->desc.bEndpointAddress);
    return bce_vhci_urb_request_cancel(q, urb, status);
}

static void bce_vhci_endpoint_reset(struct usb_hcd *hcd, struct usb_host_endpoint *ep)
{
    struct bce_vhci_transfer_queue *q = ep->hcpriv;
    pr_debug("t2bce_vhci: endpoint_reset\n");
    if (q)
        bce_vhci_transfer_queue_request_reset(q);
}

static u8 bce_vhci_endpoint_index(u8 addr)
{
    if (addr & 0x80)
        return (u8) (0x10 + (addr & 0xf));
    return (u8) (addr & 0xf);
}

static int bce_vhci_add_endpoint(struct usb_hcd *hcd, struct usb_device *udev, struct usb_host_endpoint *endp)
{
    u8 endp_index = bce_vhci_endpoint_index(endp->desc.bEndpointAddress);
    struct bce_vhci *vhci = bce_vhci_from_hcd(hcd);
    bce_vhci_device_t devid = vhci->port_to_device[udev->portnum];
    struct bce_vhci_device *vdev = vhci->devices[devid];
    pr_debug("t2bce_vhci: add_endpoint port=%u dev=%u ep_index=%u\n",
            udev->portnum, devid, endp_index);

    if (udev->bus->root_hub == udev) /* The USB hub */
        return 0;
    if (vdev == NULL)
        return -ENODEV;
    if (vdev->tq_mask & BIT(endp_index)) {
        endp->hcpriv = &vdev->tq[endp_index];
        return 0;
    }

    bce_vhci_create_transfer_queue(vhci, &vdev->tq[endp_index], endp, devid,
            usb_endpoint_dir_in(&endp->desc) ? DMA_FROM_DEVICE : DMA_TO_DEVICE);
    endp->hcpriv = &vdev->tq[endp_index];
    vdev->tq_mask |= BIT(endp_index);

    bce_vhci_cmd_endpoint_create(&vhci->cq, devid, &endp->desc);
    return 0;
}

static int bce_vhci_drop_endpoint(struct usb_hcd *hcd, struct usb_device *udev, struct usb_host_endpoint *endp)
{
    u8 endp_index = bce_vhci_endpoint_index(endp->desc.bEndpointAddress);
    struct bce_vhci *vhci = bce_vhci_from_hcd(hcd);
    bce_vhci_device_t devid = vhci->port_to_device[udev->portnum];
    struct bce_vhci_transfer_queue *q = endp->hcpriv;
    struct bce_vhci_device *vdev;
    pr_debug("t2bce_vhci: drop_endpoint port=%u ep_index=%u\n", udev->portnum, endp_index);
    if (!devid || !vhci->devices[devid]) {
        endp->hcpriv = NULL;
        return 0;
    }
    vdev = vhci->devices[devid];
    if (!q) {
        if (vdev->tq_mask & BIT(endp_index)) {
            pr_err("t2bce_vhci: endpoint missing hcpriv while still tracked\n");
            q = &vdev->tq[endp_index];
        } else {
            return 0;
        }
    }

    bce_vhci_cmd_endpoint_destroy(&vhci->cq, devid, (u8) (endp->desc.bEndpointAddress & 0x8Fu));
    vdev->tq_mask &= ~BIT(endp_index);
    bce_vhci_destroy_transfer_queue(vhci, q);
    endp->hcpriv = NULL;
    return 0;
}

static int bce_vhci_create_message_queues(struct bce_vhci *vhci)
{
    if (bce_vhci_message_queue_create(vhci, &vhci->msg_commands, "VHC1HostCommands") ||
        bce_vhci_message_queue_create(vhci, &vhci->msg_system, "VHC1HostSystemEvents") ||
        bce_vhci_message_queue_create(vhci, &vhci->msg_isochronous, "VHC1HostIsochronousEvents") ||
        bce_vhci_message_queue_create(vhci, &vhci->msg_interrupt, "VHC1HostInterruptEvents") ||
        bce_vhci_message_queue_create(vhci, &vhci->msg_asynchronous, "VHC1HostAsynchronousEvents")) {
        bce_vhci_destroy_message_queues(vhci);
        return -EINVAL;
    }
    spin_lock_init(&vhci->msg_asynchronous_lock);
    bce_vhci_command_queue_create(&vhci->cq, &vhci->msg_commands);
    return 0;
}

static void bce_vhci_destroy_message_queues(struct bce_vhci *vhci)
{
    bce_vhci_command_queue_destroy(&vhci->cq);
    bce_vhci_message_queue_destroy(vhci, &vhci->msg_commands);
    bce_vhci_message_queue_destroy(vhci, &vhci->msg_system);
    bce_vhci_message_queue_destroy(vhci, &vhci->msg_isochronous);
    bce_vhci_message_queue_destroy(vhci, &vhci->msg_interrupt);
    bce_vhci_message_queue_destroy(vhci, &vhci->msg_asynchronous);
}

static void bce_vhci_handle_system_event(struct bce_vhci_event_queue *q, struct bce_vhci_message *msg);
static void bce_vhci_handle_usb_event(struct bce_vhci_event_queue *q, struct bce_vhci_message *msg);

static int bce_vhci_create_event_queues(struct bce_vhci *vhci)
{
    vhci->ev_cq = t2bce_core_create_cq(vhci->client, 0x100);
    if (!vhci->ev_cq)
        return -EINVAL;
#define CREATE_EVENT_QUEUE(field, name, cb) bce_vhci_event_queue_create(vhci, &vhci->field, name, cb)
    if (__bce_vhci_event_queue_create(vhci, &vhci->ev_commands, "VHC1FirmwareCommands",
            bce_vhci_firmware_event_completion) ||
        CREATE_EVENT_QUEUE(ev_system,       "VHC1FirmwareSystemEvents",       bce_vhci_handle_system_event) ||
        CREATE_EVENT_QUEUE(ev_isochronous,  "VHC1FirmwareIsochronousEvents",  bce_vhci_handle_usb_event) ||
        CREATE_EVENT_QUEUE(ev_interrupt,    "VHC1FirmwareInterruptEvents",    bce_vhci_handle_usb_event) ||
        CREATE_EVENT_QUEUE(ev_asynchronous, "VHC1FirmwareAsynchronousEvents", bce_vhci_handle_usb_event)) {
        bce_vhci_destroy_event_queues(vhci);
        return -EINVAL;
    }
#undef CREATE_EVENT_QUEUE
    return 0;
}

static void bce_vhci_destroy_event_queues(struct bce_vhci *vhci)
{
    bce_vhci_event_queue_destroy(vhci, &vhci->ev_commands);
    bce_vhci_event_queue_destroy(vhci, &vhci->ev_system);
    bce_vhci_event_queue_destroy(vhci, &vhci->ev_isochronous);
    bce_vhci_event_queue_destroy(vhci, &vhci->ev_interrupt);
    bce_vhci_event_queue_destroy(vhci, &vhci->ev_asynchronous);
    if (vhci->ev_cq)
        t2bce_core_destroy_cq(vhci->client, vhci->ev_cq);
}

static void bce_vhci_send_fw_event_response(struct bce_vhci *vhci, struct bce_vhci_message *req, u16 status)
{
    unsigned long timeout = 1000;
    struct bce_vhci_message r = *req;
    r.cmd = (u16) (req->cmd | 0x8000u);
    r.status = status;
    r.param1 = req->param1;
    r.param2 = 0;

    if (t2bce_core_reserve_submission(vhci->msg_system.sq, &timeout)) {
        pr_err("t2bce_vhci: Cannot reserve submision for FW event reply\n");
        return;
    }
    bce_vhci_message_queue_write(&vhci->msg_system, &r);
}

static int bce_vhci_handle_firmware_event(struct bce_vhci *vhci, struct bce_vhci_message *msg)
{
    unsigned long flags;
    bce_vhci_device_t devid;
    u8 endp;
    struct bce_vhci_device *dev;
    struct bce_vhci_transfer_queue *tq;
    if (msg->cmd == BCE_VHCI_CMD_ENDPOINT_REQUEST_STATE || msg->cmd == BCE_VHCI_CMD_ENDPOINT_SET_STATE) {
        devid = (bce_vhci_device_t) (msg->param1 & 0xff);
        endp = bce_vhci_endpoint_index((u8) ((msg->param1 >> 8) & 0xff));
        dev = vhci->devices[devid];
        if (!dev || !(dev->tq_mask & BIT(endp)))
            return BCE_VHCI_BAD_ARGUMENT;
        tq = &dev->tq[endp];
    }

    if (msg->cmd == BCE_VHCI_CMD_ENDPOINT_REQUEST_STATE) {
        if (msg->param2 == BCE_VHCI_ENDPOINT_ACTIVE) {
            int ret = bce_vhci_transfer_queue_resume(tq, BCE_VHCI_PAUSE_FIRMWARE);

            pr_debug("t2bce_vhci: FW endpoint request ACTIVE dev=%u ep=%02x paused_by=%x state=%x active=%u ret=%d reply=%u\n",
                    devid, tq->endp_addr, tq->paused_by, tq->state, tq->active, ret,
                    ret ? BCE_VHCI_ERROR : BCE_VHCI_SUCCESS);
            return ret ? BCE_VHCI_ERROR : BCE_VHCI_SUCCESS;
        } else if (msg->param2 == BCE_VHCI_ENDPOINT_PAUSED) {
            int ret = bce_vhci_transfer_queue_pause(tq, BCE_VHCI_PAUSE_FIRMWARE);

            pr_debug("t2bce_vhci: FW endpoint request PAUSED dev=%u ep=%02x paused_by=%x state=%x active=%u ret=%d reply=%u\n",
                    devid, tq->endp_addr, tq->paused_by, tq->state, tq->active, ret,
                    ret ? BCE_VHCI_ERROR : BCE_VHCI_SUCCESS);
            return ret ? BCE_VHCI_ERROR : BCE_VHCI_SUCCESS;
        }
        return BCE_VHCI_BAD_ARGUMENT;
    } else if (msg->cmd == BCE_VHCI_CMD_ENDPOINT_SET_STATE) {
        if (msg->param2 == BCE_VHCI_ENDPOINT_STALLED) {
            tq->state = msg->param2;
            spin_lock_irqsave(&tq->urb_lock, flags);
            tq->active = false;
            tq->stalled = true;
            spin_unlock_irqrestore(&tq->urb_lock, flags);
            pr_warn("t2bce_vhci: firmware endpoint stalled dev=%u ep=%02x paused_by=%x; %s\n",
                    devid, tq->endp_addr, tq->paused_by,
                    tq->endp_addr == 0x00 ? "scheduling EP0 reset" : "leaving stalled");
            if (tq->endp_addr == 0x00)
                bce_vhci_transfer_queue_request_reset(tq);
            return BCE_VHCI_SUCCESS;
        }
        return BCE_VHCI_BAD_ARGUMENT;
    }
    pr_warn("t2bce_vhci: Unhandled firmware event: %x s=%x p1=%x p2=%llx\n",
            msg->cmd, msg->status, msg->param1, msg->param2);
    return BCE_VHCI_BAD_ARGUMENT;
}

static void bce_vhci_handle_firmware_events_w(struct work_struct *ws)
{
    size_t cnt = 0;
    int result;
    struct bce_vhci *vhci = container_of(ws, struct bce_vhci, w_fw_events);
    struct t2bce_core_queue_sq *sq = vhci->ev_commands.sq;
    struct t2bce_core_sq_completion_data *cq;
    struct bce_vhci_message *msg, *msg2 = NULL;

    while (true) {
        if (msg2) {
            msg = msg2;
            msg2 = NULL;
        } else if ((cq = t2bce_core_next_completion(sq))) {
            if (cq->status == T2BCE_COMPLETION_ABORTED) {
                t2bce_core_notify_submission_complete(sq);
                continue;
            }
            msg = &vhci->ev_commands.data[t2bce_core_queue_sq_head(sq)];
        } else {
            break;
        }

        pr_debug("t2bce_vhci: Got fw event: %x s=%x p1=%x p2=%llx\n", msg->cmd, msg->status, msg->param1, msg->param2);
        if ((cq = t2bce_core_next_completion(sq))) {
            msg2 = &vhci->ev_commands.data[(t2bce_core_queue_sq_head(sq) + 1) % t2bce_core_queue_sq_capacity(sq)];
            pr_debug("t2bce_vhci: Got second fw event: %x s=%x p1=%x p2=%llx\n",
                    msg->cmd, msg->status, msg->param1, msg->param2);
            if (cq->status != T2BCE_COMPLETION_ABORTED &&
                msg2->cmd == (msg->cmd | 0x4000) && msg2->param1 == msg->param1) {
                /* Take two elements */
                pr_debug("t2bce_vhci: Cancelled\n");
                bce_vhci_send_fw_event_response(vhci, msg, BCE_VHCI_ABORT);

                t2bce_core_notify_submission_complete(sq);
                t2bce_core_notify_submission_complete(sq);
                msg2 = NULL;
                cnt += 2;
                continue;
            }

            pr_warn("t2bce_vhci: Handle fw event - unexpected cancellation\n");
        }

        result = bce_vhci_handle_firmware_event(vhci, msg);
        bce_vhci_send_fw_event_response(vhci, msg, (u16) result);


        t2bce_core_notify_submission_complete(sq);
        ++cnt;
    }
    bce_vhci_event_queue_submit_pending(&vhci->ev_commands, cnt);
    if (t2bce_core_queue_sq_available(sq) == t2bce_core_queue_sq_capacity(sq) - 1) {
        pr_debug("t2bce_vhci: complete\n");
        complete(&vhci->ev_commands.queue_empty_completion);
    }
}

static void bce_vhci_firmware_event_completion(struct t2bce_core_queue_sq *sq)
{
    struct bce_vhci_event_queue *q = t2bce_core_queue_sq_userdata(sq);
    queue_work(q->vhci->tq_state_wq, &q->vhci->w_fw_events);
}

static const char *bce_vhci_port_state_change_label(u64 p2)
{
    return (p2 & 0x4) ? "connected" : "disconnected";
}

static void bce_vhci_handle_system_event(struct bce_vhci_event_queue *q, struct bce_vhci_message *msg)
{
    if (msg->cmd & 0x8000) {
        bce_vhci_command_queue_deliver_completion(&q->vhci->cq, msg);
    } else if (msg->cmd == BCE_VHCI_EV_PORT_STATUS_CHANGE &&
               msg->param1 > 0 &&
               msg->param1 < ARRAY_SIZE(q->vhci->port_to_device)) {
        u32 normalized_status = (u32)msg->param2 & ~BCE_VHCI_PORT_STATUS_C_CONNECTION;

        if (q->vhci->stateful_resume &&
            (msg->param2 & BCE_VHCI_PORT_STATUS_CONNECTED) &&
            normalized_status == 0x285) {
            set_bit(msg->param1 - 1, &q->vhci->stateful_suspended_bounce);
            pr_debug("t2bce_vhci: marked stateful suspended bounce port=%u raw=%llx normalized=%x\n",
                    msg->param1, msg->param2, normalized_status);
        }
        set_bit(msg->param1 - 1, &q->vhci->port_change_pending);
        pr_debug("t2bce_vhci: port state change event: port=%u state=%s status=%x p2=%llx\n",
                msg->param1, bce_vhci_port_state_change_label(msg->param2),
                msg->status, msg->param2);
    } else {
        pr_warn("t2bce_vhci: Unhandled system event: %x s=%x p1=%x p2=%llx\n",
                msg->cmd, msg->status, msg->param1, msg->param2);
    }
}

static void bce_vhci_handle_usb_event(struct bce_vhci_event_queue *q, struct bce_vhci_message *msg)
{
    bce_vhci_device_t devid;
    u8 endp;
    struct bce_vhci_device *dev;
    if (msg->cmd & 0x8000) {
        bce_vhci_command_queue_deliver_completion(&q->vhci->cq, msg);
    } else if (msg->cmd == BCE_VHCI_CMD_TRANSFER_REQUEST || msg->cmd == BCE_VHCI_CMD_CONTROL_TRANSFER_STATUS) {
        devid = (bce_vhci_device_t) (msg->param1 & 0xff);
        endp = bce_vhci_endpoint_index((u8) ((msg->param1 >> 8) & 0xff));
        dev = q->vhci->devices[devid];
        if (!dev || (dev->tq_mask & BIT(endp)) == 0) {
            pr_err("t2bce_vhci: Didn't find destination for transfer queue event\n");
            return;
        }
        bce_vhci_transfer_queue_event(&dev->tq[endp], msg);
    } else {
        pr_warn("t2bce_vhci: Unhandled USB event: %x s=%x p1=%x p2=%llx\n",
                msg->cmd, msg->status, msg->param1, msg->param2);
    }
}



static const struct hc_driver bce_vhci_driver = {
        .description = "t2bce_vhci",
        .product_desc = "BCE VHCI Host Controller",
        .hcd_priv_size = sizeof(struct bce_vhci *),

#if LINUX_VERSION_CODE < KERNEL_VERSION(5,4,0)
        .flags = HCD_USB2,
#else
        .flags = HCD_USB2 | HCD_DMA,
#endif

        .start = bce_vhci_start,
        .stop = bce_vhci_stop,
        .hub_status_data = bce_vhci_hub_status_data,
        .hub_control = bce_vhci_hub_control,
        .urb_enqueue = bce_vhci_urb_enqueue,
        .urb_dequeue = bce_vhci_urb_dequeue,
        .enable_device = bce_vhci_enable_device,
        .free_dev = bce_vhci_free_device,
        .address_device = bce_vhci_address_device,
        .add_endpoint = bce_vhci_add_endpoint,
        .drop_endpoint = bce_vhci_drop_endpoint,
        .endpoint_reset = bce_vhci_endpoint_reset,
        .check_bandwidth = bce_vhci_check_bandwidth,
        .get_frame_number = bce_vhci_get_frame_number,
        .bus_suspend = bce_vhci_bus_suspend,
        .bus_resume = bce_vhci_bus_resume
};


int __init bce_vhci_module_init(void)
{
    struct device *parent;
    int result;

    if ((result = alloc_chrdev_region(&bce_vhci_chrdev, 0, 1, "t2bce_vhci")))
        goto fail_chrdev;
#if LINUX_VERSION_CODE < KERNEL_VERSION(6,4,0)
    bce_vhci_class = class_create(THIS_MODULE, "t2bce_vhci");
#else
    bce_vhci_class = class_create("t2bce_vhci");
#endif
    if (IS_ERR(bce_vhci_class)) {
        result = PTR_ERR(bce_vhci_class);
        goto fail_class;
    }

    global_vhci = kzalloc(sizeof(*global_vhci), GFP_KERNEL);
    if (!global_vhci) {
        result = -ENOMEM;
        goto fail_alloc;
    }

    parent = t2bce_core_device_get();
    if (IS_ERR(parent)) {
        result = PTR_ERR(parent);
        goto fail_create;
    }

    result = bce_vhci_create(parent, global_vhci);
    t2bce_core_device_put(parent);
    if (result)
        goto fail_create;

    pr_info("t2bce_vhci: module initialized\n");
    return 0;

fail_create:
    kfree(global_vhci);
    global_vhci = NULL;
fail_alloc:
    class_destroy(bce_vhci_class);
fail_class:
    unregister_chrdev_region(bce_vhci_chrdev, 1);
fail_chrdev:
    if (!result)
        result = -EINVAL;
    pr_info("t2bce_vhci: module init failed status=%d\n", result);
    return result;
}
void __exit bce_vhci_module_exit(void)
{
    if (global_vhci) {
        bce_vhci_destroy(global_vhci);
        kfree(global_vhci);
        global_vhci = NULL;
    }
    class_destroy(bce_vhci_class);
    unregister_chrdev_region(bce_vhci_chrdev, 1);
    pr_info("t2bce_vhci: module exited\n");
}

module_param_named(vhci_port_mask, bce_vhci_port_mask, ushort, 0444);
MODULE_PARM_DESC(vhci_port_mask, "Specifies which VHCI ports are enabled");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("André Eikmeyer <andre.eikmeyer@gmail.com>");
MODULE_DESCRIPTION("Apple T2 BCE VHCI Driver");
MODULE_VERSION("0.01");
MODULE_SOFTDEP("pre: t2bce_core");
module_init(bce_vhci_module_init);
module_exit(bce_vhci_module_exit);
