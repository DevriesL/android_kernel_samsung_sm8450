ifneq ($(CONFIG_SAMSUNG_PRODUCT_SHIP), y)
	obj-$(CONFIG_SEC_AUTO_INPUT)            += sec_auto_input.o
else
	obj-$(CONFIG_SEC_AUTO_INPUT_NOT_SET)    += sec_auto_input.o
endif
subdir-ccflags-y := -Wformat