#include <linux/dmi.h>
#include <linux/hid.h>
#include <linux/module.h>
#include <linux/usb.h>
#include <linux/mutex.h>

//#include "hid-ids.h"

#define MSI_CLAW_FEATURE_GAMEPAD_REPORT_ID 0x0f

#define MSI_CLAW_READ_SIZE 64
#define MSI_CLAW_WRITE_SIZE 64

#define MSI_CLAW_GAME_CONTROL_DESC   0x05
#define MSI_CLAW_DEVICE_CONTROL_DESC 0x06

enum msi_claw_gamepad_mode {
	MSI_CLAW_GAMEPAD_MODE_OFFLINE = 0x00,
	MSI_CLAW_GAMEPAD_MODE_XINPUT = 0x01,
	MSI_CLAW_GAMEPAD_MODE_DINPUT = 0x02,
	MSI_CLAW_GAMEPAD_MODE_MSI = 0x03,
	MSI_CLAW_GAMEPAD_MODE_DESKTOP = 0x04,
	MSI_CLAW_GAMEPAD_MODE_BIOS = 0x05,
	MSI_CLAW_GAMEPAD_MODE_TESTING = 0x06,

	MSI_CLAW_GAMEPAD_MODE_MAX,
};

enum msi_claw_mkeys_function {
	MSI_CLAW_MKEY_FUNCTION_MACRO = 0x00,
	MSI_CLAW_MKEY_FUNCTION_COMBINATION = 0x01,
	MSI_CLAW_MKEY_FUNCTION_DISABLED = 0x02,

	MSI_CLAW_MKEY_FUNCTION_MAX,
};

static const bool gamepad_mode_debug = false;

static const struct {
	const char* name;
	const bool available;
} gamepad_mode_map[] = {
	{"offline", gamepad_mode_debug},
	{"xinput", true},
	{"dinput", gamepad_mode_debug},
	{"msi", gamepad_mode_debug},
	{"desktop", true},
	{"bios", gamepad_mode_debug},
	{"testing", gamepad_mode_debug},
};

static const char* mkeys_function_map[] =
{
	"macro",
	"combination",
};

enum msi_claw_command_type {
	MSI_CLAW_COMMAND_TYPE_ENTER_PROFILE_CONFIG = 0x01,
	MSI_CLAW_COMMAND_TYPE_EXIT_PROFILE_CONFIG = 0x02,
	MSI_CLAW_COMMAND_TYPE_WRITE_PROFILE = 0x03,
	MSI_CLAW_COMMAND_TYPE_READ_PROFILE = 0x04,
	MSI_CLAW_COMMAND_TYPE_READ_PROFILE_ACK = 0x05,
	// ACK is read after a WRITE_RGB_STATUS
	MSI_CLAW_COMMAND_TYPE_ACK = 0x06,
	MSI_CLAW_COMMAND_TYPE_SWITCH_PROFILE = 0x07,
	MSI_CLAW_COMMAND_TYPE_WRITE_PROFILE_TO_EEPROM = 0x08,
	MSI_CLAW_COMMAND_TYPE_SYNC_RGB = 0x09,
	MSI_CLAW_COMMAND_TYPE_READ_RGB_STATUS_ACK = 0x0a,
	MSI_CLAW_COMMAND_TYPE_READ_CURRENT_PROFILE = 0x0b,
	MSI_CLAW_COMMAND_TYPE_READ_CURRENT_PROFILE_ACK = 0x0c,
	MSI_CLAW_COMMAND_TYPE_READ_RGB_STATUS = 0x0d,
	// TODO: 0f00003c210100b137ff00000000ff00000000ff00000000ff00000000ff00000000ff00000000ff00000000ff00000000ff00000000ff00000000ff00000000
	MSI_CLAW_COMMAND_TYPE_WRITE_RGB_STATUS = 0x21,
	MSI_CLAW_COMMAND_TYPE_SYNC_TO_ROM = 0x22,
	MSI_CLAW_COMMAND_TYPE_RESTORE_FROM_ROM = 0x23,
	MSI_CLAW_COMMAND_TYPE_SWITCH_MODE = 0x24,
	MSI_CLAW_COMMAND_TYPE_READ_GAMEPAD_MODE = 0x26,
	MSI_CLAW_COMMAND_TYPE_GAMEPAD_MODE_ACK = 0x27,
	MSI_CLAW_COMMAND_TYPE_RESET_DEVICE = 0x28,
	MSI_CLAW_COMMAND_TYPE_RGB_CONTROL = 0xe0,
	MSI_CLAW_COMMAND_TYPE_CALIBRATION_CONTROL = 0xfd,
	MSI_CLAW_COMMAND_TYPE_CALIBRATION_ACK = 0xfe,
};

