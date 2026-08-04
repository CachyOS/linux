#ifndef T2AUDIO_PROTOCOL_H
#define T2AUDIO_PROTOCOL_H

#include <linux/types.h>

struct t2audio_device;

typedef u64 t2audio_device_id_t;
typedef u64 t2audio_object_id_t;

struct t2audio_msg {
    void *data;
    size_t size;
};

struct __attribute__((packed)) t2audio_msg_header {
    char tag[4];
    u8 type;
    t2audio_device_id_t device_id;
};
struct __attribute__((packed)) t2audio_msg_base {
    u32 msg;
    u32 status;
};

struct t2audio_prop_addr {
    u32 scope;
    u32 selector;
    u32 element;
};
#define T2AUDIO_PROP(scope, sel, el) (struct t2audio_prop_addr) { scope, sel, el }

enum {
    T2AUDIO_MSG_TYPE_COMMAND = 1,
    T2AUDIO_MSG_TYPE_RESPONSE = 2,
    T2AUDIO_MSG_TYPE_NOTIFICATION = 3
};

enum {
    T2AUDIO_MSG_START_IO = 0,
    T2AUDIO_MSG_START_IO_RESPONSE = 1,
    T2AUDIO_MSG_STOP_IO = 2,
    T2AUDIO_MSG_STOP_IO_RESPONSE = 3,
    T2AUDIO_MSG_UPDATE_TIMESTAMP = 4,
    T2AUDIO_MSG_GET_PROPERTY = 7,
    T2AUDIO_MSG_GET_PROPERTY_RESPONSE = 8,
    T2AUDIO_MSG_SET_PROPERTY = 9,
    T2AUDIO_MSG_SET_PROPERTY_RESPONSE = 10,
    T2AUDIO_MSG_PROPERTY_LISTENER = 11,
    T2AUDIO_MSG_PROPERTY_LISTENER_RESPONSE = 12,
    T2AUDIO_MSG_PROPERTY_CHANGED = 13,
    T2AUDIO_MSG_SET_INPUT_STREAM_ADDRESS_RANGES = 18,
    T2AUDIO_MSG_SET_INPUT_STREAM_ADDRESS_RANGES_RESPONSE = 19,
    T2AUDIO_MSG_GET_INPUT_STREAM_LIST = 24,
    T2AUDIO_MSG_GET_INPUT_STREAM_LIST_RESPONSE = 25,
    T2AUDIO_MSG_GET_OUTPUT_STREAM_LIST = 26,
    T2AUDIO_MSG_GET_OUTPUT_STREAM_LIST_RESPONSE = 27,
    T2AUDIO_MSG_SET_REMOTE_ACCESS = 32,
    T2AUDIO_MSG_SET_REMOTE_ACCESS_RESPONSE = 33,
    T2AUDIO_MSG_UPDATE_TIMESTAMP_RESPONSE = 34,

    T2AUDIO_MSG_NOTIFICATION_ALIVE = 100,
    T2AUDIO_MSG_GET_DEVICE_LIST = 101,
    T2AUDIO_MSG_GET_DEVICE_LIST_RESPONSE = 102,
    T2AUDIO_MSG_NOTIFICATION_BOOT = 104
};

enum {
    T2AUDIO_REMOTE_ACCESS_OFF = 0,
    T2AUDIO_REMOTE_ACCESS_ON = 2
};

enum {
    T2AUDIO_PROP_SCOPE_GLOBAL = 0x676c6f62, // 'glob'
    T2AUDIO_PROP_SCOPE_INPUT  = 0x696e7074, // 'inpt'
    T2AUDIO_PROP_SCOPE_OUTPUT = 0x6f757470  // 'outp'
};

enum {
    T2AUDIO_PROP_UID          = 0x75696420, // 'uid '
    T2AUDIO_PROP_BOOL_VALUE   = 0x6263766c, // 'bcvl'
    T2AUDIO_PROP_JACK_PLUGGED = 0x6a61636b, // 'jack'
    T2AUDIO_PROP_SEL_VOLUME   = 0x64656176, // 'deav'
    T2AUDIO_PROP_LATENCY      = 0x6c746e63, // 'ltnc'
    T2AUDIO_PROP_PHYS_FORMAT  = 0x70667420  // 'pft '
};

int t2audio_msg_read_base(struct t2audio_msg *msg, struct t2audio_msg_base *base);

int t2audio_msg_read_start_io_response(struct t2audio_msg *msg);
int t2audio_msg_read_stop_io_response(struct t2audio_msg *msg);
int t2audio_msg_read_update_timestamp(struct t2audio_msg *msg, t2audio_device_id_t *devid,
        u64 *timestamp, u64 *update_seed);
int t2audio_msg_read_get_property_response(struct t2audio_msg *msg, t2audio_object_id_t *obj,
        struct t2audio_prop_addr *prop, void **data, u64 *data_size);
