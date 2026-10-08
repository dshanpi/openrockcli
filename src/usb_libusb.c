/* Linux/macOS transport. Windows builds use usb_windows.c, without libusb. */
#ifndef _WIN32
#include <libusb.h>
#include <rockchip_backend.h>
#include <usb_transport.h>
struct rk_usb_handle {
 libusb_context *context;
 libusb_device_handle *hdl;
 int epin, epout;
};
static int openrockcli_is_adb(libusb_device * device)
{
	struct libusb_config_descriptor * config = NULL;
	int adb = 0;
	if(libusb_get_active_config_descriptor(device, &config) != 0)
		return 0;
	for(int i = 0; i < config->bNumInterfaces && !adb; i++)
	{
		const struct libusb_interface * iface = &config->interface[i];
		for(int j = 0; j < iface->num_altsetting; j++)
		{
			const struct libusb_interface_descriptor * desc = &iface->altsetting[j];
			if(desc->bInterfaceClass == 0xff && desc->bInterfaceSubClass == 0x42 &&
				desc->bInterfaceProtocol == 0x01)
			{
				adb = 1;
				break;
			}
		}
	}
	libusb_free_config_descriptor(config);
	return adb;
}

static int backend_port_string(libusb_device * device, char * out, size_t out_size)
{
	uint8_t ports[8] = { 0 };
	int count = libusb_get_port_numbers(device, ports, sizeof(ports));
	int pos = 0;
	if(!out || !out_size)
		return 0;
	out[0] = 0;
	for(int i = 0; i < count && pos < (int)out_size - 1; i++)
		pos += snprintf(out + pos, out_size - pos, "%s%u", i ? "." : "", ports[i]);
	if(count <= 0)
		snprintf(out, out_size, "-");
	return count;
}

int rk_usb_scan(struct rockchip_device_info * devices, int max_devices)
{
	libusb_context * context = NULL;
	libusb_device ** list = NULL;
	ssize_t count;
	int found = 0;
	if(!devices || max_devices <= 0) return RK_USB_ERROR_INVALID_PARAM;
    int init = libusb_init(&context);
    if(init) return init;
	count = libusb_get_device_list(context, &list);
	if(count < 0)
	{
		libusb_exit(context);
		return (int)count;
	}
	for(ssize_t i = 0; i < count; i++)
	{
		struct libusb_device_descriptor desc;
		struct rockchip_device_info * out;
		if(libusb_get_device_descriptor(list[i], &desc) != 0 || desc.idVendor != 0x2207 ||
			openrockcli_is_adb(list[i]))
			continue;
		if(found == max_devices) { found = RK_USB_ERROR_OVERFLOW; break; }
		out = &devices[found++];
		memset(out, 0, sizeof(*out));
		out->bus = libusb_get_bus_number(list[i]);
		out->vid = desc.idVendor;
		out->pid = desc.idProduct;
		backend_port_string(list[i], out->port, sizeof(out->port));
		snprintf(out->mode, sizeof(out->mode), "%s",
			(desc.bcdUSB & 1) ? "loader" : "maskrom");
		if(!strcmp(out->mode, "maskrom") && desc.iProduct)
		{
			libusb_device_handle * handle = NULL;
			unsigned char product[128] = { 0 };
			if(libusb_open(list[i], &handle) == 0)
			{
				if(libusb_get_string_descriptor_ascii(handle, desc.iProduct, product,
					sizeof(product) - 1) > 0 &&
					(strstr((char *)product, "USB-MSC") || strstr((char *)product, "Loader") ||
					 strstr((char *)product, "download")))
					snprintf(out->mode, sizeof(out->mode), "loader");
				libusb_close(handle);
			}
		}
		snprintf(out->chip, sizeof(out->chip), "%s", openrockcli_chip_name(desc.idProduct));
	}
	libusb_free_device_list(list, 1);
	libusb_exit(context);
	return found;
}