struct msi_claw_control_status {
	enum msi_claw_gamepad_mode gamepad_mode;
	enum msi_claw_mkeys_function mkeys_function;
};

struct msi_claw_read_data {
	const uint8_t *data;
	int size;

	struct msi_claw_read_data *next;
};

struct msi_claw_drvdata {
	struct hid_device *hdev;

	//struct input_dev *input;

	struct msi_claw_control_status *control;

	struct mutex read_data_mutex;
	struct msi_claw_read_data *read_data;
};

static int msi_claw_write_cmd(struct hid_device *hdev, enum msi_claw_command_type cmdtype,
    const uint8_t *const buffer, size_t buffer_len)
{
	int ret;
	uint8_t *dmabuf = NULL;
	struct msi_claw_drvdata *drvdata = hid_get_drvdata(hdev);
	const uint8_t buf[MSI_CLAW_WRITE_SIZE] = {
		MSI_CLAW_FEATURE_GAMEPAD_REPORT_ID, 0, 0, 0x3c, cmdtype };

	if (!drvdata->control) {
		hid_err(hdev, "hid-msi-claw couldn't find control interface\n");
		ret = -ENODEV;
		goto msi_claw_write_cmd_err;
	}

	if (buffer != NULL)
		memcpy((void *)&buf[5], buffer, buffer_len);
	else
		buffer_len = 0;

	memset((void *)&buf[5 + buffer_len], 0, MSI_CLAW_WRITE_SIZE - (5 + buffer_len));
	dmabuf = kmemdup(buf, MSI_CLAW_WRITE_SIZE, GFP_KERNEL);
	if (!dmabuf) {
		ret = -ENOMEM;
		hid_err(hdev, "hid-msi-claw failed to alloc dma buf: %d\n", ret);
		goto msi_claw_write_cmd_err;
	}

	ret = hid_hw_output_report(hdev, dmabuf, MSI_CLAW_WRITE_SIZE);
	if (ret != MSI_CLAW_WRITE_SIZE) {
		hid_err(hdev, "hid-msi-claw failed to switch controller mode: %d\n", ret);
		goto msi_claw_write_cmd_err;
	}

	hid_notice(hdev, "hid-msi-claw sent %d bytes, cmd: 0x%02x\n", ret, dmabuf[4]);

msi_claw_write_cmd_err:
	kfree(dmabuf);

	return ret;
}

static int msi_claw_read(struct hid_device *hdev, uint8_t *const buffer, int size, uint32_t timeout)
{
	struct msi_claw_drvdata *drvdata = hid_get_drvdata(hdev);
	struct msi_claw_read_data *event = NULL;
	int ret = 0;

	if (!drvdata->control) {
		hid_err(hdev, "hid-msi-claw couldn't find control interface\n");
		ret = -ENODEV;
		goto msi_claw_read_err;
	}

	for (uint32_t i = 0; (event == NULL) && (i <= timeout); i++) {
		if (timeout - i)
			msleep(20);

		scoped_guard(mutex, &drvdata->read_data_mutex) {
			event = drvdata->read_data;

			if (event != NULL)
				drvdata->read_data = event->next;
		};
	}

	if (event == NULL) {
		ret = -EIO;
		hid_err(hdev, "hid-msi-claw no answer from device\n");
		goto msi_claw_read_err;
	}

	if (size < event->size) {
		ret = -EINVAL;
		hid_err(hdev, "hid-msi-claw invalid buffer size: too short\n");
		goto msi_claw_read_err;
	}

	memcpy((void *)buffer, (const void *)event->data, event->size);
	ret = event->size;

msi_claw_read_err:
	if (event != NULL) {
		kfree(event->data);
		kfree(event);
	}

	return ret;
}

