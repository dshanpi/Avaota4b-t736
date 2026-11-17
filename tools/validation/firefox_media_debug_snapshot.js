/* Read-only Marionette async script for an existing selected content tab.
 * Use context="content", sandbox="system", newSandbox=true. The ESR153
 * Marionette implementation explicitly maps that sandbox name to the system
 * principal while retaining the selected content Window.
 * No playback controls, preferences, URL changes, or persistent state writes.
 */
const finish = arguments[arguments.length - 1];
const videos = document.querySelectorAll("video");
const started = performance.now();
if (videos.length !== 1) {
  finish({ error: "Expected exactly one video", videoCount: videos.length });
} else {
  const video = videos[0];
  if (typeof video.mozRequestDebugInfo !== "function") {
    finish({ error: "mozRequestDebugInfo unavailable in this sandbox" });
  } else {
    video.mozRequestDebugInfo().then(
      info => {
        const quality = video.getVideoPlaybackQuality();
        finish({
          sampleStartMs: started,
          sampleEndMs: performance.now(),
          mediaTime: video.currentTime,
          paused: video.paused,
          ended: video.ended,
          readyState: video.readyState,
          width: video.videoWidth,
          height: video.videoHeight,
          playbackRate: video.playbackRate,
          parsed: video.mozParsedFrames,
          decoded: video.mozDecodedFrames,
          presented: video.mozPresentedFrames,
          painted: video.mozPaintedFrames,
          frameDelaySeconds: video.mozFrameDelay,
          qualityTotal: quality.totalVideoFrames,
          qualityDropped: quality.droppedVideoFrames,
          info: JSON.parse(JSON.stringify(info)),
        });
      },
      error => finish({ error: String(error), sampleStartMs: started }),
    );
  }
}
