import { setTimeout as sleep } from 'node:timers/promises';
import { isSnowflake } from './config.mjs';

const API = 'https://discord.com/api/v10';
const OMIT_MENTIONS = { parse: [] };
const DM_COLOR = 0x4aabe0;

export class DiscordError extends Error {
  constructor(code, retryAfter) {
    super(code);
    this.code = code;
    this.retryAfter = retryAfter;
  }
}

const escapeMarkdown = (text) => text.replace(/[\\`*_{}\[\]()<>#+|~]/g, '\\$&');

export function tellMessage(tell, sentAt, nonce) {
  return {
    allowed_mentions: OMIT_MENTIONS,
    nonce,
    enforce_nonce: true,
    embeds: [{
      title: 'Incoming tell',
      description: escapeMarkdown(tell.message),
      color: DM_COLOR,
      fields: [
        { name: 'From', value: escapeMarkdown(tell.sender), inline: true },
        { name: 'To', value: escapeMarkdown(tell.character), inline: true },
        { name: 'Server', value: escapeMarkdown(tell.server), inline: true },
      ],
      footer: { text: 'Hailwire · Hailstone' },
      timestamp: tell.timestamp ?? new Date(sentAt).toISOString(),
    }],
  };
}

export function testMessage(linking) {
  return {
    allowed_mentions: OMIT_MENTIONS,
    embeds: [{
      title: linking ? 'Hailwire is linked' : 'Hailwire test',
      description: linking
        ? 'Hailwire can deliver DMs to you. Return to Hailstone to finish linking and enable tell forwarding. To disconnect all clients, use `/hailwire unlink` in the Discord server where you linked.'
        : 'Discord delivery is working for this Hailstone client.',
      color: DM_COLOR,
    }],
  };
}

export class Discord {
  constructor(config, fetcher = fetch) {
    this.config = config;
    this.fetcher = fetcher;
    this.queue = [];
    this.queuedPerUser = new Map();
    this.running = false;
    this.retryAt = 0;
  }

  async request(method, path, body, authenticated = true) {
    if (authenticated && this.retryAt > Date.now()) {
      throw new DiscordError('discord_rate_limited', Math.ceil((this.retryAt - Date.now()) / 1000));
    }
    const headers = { 'Content-Type': 'application/json', 'User-Agent': 'DiscordBot (https://github.com/fryguy503/Hailwire, 1.0)' };
    if (authenticated) headers.Authorization = `Bot ${this.config.botToken}`;
    let response;
    let data;
    try {
      response = await this.fetcher(API + path, {
        method,
        headers,
        body: JSON.stringify(body),
        signal: AbortSignal.timeout(5000),
        redirect: 'error',
      });
      data = await response.json().catch(() => null);
    } catch {
      throw new DiscordError('discord_unavailable');
    }
    if (response.ok) return data;
    if (response.status === 429) {
      const retryAfter = Math.max(1, Math.ceil(Number(response.headers.get('retry-after') ?? data?.retry_after) || 1));
      if (authenticated && (data?.global || response.headers.get('x-ratelimit-global') === 'true')) {
        this.retryAt = Date.now() + retryAfter * 1000;
      }
      throw new DiscordError('discord_rate_limited', retryAfter);
    }
    if (response.status === 403 || response.status === 404) throw new DiscordError('discord_dm_unavailable');
    throw new DiscordError('discord_unavailable');
  }

  async createDM(userId) {
    const channel = await this.request('POST', '/users/@me/channels', { recipient_id: userId });
    if (!isSnowflake(channel?.id)) throw new DiscordError('discord_unavailable');
    return channel.id;
  }

  send(channelId, message) {
    return this.request('POST', `/channels/${channelId}/messages`, message);
  }

  respond(token, content) {
    return this.request('PATCH', `/webhooks/${this.config.applicationId}/${encodeURIComponent(token)}/messages/@original`,
      { content, allowed_mentions: OMIT_MENTIONS }, false);
  }

  register() {
    const command = {
      name: 'hailwire',
      description: 'Connect Hailstone tell forwarding to Discord',
      options: [
        {
          type: 1,
          name: 'link',
          description: 'Allow Hailwire to DM you forwarded tells',
          options: [{ type: 3, name: 'code', description: 'The link code shown in Hailstone', required: true }],
        },
        { type: 1, name: 'unlink', description: 'Disconnect all Hailwire clients from your Discord account' },
      ],
    };
    return this.request('POST', `/applications/${this.config.applicationId}/guilds/${this.config.guildId}/commands`, command);
  }

  enqueue(userId, work, stillNeeded) {
    const count = this.queuedPerUser.get(userId) ?? 0;
    if (this.queue.length >= 128 || count >= 8) return null;
    const result = new Promise((resolve, reject) => {
      this.queue.push({ userId, work, stillNeeded, resolve, reject, deadline: Date.now() + 25_000 });
      this.queuedPerUser.set(userId, count + 1);
    });
    if (!this.running) void this.drain();
    return result;
  }

  async drain() {
    this.running = true;
    while (this.queue.length) {
      const job = this.queue.shift();
      try {
        job.resolve(await this.run(job));
      } catch (error) {
        job.reject(error);
      } finally {
        const remaining = this.queuedPerUser.get(job.userId) - 1;
        if (remaining) this.queuedPerUser.set(job.userId, remaining);
        else this.queuedPerUser.delete(job.userId);
      }
      if (this.queue.length) await sleep(110);
    }
    this.running = false;
  }

  async run(job) {
    const attempt = () => {
      if (!job.stillNeeded()) throw new DiscordError('link_revoked');
      if (job.deadline <= Date.now()) throw new DiscordError('delivery_expired');
      return job.work();
    };
    try {
      return await attempt();
    } catch (error) {
      if (error.code !== 'discord_rate_limited' || error.retryAfter > 5) throw error;
      await sleep(error.retryAfter * 1000 + 100);
      return attempt();
    }
  }
}