static int msi_claw_raw_event_control(struct hid_device *hdev, struct msi_claw_drvdata *drvdata,
	struct hid_report *report, uint8_t *data, int size)
{
	struct msi_claw_read_data **list = NULL;
	struct msi_claw_read_data *node = NULL;
	uint8_t *buffer;
	int ret, i;

	if (size != MSI_CLAW_READ_SIZE) {
		//hid_err(hdev, "hid-msi-claw got unknown %d bytes\n", size);
		goto msi_claw_raw_event_control_err;
	} else if (data[0] != 0x10) {
		hid_err(hdev, "hid-msi-claw unrecognised byte at offset 0: expected 0x10, got 0x%02x\n", data[0]);
		goto msi_claw_raw_event_control_err;
	} else if (data[1] != 0x00) {
		hid_err(hdev, "hid-msi-claw unrecognised byte at offset 1: expected 0x00, got 0x%02x\n", data[1]);
		goto msi_claw_raw_event_control_err;
	} else if (data[2] != 0x00) {
		hid_err(hdev, "hid-msi-claw unrecognised byte at offset 2: expected 0x00, got 0x%02x\n", data[2]);
		goto msi_claw_raw_event_control_err;
	} else if (data[3] != 0x3c) {
		hid_err(hdev, "hid-msi-claw unrecognised byte at offset 3: expected 0x3c, got 0x%02x\n", data[3]);
		goto msi_claw_raw_event_control_err;
	}

	buffer = kmemdup(data, size, GFP_KERNEL);
	if (buffer == NULL) {
		ret = -ENOMEM;
		hid_err(hdev, "hid-msi-claw failed to alloc %d bytes for read buffer: %d\n", size, ret);
		goto msi_claw_raw_event_control_err;
	}

	struct msi_claw_read_data evt = {
		.data = buffer,
		.size = size,
		.next = NULL,
	};

	node = kmemdup(&evt, sizeof(evt), GFP_KERNEL);
	if (!node) {
		ret = -ENOMEM;
		kfree(buffer);
		hid_err(hdev, "hid-msi-claw failed to alloc event node: %d\n", ret);
		goto msi_claw_raw_event_control_err;
	}

	scoped_guard(mutex, &drvdata->read_data_mutex) {
		list = &drvdata->read_data;
		for (i = 0; (i < 32) && (*list != NULL); i++)
			list = &(*list)->next;

		if (*list != NULL) {
			ret = -EIO;
			hid_err(hdev, "too many unparsed events: ignoring\n");
			goto msi_claw_raw_event_control_err;
		}

		*list = node;
	}

	hid_notice(hdev, "hid-msi-claw received %d bytes, cmd: 0x%02x\n", size, buffer[4]);

	return 0;

msi_claw_raw_event_control_err:
	if (buffer != NULL)
		kfree(buffer);

	if (node != NULL)
		kfree(node);

	return ret;
}

static int msi_claw_raw_event(struct hid_device *hdev, struct hid_report *report, uint8_t *data, int size)
{
	struct msi_claw_drvdata *drvdata = hid_get_drvdata(hdev);

	if (!drvdata->control) {
		hid_notice(hdev, "hid-msi-claw event not from control interface: ignoring\n");
		return 0;
	}

	return msi_claw_raw_event_control(hdev, drvdata, report, data, size);
}

