const fs = require('fs');
const path = require('path');

const repositoryRoot = path.resolve(__dirname, '..');
const moduleRoot = process.env.CODEXPETS_EDGE_TTS_MODULE_ROOT ||
  path.join(repositoryRoot, '.tools', 'edge-tts-node', 'node_modules', 'msedge-tts');

let ttsModule;
try {
  ttsModule = require(moduleRoot);
} catch (error) {
  throw new Error(
    'Missing msedge-tts. Install it without adding it to the product runtime:\n' +
    '  npm install --prefix .tools/edge-tts-node msedge-tts@2.0.7 --no-audit --no-fund\n' +
    error.message);
}

const { MsEdgeTTS, OUTPUT_FORMAT } = ttsModule;
const outputDirectory = path.join(repositoryRoot, 'assets', 'audio');
const prompts = [
  ['voice-start.mp3', '任务开始啦！'],
  ['voice-complete.mp3', '任务完成啦！'],
  ['voice-error.mp3', '任务出错啦！'],
  ['voice-interrupted.mp3', '任务已中断。'],
];

// Yunxia is Microsoft Edge's Chinese cartoon boy neural voice. Keep every event
// on one voice and one prosody profile so the pet has a single consistent
// timbre.
const voice = 'zh-CN-YunxiaNeural';
const prosody = { rate: '0%', pitch: '+12Hz' };

(async () => {
  const temporaryDirectory = path.join(repositoryRoot, '.tools', 'generated-audio');
  fs.mkdirSync(temporaryDirectory, { recursive: true });
  fs.mkdirSync(outputDirectory, { recursive: true });

  const tts = new MsEdgeTTS();
  try {
    await tts.setMetadata(voice, OUTPUT_FORMAT.AUDIO_24KHZ_48KBITRATE_MONO_MP3);
    for (const [fileName, text] of prompts) {
      const { audioFilePath } = await tts.toFile(temporaryDirectory, text, prosody);
      const destination = path.join(outputDirectory, fileName);
      fs.copyFileSync(audioFilePath, destination);
      console.log(`${fileName}: ${text}`);
    }
  } finally {
    tts.close();
  }
})().catch((error) => {
  console.error(error);
  process.exit(1);
});
