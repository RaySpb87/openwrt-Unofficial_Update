BOARDNAME:=Devices with small flash
FEATURES += squashfs small_flash

DEFAULT_PACKAGES += wpad-mini
DEFAULT_PACKAGES := $(filter-out opkg,$(DEFAULT_PACKAGES))
DEFAULT_PACKAGES.router := $(filter-out ppp ppp-mod-pppoe,$(DEFAULT_PACKAGES.router))

define Target/Description
	Build firmware images for Atheros AR71xx/AR913x/AR934x based boards with small flash
endef
