#ifndef CACHYOS_T2_H
#define CACHYOS_T2_H

#include <linux/dmi.h>


#ifndef T2_MAC
#define T2_MAC(vendor, product) \
	 .matches = { \
		DMI_MATCH(DMI_BOARD_VENDOR, vendor), \
		DMI_MATCH(DMI_PRODUCT_NAME, product), \
	},
#endif

static const struct dmi_system_id t2_mac_tbl[] = {
	{ T2_MAC("Apple Inc.", "MacBookPro15,1") },
	{ T2_MAC("Apple Inc.", "MacBookPro15,2") },
	{ T2_MAC("Apple Inc.", "MacBookPro15,3") },
	{ T2_MAC("Apple Inc.", "MacBookPro15,4") },
	{ T2_MAC("Apple Inc.", "MacBookPro16,1") },
	{ T2_MAC("Apple Inc.", "MacBookPro16,2") },
	{ T2_MAC("Apple Inc.", "MacBookPro16,3") },
	{ T2_MAC("Apple Inc.", "MacBookPro16,4") },
	{ T2_MAC("Apple Inc.", "MacBookAir8,1") },
	{ T2_MAC("Apple Inc.", "MacBookAir8,2") },
	{ T2_MAC("Apple Inc.", "MacBookAir9,1") },
	{ T2_MAC("Apple Inc.", "Macmini8,1") },
	{ T2_MAC("Apple Inc.", "MacPro7,1") },
	{ T2_MAC("Apple Inc.", "iMac20,1") },
	{ T2_MAC("Apple Inc.", "iMac20,2") },
	{ T2_MAC("Apple Inc.", "iMacPro1,1") },
	{ }
};

#endif