static int msi_claw_await_ack(struct hid_device *hdev)
{
	struct msi_claw_drvdata *drvdata = hid_get_drvdata(hdev);
	uint8_t buffer[MSI_CLAW_READ_SIZE];
	int ret;

	if (!drvdata->control) {
		hid_err(hdev, "hid-msi-claw couldn't find control interface\n");
		ret = -ENODEV;
		goto msi_claw_await_ack_err;
	}

	ret = msi_claw_read(hdev, buffer, MSI_CLAW_READ_SIZE, 1000);
	if (ret < 0) {
		hid_err(hdev, "hid-msi-claw failed to read ack: %d\n", ret);
		goto msi_claw_await_ack_err;
	} else if (ret != MSI_CLAW_READ_SIZE) {
		hid_err(hdev, "hid-msi-claw invalid read: expected %d bytes, got %d\n", MSI_CLAW_READ_SIZE, ret);
		ret = -EINVAL;
		goto msi_claw_await_ack_err;
	}

	if (buffer[4] != (uint8_t)MSI_CLAW_COMMAND_TYPE_ACK) {
		hid_err(hdev, "hid-msi-claw received invalid response: expected ack 0x06, got 0x%02x\n", buffer[4]);
		ret = -EINVAL;
		goto msi_claw_await_ack_err;
	}

	ret = 0;

msi_claw_await_ack_err:
	return ret;
}

static int sync_to_rom(struct hid_device *hdev)
{
	struct msi_claw_drvdata *drvdata = hid_get_drvdata(hdev);
	int ret;

	if (!drvdata->control) {
		hid_err(hdev, "hid-msi-claw couldn't find control interface\n");
		ret = -ENODEV;
		goto sync_to_rom_err;
	}

	ret = msi_claw_write_cmd(hdev, MSI_CLAW_COMMAND_TYPE_SYNC_TO_ROM, NULL, 0);
	if (ret < 0) {
		hid_err(hdev, "hid-msi-claw failed to send write request for switch controller mode: %d\n", ret);
		goto sync_to_rom_err;
	} else if (ret != MSI_CLAW_WRITE_SIZE) {
		hid_err(hdev, "hid-msi-claw failed to send the sync to rom command: %d\n", ret);
		ret = -EIO;
		goto sync_to_rom_err;
	}

	ret = msi_claw_await_ack(hdev);
	if (ret) {
		hid_err(hdev, "hid-msi-claw failed to await first ack: %d\n", ret);
		goto sync_to_rom_err;
	}

	// the sync to rom also triggers two ack
	ret = msi_claw_await_ack(hdev);
	if (ret) {
		hid_err(hdev, "hid-msi-claw failed to await second ack: %d\n", ret);
		goto sync_to_rom_err;
	}

	ret = 0;

sync_to_rom_err:
	return ret;
}

static int msi_claw_reset_device(struct hid_device *hdev)
{
	struct msi_claw_drvdata *drvdata = hid_get_drvdata(hdev);
	int ret;

	if (!drvdata->control) {
		hid_err(hdev, "hid-msi-claw couldn't find control interface\n");
		ret = -ENODEV;
		goto msi_claw_reset_device_err;
	}

	ret = msi_claw_write_cmd(hdev, MSI_CLAW_COMMAND_TYPE_RESET_DEVICE, NULL, 0);
	if (ret < 0) {
		hid_err(hdev, "hid-msi-claw failed to send reset: %d\n", ret);
		goto msi_claw_reset_device_err;
	} else if (ret != MSI_CLAW_WRITE_SIZE) {
		hid_err(hdev, "hid-msi-claw couldn't send reset request: %d\n", ret);
		ret = -EIO;
		goto msi_claw_reset_device_err;
	}

	ret = msi_claw_await_ack(hdev);
	if (ret) {
		hid_err(hdev, "hid-msi-claw failed to await ack: %d\n", ret);
		goto msi_claw_reset_device_err;
	}

msi_claw_reset_device_err:
	return ret;
}

static int msi_claw_read_gamepad_mode(struct hid_device *hdev,
	struct msi_claw_control_status *status)
{
	uint8_t buffer[MSI_CLAW_READ_SIZE] = {};
	int ret;

	ret = msi_claw_write_cmd(hdev, MSI_CLAW_COMMAND_TYPE_READ_GAMEPAD_MODE, NULL, 0);
	if (ret < 0) {
		hid_err(hdev, "hid-msi-claw failed to send read request for controller mode: %d\n", ret);
		goto msi_claw_read_gamepad_mode_err;
	} else if (ret != MSI_CLAW_WRITE_SIZE) {
		hid_err(hdev, "hid-msi-claw couldn't send request: %d\n", ret);
		ret = -EIO;
		goto msi_claw_read_gamepad_mode_err;
	}

