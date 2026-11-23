import { resolve } from 'node:path';

export const isSnowflake = (value) => typeof value === 'string' && /^[1-9]\d{16,19}$/.test(value);

export function readConfig(env = process.env) {
  for (const key of ['DISCORD_APPLICATION_ID', 'DISCORD_GUILD_ID']) {
    if (!isSnowflake(env[key])) throw new Error(`Set ${key} to its ID from Discord.`);
  }
  if (!/^[a-f0-9]{64}$/i.test(env.DISCORD_PUBLIC_KEY ?? '')) {
    throw new Error('Set DISCORD_PUBLIC_KEY to the public key from the Discord Developer Portal.');
  }
  if (!/^[\w.-]{20,512}$/.test(env.DISCORD_BOT_TOKEN ?? '')) {
    throw new Error('Set DISCORD_BOT_TOKEN to the bot token from the Discord Developer Portal.');
  }

  const port = Number(env.HAILWIRE_PORT ?? 8787);
  if (!Number.isInteger(port) || port < 1 || port > 65535) throw new Error('Set HAILWIRE_PORT to a valid port.');

  let guildUrl;
  if (env.HAILWIRE_GUILD_URL) {
    let url;
    try { url = new URL(env.HAILWIRE_GUILD_URL); } catch { /* checked below */ }
    if (url?.protocol !== 'https:' || url.username || url.password || url.port || url.search || url.hash ||
        !((url.hostname === 'discord.gg' && /^\/[A-Za-z0-9-]+\/?$/.test(url.pathname)) ||
          (url.hostname === 'discord.com' && /^\/(invite\/[A-Za-z0-9-]+|channels\/[0-9]+(?:\/[0-9]+)?)\/?$/.test(url.pathname)))) {
      throw new Error('Set HAILWIRE_GUILD_URL to a discord.gg invite or a discord.com server or channel URL.');
    }
    guildUrl = url.href;
  }

  return {
    applicationId: env.DISCORD_APPLICATION_ID,
    guildId: env.DISCORD_GUILD_ID,
    publicKey: env.DISCORD_PUBLIC_KEY,
    botToken: env.DISCORD_BOT_TOKEN,
    guildUrl,
    dataDir: resolve(env.HAILWIRE_DATA_DIR || 'data'),
    bind: env.HAILWIRE_BIND || '127.0.0.1',
    port,
    trustProxy: env.HAILWIRE_TRUST_PROXY === '1',
  };
}
