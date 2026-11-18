# T736 HDMI browser modeset diagnosis

The board's Xorg output is named `HDMI-1`; the kernel DRM connector is
`card0-HDMI-A-1`.  Mixing those names in the Xorg monitor section causes the
1080p preference to be ignored.

During the browser-triggered failure, the board logged an HDMI atomic disable,
then selected `2560x1440` from the monitor EDID, and enabled the output again.
The same log also reported `dynamic range 1 unsupport`.  The desktop had been
configured for `1920x1080`, so the mode change reset the display engine and the
software cursor together.  USB HID did not disconnect or reset.

The persistent desktop configuration now keeps the Xorg/Mutter name as
`HDMI-1`, stores `1920x1080@60` for both GDM and `t736`, and runs a one-shot
session initializer that restores 1080p and SDR only when the current session
does not already match.  It does not restart GDM or continuously poll the
display.

Remaining kernel-side evidence is the vendor DRM message
`sunxi_fbdev_plane_update timeout`; a future kernel patch should guard that
path when the CRTC is inactive.  This evidence does not claim that patch has
been validated on hardware.