	ret = msi_claw_read(hdev, buffer, MSI_CLAW_READ_SIZE, 50);
	if (ret != MSI_CLAW_READ_SIZE) {
		hid_err(hdev, "hid-msi-claw failed to read: %d\n", ret);
		ret = -EINVAL;
		goto msi_claw_read_gamepad_mode_err;
	}

	if (buffer[4] != (uint8_t)MSI_CLAW_COMMAND_TYPE_GAMEPAD_MODE_ACK) {
		hid_err(hdev, "hid-msi-claw received invalid response: expected 0x27, got 0x%02x\n", buffer[4]);
		ret = -EINVAL;
		goto msi_claw_read_gamepad_mode_err;
	} else if (buffer[5] >= MSI_CLAW_GAMEPAD_MODE_MAX) {
		hid_err(hdev, "hid-msi-claw unknown gamepad mode: 0x%02x\n", buffer[5]);
		ret = -EINVAL;
		goto msi_claw_read_gamepad_mode_err;
	} else if (buffer[6] >= MSI_CLAW_MKEY_FUNCTION_MAX) {
		hid_err(hdev, "hid-msi-claw unknown gamepad mode: 0x%02x\n", buffer[6]);
		ret = -EINVAL;
		goto msi_claw_read_gamepad_mode_err;
	}

	status->gamepad_mode = (enum msi_claw_gamepad_mode)buffer[5];
	status->mkeys_function = (enum msi_claw_mkeys_function)buffer[6];

	ret = 0;

msi_claw_read_gamepad_mode_err:
	return ret;
}

static int msi_claw_switch_gamepad_mode(struct hid_device *hdev,
	const struct msi_claw_control_status *status)
{
	struct msi_claw_drvdata *drvdata = hid_get_drvdata(hdev);
	struct msi_claw_control_status check_status;
	const uint8_t cmd_buffer[2] = {(uint8_t)status->gamepad_mode, (uint8_t)status->mkeys_function};
	int ret;

	if (!drvdata->control) {
		hid_err(hdev, "hid-msi-claw couldn't find control interface\n");
		ret = -ENODEV;
		goto msi_claw_switch_gamepad_mode_err;
	}

	ret = msi_claw_write_cmd(hdev, MSI_CLAW_COMMAND_TYPE_SWITCH_MODE, cmd_buffer, sizeof(cmd_buffer));
	if (ret < 0) {
		hid_err(hdev, "hid-msi-claw failed to send write request to switch controller mode: %d\n", ret);
		goto msi_claw_switch_gamepad_mode_err;
	} else if (ret != MSI_CLAW_WRITE_SIZE) {
		hid_err(hdev, "hid-msi-claw failed to write: %d bytes got written\n", ret);
		ret = -EIO;
		goto msi_claw_switch_gamepad_mode_err;
	}

	ret = msi_claw_await_ack(hdev);
	if (ret) {
		hid_err(hdev, "hid-msi-claw failed to await first ack: %d\n", ret);
		goto msi_claw_switch_gamepad_mode_err;
	}

	// the gamepad mode switch mode triggers two ack
	ret = msi_claw_await_ack(hdev);
	if (ret) {
		hid_err(hdev, "hid-msi-claw failed to await second ack: %d\n", ret);
		goto msi_claw_switch_gamepad_mode_err;
	}

	// check the new mode as official application does
	ret = msi_claw_read_gamepad_mode(hdev, &check_status);
	if (ret) {
		hid_err(hdev, "hid-msi-claw failed to read status: %d\n", ret);
		goto msi_claw_switch_gamepad_mode_err;
	}

	if (memcmp((const void *)&check_status, (const void *)status, sizeof(*status))) {
		hid_err(hdev, "hid-msi-claw current status and target one are different\n");
		ret = -EIO;
		goto msi_claw_switch_gamepad_mode_err;
	}

	// the device now sends back 03 00 00 00 00 00 00 00 00

	// this command is always issued by the windows counterpart after a mode switch
	ret = sync_to_rom(hdev);
	if (ret) {
		hid_err(hdev, "hid-msi-claw failed the sync to rom command: %d\n", ret);
		return ret;
	}

msi_claw_switch_gamepad_mode_err:
	return ret;
}

