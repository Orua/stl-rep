window.OPENBW_VIDEO_EXPORT_CONFIG = Object.freeze({
  width: 1280,
  height: 960,
  fps: 24,
  videoBitrateMbps: 5,
  replaySpeed: 1024,
  pollIntervalMs: 100,
  extension: "webm",
  mimeTypes: [
    "video/webm;codecs=vp9",
    "video/webm;codecs=vp8",
    "video/webm"
  ],
  modalTitle: "正在导出视频",
  modalMessage: "从录像起点以最高速度录制 WebM 视频。"
});