static int libusb_open_match(struct openrockcli_ctx_t * ctx, uint8_t bus, const char * port)
{
	libusb_device ** list = NULL;
	ssize_t count;
	if(!ctx)
		return 0;

	ctx->usb->hdl = NULL;
	ctx->last_usb_error = LIBUSB_ERROR_NO_DEVICE;
	count = libusb_get_device_list(ctx->usb->context, &list);
	if(count < 0)
	{
		ctx->last_usb_error = (int)count;
		return 0;
	}
	for(ssize_t index = 0; index < count; index++)
	{
		struct libusb_device_descriptor desc;
		libusb_device * device = list[index];
		libusb_device_handle * hdl = NULL;
		int rc;
		if(libusb_get_device_descriptor(device, &desc) != 0 || desc.idVendor != 0x2207)
			continue;
		if(openrockcli_is_adb(device))
			continue;
		if(bus && libusb_get_bus_number(device) != bus)
			continue;
		if(port && port[0])
		{
			uint8_t ports[8] = { 0 };
			char actual[32] = { 0 };
			int nports = libusb_get_port_numbers(device, ports, sizeof(ports));
			int pos = 0;
			for(int p = 0; p < nports && pos < (int)sizeof(actual) - 1; p++)
				pos += snprintf(actual + pos, sizeof(actual) - pos, "%s%u", p ? "." : "", ports[p]);
			if(strcmp(actual, port))
				continue;
		}
		rc = libusb_open(device, &hdl);
		if(rc != 0)
		{
			ctx->last_usb_error = rc;
			continue;
		}
		ctx->usb->hdl = hdl;
		ctx->pid = desc.idProduct;
#ifndef _WIN32
		if(libusb_kernel_driver_active(ctx->usb->hdl, 0))
			libusb_detach_kernel_driver(ctx->usb->hdl, 0);
#endif
		rc = libusb_claim_interface(ctx->usb->hdl, 0);
		if(rc == 0)
		{
			struct libusb_config_descriptor * config = NULL;
			ctx->maskrom = (desc.bcdUSB & 0x0001) == 0x0000;
			if(ctx->maskrom && desc.iProduct)
			{
				unsigned char product[128] = { 0 };
				if(libusb_get_string_descriptor_ascii(ctx->usb->hdl, desc.iProduct,
					product, sizeof(product) - 1) > 0 &&
					(strstr((char *)product, "USB-MSC") || strstr((char *)product, "Loader") ||
					 strstr((char *)product, "download")))
					ctx->maskrom = 0;
			}
			rc = libusb_get_active_config_descriptor(device, &config);
			if(rc == 0)
			{
				for(int if_idx = 0; if_idx < config->bNumInterfaces; if_idx++)
				{
					const struct libusb_interface * iface = config->interface + if_idx;
					for(int set_idx = 0; set_idx < iface->num_altsetting; set_idx++)
					{
						const struct libusb_interface_descriptor * setting = iface->altsetting + set_idx;
						for(int ep_idx = 0; ep_idx < setting->bNumEndpoints; ep_idx++)
						{
							const struct libusb_endpoint_descriptor * ep = setting->endpoint + ep_idx;
							if((ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) != LIBUSB_TRANSFER_TYPE_BULK)
								continue;
							if((ep->bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_IN)
								ctx->usb->epin = ep->bEndpointAddress;
							else
								ctx->usb->epout = ep->bEndpointAddress;
						}
					}
				}
				libusb_free_config_descriptor(config);
				ctx->last_usb_error = 0;
				libusb_free_device_list(list, 1);
				return 1;
			}
			libusb_release_interface(ctx->usb->hdl, 0);
		}
		ctx->last_usb_error = rc;
		libusb_close(ctx->usb->hdl);
		ctx->usb->hdl = NULL;
	}
	libusb_free_device_list(list, 1);
	return 0;
}


int rk_usb_open(struct openrockcli_ctx_t *ctx, uint8_t bus, const char *port)
{
 int rc;
 ctx->usb = calloc(1, sizeof(*ctx->usb));
 if(!ctx->usb) return RK_USB_ERROR_NO_MEM;
 rc = libusb_init(&ctx->usb->context);
 if(rc == 0 && libusb_open_match(ctx, bus, port)) return 0;
 if(rc == 0) rc = ctx->last_usb_error;
 rk_usb_close(ctx);
 return rc;
}
void rk_usb_close(struct openrockcli_ctx_t *ctx)
{
 if(!ctx || !ctx->usb) return;
 if(ctx->usb->hdl) {
  libusb_release_interface(ctx->usb->hdl, 0);
  libusb_close(ctx->usb->hdl);
 }
 if(ctx->usb->context) libusb_exit(ctx->usb->context);
 free(ctx->usb);
 ctx->usb = NULL;
}
int rk_usb_has_bulk(const struct openrockcli_ctx_t *ctx)
{
 return ctx && ctx->usb && ctx->usb->hdl && ctx->usb->epin && ctx->usb->epout;
}
int rk_usb_control(struct openrockcli_ctx_t *ctx, uint16_t code, void *data, int size)
{
 int rc = libusb_control_transfer(ctx->usb->hdl, LIBUSB_REQUEST_TYPE_VENDOR,
  0x0c, 0, code, data, size, 5000);
 if(rc < 0) ctx->last_usb_error = rc;
 return rc;
}
int rk_usb_bulk(struct openrockcli_ctx_t *ctx, int input, void *data, int size, int *done)
{
 int rc = libusb_bulk_transfer(ctx->usb->hdl,
  input ? ctx->usb->epin : ctx->usb->epout, data, size, done, 5000);
 if(rc < 0) ctx->last_usb_error = rc;
 return rc;
}
#endif