static ssize_t reset_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct hid_device *hdev = to_hid_device(dev);
	int ret;

	ret = msi_claw_reset_device(hdev);
	if (ret < 0) {
		hid_err(hdev, "hid-msi-claw error resetting device: %d\n", ret);
		goto reset_store_err;
	}

	return count;

reset_store_err:
	return ret;
}
static DEVICE_ATTR_WO(reset);

static ssize_t gamepad_mode_available_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	int i, ret = 0;
	int len = ARRAY_SIZE(gamepad_mode_map);

	for (i = 0; i < len; i++) {
		if (!gamepad_mode_map[i].available)
			continue;

		ret += sysfs_emit_at(buf, ret, "%s", gamepad_mode_map[i].name);

		if (i < len-1)
			ret += sysfs_emit_at(buf, ret, " ");
	}
	ret += sysfs_emit_at(buf, ret, "\n");

	return ret;
}
static DEVICE_ATTR_RO(gamepad_mode_available);

static ssize_t gamepad_mode_current_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct hid_device *hdev = to_hid_device(dev);
	struct msi_claw_control_status status;
	int ret;

	ret = msi_claw_read_gamepad_mode(hdev, &status);
	if (ret) {
		hid_err(hdev, "hid-msi-claw error reaging the gamepad mode: %d\n", ret);
		return ret;
	}

	return sysfs_emit(buf, "%s\n", gamepad_mode_map[(int)status.gamepad_mode].name);
}

static ssize_t gamepad_mode_current_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	ssize_t ret;
	uint8_t *input;
	struct hid_device *hdev = to_hid_device(dev);
	struct msi_claw_drvdata *drvdata = hid_get_drvdata(hdev);
	enum msi_claw_gamepad_mode new_gamepad_mode = ARRAY_SIZE(gamepad_mode_map);
	struct msi_claw_control_status status = {
		.gamepad_mode = drvdata->control->gamepad_mode,
		.mkeys_function = drvdata->control->mkeys_function,
	};

	if (!count) {
		ret = -EINVAL;
		goto gamepad_mode_current_store_err;
	}

	input = kmemdup(buf, count+1, GFP_KERNEL);
	if (!input) {
		ret = -ENOMEM;
		goto gamepad_mode_current_store_err;
	}

	input[count] = '\0';
	if (input[count-1] == '\n')
		input[count-1] = '\0';

	for (size_t i = 0; i < (size_t)new_gamepad_mode; i++)
		if ((!strcmp(input, gamepad_mode_map[i].name)) && (gamepad_mode_map[i].available))
			new_gamepad_mode = (enum msi_claw_gamepad_mode)i;

	kfree(input);

	if (new_gamepad_mode == ARRAY_SIZE(gamepad_mode_map)) {
		hid_err(hdev, "Invalid gamepad mode selected\n");
		ret = -EINVAL;
		goto gamepad_mode_current_store_err;
	}

	status.gamepad_mode = new_gamepad_mode;
	ret = msi_claw_switch_gamepad_mode(hdev, &status);
	if (ret) {
		hid_err(hdev, "Error changing gamepad mode: %d\n", (int)ret);
		goto gamepad_mode_current_store_err;
	}

	ret = count;

gamepad_mode_current_store_err:
	return ret;
}
static DEVICE_ATTR_RW(gamepad_mode_current);

static ssize_t mkeys_function_available_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	int i, ret = 0;
	int len = ARRAY_SIZE(mkeys_function_map);

	for (i = 0; i < len; i++) {
		ret += sysfs_emit_at(buf, ret, "%s", mkeys_function_map[i]);

		if (i < len-1)
			ret += sysfs_emit_at(buf, ret, " ");
	}
	ret += sysfs_emit_at(buf, ret, "\n");

	return ret;
}
static DEVICE_ATTR_RO(mkeys_function_available);

