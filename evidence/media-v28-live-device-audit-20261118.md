# media-v28 live media-device audit

This is a read-only serial audit of the running `6.18.63-t736-media-v28`
image. No display-manager restart, flash, or desktop configuration change was
performed during the audit.

Observed on the board:

- `t736_cedar_wc` is built into the kernel and `/dev/cedar_dev` exists, but
  both `/dev/cedar_dev` and `/dev/dma_heap/t736-ve` are `root:root` mode `0600`.
- There are no `/dev/video*` V4L2 nodes and no Cedar/OMX broker, browser bridge,
  or media service installed. The only `/opt` payload is the PVR GPU runtime.
- FFmpeg 7.1.5 and GStreamer core/base/good packages are installed, but
  `gstreamer1.0-tools` is absent, so `gst-inspect-1.0` and `gst-launch-1.0`
  are unavailable.
- The generic FFmpeg `h264_v4l2m2m` decoder was tested against a temporary
  software-generated H.264 clip and failed with `Could not find a valid
  device`; this is expected while no V4L2 decoder node is present.
- The udev rules currently cover unrelated Qualcomm/Rockchip V4L2 devices and
  do not grant access to the T736 Cedar devices.

This audit does not claim hardware video decode or encode acceptance. A
complete image still needs the private Cedar/OMX payload, matching broker and
Firefox bridge, service units, device rules, and native board frame-count
tests for 1080p60, 4K, encode, switching, and long-running playback.
