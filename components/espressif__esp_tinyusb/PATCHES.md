# Local patch to espressif/esp_tinyusb 2.1.1

This is a vendored copy of the managed component, checked in so the build is
reproducible with one local change.

## Why it is vendored

`tinyusb_msc.c` defines the six `tud_msc_*` SCSI callbacks unconditionally and
strongly, and only supports its own FAT-on-flash / FAT-on-SD backends. p4kvm
needs to present an arbitrary raw block image (a bootable `.img`/`.iso` held in
PSRAM, later SD/network), so `main/usb_msc.c` provides its own strong
`tud_msc_*` definitions. With the upstream strong symbols that is a duplicate-
definition link error.

## The change

The six callbacks in `tinyusb_msc.c` are marked `__attribute__((weak))` so the
application's strong definitions in `main/usb_msc.c` override them at link time:

- `tud_msc_inquiry_cb`
- `tud_msc_test_unit_ready_cb`
- `tud_msc_capacity_cb`
- `tud_msc_read10_cb`
- `tud_msc_write10_cb`
- `tud_msc_scsi_cb`

Nothing else diverges from upstream 2.1.1. When bumping the component, re-apply
these six `__attribute__((weak))` prefixes (verify with
`nm main/.../usb_msc.c.obj | grep tud_msc` → `T`, and the component's
`tinyusb_msc.c.obj` → `W`).