static ssize_t mkeys_function_current_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct hid_device *hdev = to_hid_device(dev);
	struct msi_claw_control_status status;
	int ret = msi_claw_read_gamepad_mode(hdev, &status);

	if (ret) {
		hid_err(hdev, "hid-msi-claw error reaging the gamepad mode: %d\n", ret);
		return ret;
	}

	return sysfs_emit(buf, "%s\n", mkeys_function_map[(int)status.mkeys_function]);
}

static ssize_t mkeys_function_current_store(struct device *dev, struct device_attribute *attr,
					const char *buf, size_t count)
{
	uint8_t *input;
	ssize_t err;
	struct hid_device *hdev = to_hid_device(dev);
	struct msi_claw_drvdata *drvdata = hid_get_drvdata(hdev);
	enum msi_claw_mkeys_function new_mkeys_function = ARRAY_SIZE(mkeys_function_map);
	struct msi_claw_control_status status = {
		.gamepad_mode = drvdata->control->gamepad_mode,
		.mkeys_function = drvdata->control->mkeys_function,
	};

	if (!count)
		return -EINVAL;

	input = kmemdup(buf, count+1, GFP_KERNEL);
	if (!input)
		return -ENOMEM;

	input[count] = '\0';
	if (input[count-1] == '\n')
		input[count-1] = '\0';

	for (size_t i = 0; i < (size_t)new_mkeys_function; i++)
		if (!strcmp(input, mkeys_function_map[i]))
			new_mkeys_function = i;

	kfree(input);

	if (new_mkeys_function == ARRAY_SIZE(mkeys_function_map)) {
		hid_err(hdev, "Invalid mkeys function selected\n");
		return -EINVAL;
	}

	status.mkeys_function = new_mkeys_function;
	err = msi_claw_switch_gamepad_mode(hdev, &status);
	if (err) {
		hid_err(hdev, "Error changing mkeys function: %d\n", (int)err);
		return err;
	}

	return count;
}
static DEVICE_ATTR_RW(mkeys_function_current);

static int __maybe_unused msi_claw_resume(struct hid_device *hdev)
{
	int ret;
	struct msi_claw_drvdata *drvdata = hid_get_drvdata(hdev);
	struct msi_claw_control_status status = {
		.gamepad_mode = drvdata->control->gamepad_mode,
		.mkeys_function = drvdata->control->mkeys_function,
	};

	// TODO: clear out events list here (or in suspend?)

	// wait for device to be ready
	msleep(500);

	ret = msi_claw_switch_gamepad_mode(hdev, &status);
	if (ret) {
		hid_err(hdev, "Error changing gamepad mode: %d\n", (int)ret);
		goto msi_claw_resume_err;
	}

	// TODO: retry until this works?

	return 0;

msi_claw_resume_err:
	return ret;
}

