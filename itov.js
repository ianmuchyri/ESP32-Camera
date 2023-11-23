//image to video script file
//ffmpeg -framerate 30 -pattern_type glob -i '*.jpg' -c:v libx264 -pix_fmt yuv420p ../../videos/out.mp4
import { exec } from 'child_process';

const command = `ffmpeg -framerate 8 -pattern_type glob -i 'images/${process.argv[2]}/*.jpg' -c:v libx264 -pix_fmt yuv420p videos/${process.argv[2]}.mp4`;

exec(command, (error, stdout, stderr) => {
  if (error) {
    console.error(`exec error: ${error}`);
    return;
  }
  console.log(`stdout: ${stdout}`);
  console.error(`stderr: ${stderr}`);
});