int t2audio_msg_read_set_property_response(struct t2audio_msg *msg, t2audio_object_id_t *obj);
int t2audio_msg_read_property_listener_response(struct t2audio_msg *msg,t2audio_object_id_t *obj,
        struct t2audio_prop_addr *prop);
int t2audio_msg_read_property_changed(struct t2audio_msg *msg, t2audio_device_id_t *devid, t2audio_object_id_t *obj,
        struct t2audio_prop_addr *prop);
int t2audio_msg_read_set_input_stream_address_ranges_response(struct t2audio_msg *msg);
int t2audio_msg_read_get_input_stream_list_response(struct t2audio_msg *msg, t2audio_object_id_t **str_l, u64 *str_cnt);
int t2audio_msg_read_get_output_stream_list_response(struct t2audio_msg *msg, t2audio_object_id_t **str_l, u64 *str_cnt);
int t2audio_msg_read_set_remote_access_response(struct t2audio_msg *msg);
int t2audio_msg_read_get_device_list_response(struct t2audio_msg *msg, t2audio_device_id_t **dev_l, u64 *dev_cnt);

void t2audio_msg_write_start_io(struct t2audio_msg *msg, t2audio_device_id_t dev);
void t2audio_msg_write_stop_io(struct t2audio_msg *msg, t2audio_device_id_t dev);
void t2audio_msg_write_get_property(struct t2audio_msg *msg, t2audio_device_id_t dev, t2audio_object_id_t obj,
        struct t2audio_prop_addr prop, void *qualifier, u64 qualifier_size);
void t2audio_msg_write_set_property(struct t2audio_msg *msg, t2audio_device_id_t dev, t2audio_object_id_t obj,
        struct t2audio_prop_addr prop, void *data, u64 data_size, void *qualifier, u64 qualifier_size);
void t2audio_msg_write_property_listener(struct t2audio_msg *msg, t2audio_device_id_t dev, t2audio_object_id_t obj,
        struct t2audio_prop_addr prop);
void t2audio_msg_write_set_input_stream_address_ranges(struct t2audio_msg *msg, t2audio_device_id_t devid);
void t2audio_msg_write_get_input_stream_list(struct t2audio_msg *msg, t2audio_device_id_t devid);
void t2audio_msg_write_get_output_stream_list(struct t2audio_msg *msg, t2audio_device_id_t devid);
void t2audio_msg_write_set_remote_access(struct t2audio_msg *msg, u64 mode);
void t2audio_msg_write_alive_notification(struct t2audio_msg *msg, u32 proto_ver, u32 msg_ver);
void t2audio_msg_write_update_timestamp_response(struct t2audio_msg *msg);
void t2audio_msg_write_get_device_list(struct t2audio_msg *msg);


int t2audio_cmd_start_io(struct t2audio_device *a, t2audio_device_id_t devid);
int t2audio_cmd_stop_io(struct t2audio_device *a, t2audio_device_id_t devid);
int t2audio_cmd_get_property(struct t2audio_device *a, struct t2audio_msg *buf,
        t2audio_device_id_t devid, t2audio_object_id_t obj,
        struct t2audio_prop_addr prop, void *qualifier, u64 qualifier_size, void **data, u64 *data_size);
int t2audio_cmd_get_primitive_property(struct t2audio_device *a,
        t2audio_device_id_t devid, t2audio_object_id_t obj,
        struct t2audio_prop_addr prop, void *qualifier, u64 qualifier_size, void *data, u64 data_size);
int t2audio_cmd_set_property(struct t2audio_device *a, t2audio_device_id_t devid, t2audio_object_id_t obj,
        struct t2audio_prop_addr prop, void *qualifier, u64 qualifier_size, void *data, u64 data_size);
int t2audio_cmd_property_listener(struct t2audio_device *a, t2audio_device_id_t devid, t2audio_object_id_t obj,
        struct t2audio_prop_addr prop);
int t2audio_cmd_set_input_stream_address_ranges(struct t2audio_device *a, t2audio_device_id_t devid);
int t2audio_cmd_get_input_stream_list(struct t2audio_device *a, struct t2audio_msg *buf, t2audio_device_id_t devid,
        t2audio_object_id_t **str_l, u64 *str_cnt);
int t2audio_cmd_get_output_stream_list(struct t2audio_device *a, struct t2audio_msg *buf, t2audio_device_id_t devid,
        t2audio_object_id_t **str_l, u64 *str_cnt);
int t2audio_cmd_set_remote_access(struct t2audio_device *a, u64 mode);
int t2audio_cmd_get_device_list(struct t2audio_device *a, struct t2audio_msg *buf,
        t2audio_device_id_t **dev_l, u64 *dev_cnt);



#endif //T2AUDIO_PROTOCOL_H