static int msi_claw_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	int ret;
	struct msi_claw_drvdata *drvdata;

	if (!hid_is_usb(hdev)) {
		hid_err(hdev, "hid-msi-claw hid not usb\n");
		return -ENODEV;
	}

	drvdata = devm_kzalloc(&hdev->dev, sizeof(*drvdata), GFP_KERNEL);
	if (drvdata == NULL) {
		hid_err(hdev, "hid-msi-claw can't alloc descriptor\n");
		return -ENOMEM;
	}

	mutex_init(&drvdata->read_data_mutex);
	drvdata->read_data = NULL;
	drvdata->control = NULL;

	hid_set_drvdata(hdev, drvdata);

	ret = hid_parse(hdev);
	if (ret) {
		hid_err(hdev, "hid-msi-claw hid parse failed: %d\n", ret);
		return ret;
	}

	ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
	if (ret) {
		hid_err(hdev, "hid-msi-claw hw start failed: %d\n", ret);
		return ret;
	}

	ret = hid_hw_open(hdev);
	if (ret) {
		hid_err(hdev, "hid-msi-claw failed to open HID device: %d\n", ret);
		goto err_stop_hw;
	}

	if (hdev->rdesc[0] == MSI_CLAW_DEVICE_CONTROL_DESC) {
		drvdata->control = devm_kzalloc(&hdev->dev, sizeof(*(drvdata->control)), GFP_KERNEL);
		if (drvdata->control == NULL) {
			hid_err(hdev, "hid-msi-claw can't alloc control interface data\n");
			ret = -ENOMEM;
			goto err_close;
		}

		drvdata->control->gamepad_mode = MSI_CLAW_GAMEPAD_MODE_XINPUT;
		drvdata->control->mkeys_function = MSI_CLAW_MKEY_FUNCTION_MACRO;

		ret = sysfs_create_file(&hdev->dev.kobj, &dev_attr_gamepad_mode_available.attr);
		if (ret) {
			hid_err(hdev, "hid-msi-claw failed to sysfs_create_file dev_attr_gamepad_mode_available: %d\n", ret);
			goto err_close;
		}

		ret = sysfs_create_file(&hdev->dev.kobj, &dev_attr_gamepad_mode_current.attr);
		if (ret) {
			hid_err(hdev, "hid-msi-claw failed to sysfs_create_file dev_attr_gamepad_mode_current: %d\n", ret);
			goto err_dev_attr_gamepad_mode_current;
		}

		ret = sysfs_create_file(&hdev->dev.kobj, &dev_attr_mkeys_function_available.attr);
		if (ret) {
			hid_err(hdev, "hid-msi-claw failed to sysfs_create_file dev_attr_mkeys_function_available: %d\n", ret);
			goto err_dev_attr_mkeys_function_available;
		}

		ret = sysfs_create_file(&hdev->dev.kobj, &dev_attr_mkeys_function_current.attr);
		if (ret) {
			hid_err(hdev, "hid-msi-claw failed to sysfs_create_file dev_attr_mkeys_function_current: %d\n", ret);
			goto err_dev_attr_mkeys_function_current;
		}

		ret = sysfs_create_file(&hdev->dev.kobj, &dev_attr_reset.attr);
		if (ret) {
			hid_err(hdev, "hid-msi-claw failed to sysfs_create_file dev_attr_reset: %d\n", ret);
			goto err_dev_attr_reset;
		}
	}

	return 0;

err_dev_attr_gamepad_mode_current:
	sysfs_remove_file(&hdev->dev.kobj, &dev_attr_gamepad_mode_available.attr);
err_dev_attr_mkeys_function_available:
	sysfs_remove_file(&hdev->dev.kobj, &dev_attr_gamepad_mode_current.attr);
err_dev_attr_mkeys_function_current:
	sysfs_remove_file(&hdev->dev.kobj, &dev_attr_mkeys_function_available.attr);
err_dev_attr_reset:
	sysfs_remove_file(&hdev->dev.kobj, &dev_attr_mkeys_function_current.attr);
err_close:
	hid_hw_close(hdev);
err_stop_hw:
	hid_hw_stop(hdev);
	return ret;
}

static void msi_claw_remove(struct hid_device *hdev)
{
	struct msi_claw_drvdata *drvdata = hid_get_drvdata(hdev);

	if (drvdata->control) {
		sysfs_remove_file(&hdev->dev.kobj, &dev_attr_gamepad_mode_available.attr);
		sysfs_remove_file(&hdev->dev.kobj, &dev_attr_gamepad_mode_current.attr);
		sysfs_remove_file(&hdev->dev.kobj, &dev_attr_mkeys_function_available.attr);
		sysfs_remove_file(&hdev->dev.kobj, &dev_attr_mkeys_function_current.attr);
		sysfs_remove_file(&hdev->dev.kobj, &dev_attr_reset.attr);
	}

	hid_hw_close(hdev);
	hid_hw_stop(hdev);
}

static const struct hid_device_id msi_claw_devices[] = {
	{ HID_USB_DEVICE(0x0DB0, 0x1901) },
	{ }
};
MODULE_DEVICE_TABLE(hid, msi_claw_devices);

static struct hid_driver msi_claw_driver = {
	.name			= "hid-msi-claw",
	.id_table		= msi_claw_devices,
	.raw_event		= msi_claw_raw_event,
	.probe			= msi_claw_probe,
	.remove			= msi_claw_remove,
#ifdef CONFIG_PM
	.resume			= msi_claw_resume,
#endif
};
module_hid_driver(msi_claw_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Denis Benato <benato.denis96@gmail.com>");
MODULE_DESCRIPTION("Manage MSI Claw gamepad device");
