import { readConfig } from './config.mjs';
import { Discord } from './discord.mjs';

try {
  const config = readConfig();
  await new Discord(config).register();
  console.log('Registered /hailwire link and /hailwire unlink in your Discord server.');
} catch (error) {
  console.error(error.code ? `Discord command registration failed (${error.code}). Check the bot token, server ID, and bot installation.` : error.message);
  process.exitCode = 1;
}
