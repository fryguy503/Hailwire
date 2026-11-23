import { createHash, createPublicKey, randomBytes, verify } from 'node:crypto';
import { createServer } from 'node:http';
import { isIP } from 'node:net';
import { isSnowflake } from './config.mjs';
import { DeviceStore } from './devices.mjs';
import { Discord, tellMessage, testMessage } from './discord.mjs';

const LINK_TTL = 300_000;
const CODE_ALPHABET = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789';
const MAX_BODY = 16 * 1024;
const MAX_LINKS = 2048;
const TOKEN_PATTERN = /^[A-Za-z0-9_-]{43}$/;
const SPKI_PREFIX = '302a300506032b6570032100';
const token = () => randomBytes(32).toString('base64url');
const hash = (value) => createHash('sha256').update(value).digest('hex');

class HttpError extends Error {
  constructor(status, code, retryAfter) {
    super(code);
    this.status = status;
    this.retryAfter = retryAfter;
  }
}

function reply(res, status, data, extraHeaders) {
  res.writeHead(status, {
    'Content-Type': 'application/json; charset=utf-8',
    'Cache-Control': 'no-store',
    'X-Content-Type-Options': 'nosniff',
    ...extraHeaders,
  });
  res.end(JSON.stringify(data));
}

const ephemeral = (res, content) => reply(res, 200, { type: 4, data: { content, flags: 64 } });

async function readBody(req) {
  if (req.headers['content-type']?.split(';', 1)[0].trim().toLowerCase() !== 'application/json') {
    throw new HttpError(415, 'json_required');
  }
  if (Number(req.headers['content-length']) > MAX_BODY) throw new HttpError(413, 'payload_too_large');
  const chunks = [];
  let size = 0;
  for await (const chunk of req.iterator({ destroyOnReturn: false })) {
    size += chunk.length;
    if (size > MAX_BODY) throw new HttpError(413, 'payload_too_large');
    chunks.push(chunk);
  }
  return Buffer.concat(chunks, size);
}

function parseBody(bytes) {
  try {
    const value = JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(bytes));
    if (value && typeof value === 'object' && !Array.isArray(value)) return value;
  } catch { /* reported uniformly below */ }
  throw new HttpError(400, 'invalid_json');
}

function parseTell(value) {
  if (typeof value.id !== 'string' || !/^[a-zA-Z0-9_-]{1,80}$/.test(value.id)) {
    throw new HttpError(400, 'invalid_tell');
  }
  const tell = { id: value.id };
  for (const [field, limit] of [['server', 128], ['character', 128], ['sender', 128], ['message', 2048]]) {
    if (typeof value[field] !== 'string' || value[field].length > limit) throw new HttpError(400, 'invalid_tell');
    const clean = value[field].toWellFormed().replace(/[\x00-\x08\x0b-\x1f\x7f]/g, '').trim();
    if (!clean) throw new HttpError(400, 'invalid_tell');
    tell[field] = clean;
  }
  if (value.timestamp !== undefined) {
    if (typeof value.timestamp !== 'string' ||
        !/^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d(?:\.\d{1,3})?Z$/.test(value.timestamp) ||
        !Number.isFinite(Date.parse(value.timestamp))) throw new HttpError(400, 'invalid_timestamp');
    tell.timestamp = value.timestamp;
  }
  return tell;
}

function discordError(error) {
  switch (error.code) {
    case 'discord_dm_unavailable': return new HttpError(409, error.code);
    case 'discord_rate_limited': return new HttpError(429, error.code, error.retryAfter);
    case 'link_revoked': return new HttpError(401, 'invalid_device');
    default: return new HttpError(503, 'discord_unavailable', 5);
  }
}

export function createRelay(config, { fetcher = fetch, now = Date.now, logger = console } = {}) {
  const store = new DeviceStore(config.dataDir);
  const discord = new Discord(config, fetcher);
  const publicKey = createPublicKey({ key: Buffer.from(SPKI_PREFIX + config.publicKey, 'hex'), format: 'der', type: 'spki' });
  const links = new Map();
  const byCode = new Map();
  const interactions = new Map();
  const counters = new Map();
  const accepted = new Map();
  const dmUnavailable = new Map();
  let cleanupAt = 0;

  function forgetLink(key, link) {
    if (byCode.get(link.code) === key) byCode.delete(link.code);
    links.delete(key);
  }

  function cleanup(time) {
    if (time < cleanupAt) return;
    cleanupAt = time + 15_000;
    for (const [key, link] of links) if (time >= link.expiresAt) forgetLink(key, link);
    for (const entries of [interactions, counters, accepted]) {
      for (const [key, item] of entries) if (time >= item.expiresAt) entries.delete(key);
    }
    for (const [key, past] of dmUnavailable) if (time >= past + 600_000) dmUnavailable.delete(key);
  }

  function allow(key, limit, time) {
    let item = counters.get(key);
    if (!item || item.expiresAt <= time) {
      if (counters.size >= 8192) throw new HttpError(503, 'relay_busy', 15);
      item = { count: 0, expiresAt: time + 60_000 };
      counters.set(key, item);
    }
    if (item.count >= limit) throw new HttpError(429, 'rate_limited', Math.ceil((item.expiresAt - time) / 1000));
    item.count++;
  }

  function clientAddress(req) {
    const proxy = req.headers['x-real-ip'];
    if (config.trustProxy && typeof proxy === 'string' && isIP(proxy)) return proxy;
    return req.socket.remoteAddress ?? 'unknown';
  }

  function startLink(res, time, address) {
    allow(`start:${address}`, 12, time);
    if (links.size >= MAX_LINKS) throw new HttpError(503, 'relay_busy', 15);
    const pollToken = token();
    const key = hash(pollToken);
    let code;
    do {
      code = Array.from(randomBytes(8), (byte) => CODE_ALPHABET[byte % CODE_ALPHABET.length]).join('');
    } while (byCode.has(code));
    links.set(key, { code, state: 'pending', expiresAt: time + LINK_TTL });
    byCode.set(code, key);
    reply(res, 200, { code, poll_token: pollToken, expires_in: LINK_TTL / 1000, guild_url: config.guildUrl });
  }

  function pollLink(res, value, time, address) {
    allow(`poll:${address}`, 300, time);
    if (typeof value.poll_token !== 'string' || !TOKEN_PATTERN.test(value.poll_token)) {
      throw new HttpError(400, 'invalid_poll_token');
    }
    const key = hash(value.poll_token);
    const link = links.get(key);
    if (!link || time >= link.expiresAt) return reply(res, 200, { status: 'expired' });
    if (link.state !== 'linked') return reply(res, 200, { status: 'pending' });
    if (!link.deviceToken) {
      const deviceToken = token();
      if (!store.add({ hash: hash(deviceToken), userId: link.userId, channelId: link.channelId })) {
        forgetLink(key, link);
        throw new HttpError(409, 'device_limit');
      }
      link.deviceToken = deviceToken;
      link.expiresAt = Math.min(link.expiresAt, time + 60_000);
    }
    reply(res, 200, { status: 'linked', device_token: link.deviceToken, discord_label: link.label });
  }

  function verifySignature(req, bytes, time) {
    const timestamp = req.headers['x-signature-timestamp'];
    const signature = req.headers['x-signature-ed25519'];
    if (typeof timestamp !== 'string' || !/^\d{1,12}$/.test(timestamp) || Math.abs(time - Number(timestamp) * 1000) > LINK_TTL ||
        typeof signature !== 'string' || !/^[a-f0-9]{128}$/i.test(signature) ||
        !verify(null, Buffer.concat([Buffer.from(timestamp), bytes]), publicKey, Buffer.from(signature, 'hex'))) {
      throw new HttpError(401, 'invalid_signature');
    }
  }

  async function linkDiscord(value, res, user, input, time) {
    const code = typeof input === 'string' ? input.trim().toUpperCase() : '';
    const key = byCode.get(code);
    const link = links.get(key);
    if (!link || time >= link.expiresAt || link.state !== 'pending') {
      return ephemeral(res, 'That code is invalid, expired, or already in use. Open Hailwire in Hailstone for a fresh code.');
    }
    if (!store.hasRoom(user.id)) {
      return ephemeral(res, 'The Hailwire device limit was reached. Use `/hailwire unlink` to disconnect your existing clients before linking again.');
    }
    const stillNeeded = () => links.get(key) === link && link.expiresAt > now();
    link.state = 'linking';
    link.userId = user.id;
    const sending = discord.enqueue(user.id, async () => {
      const channelId = await discord.createDM(user.id);
      if (stillNeeded()) await discord.send(channelId, testMessage(true));
      return channelId;
    }, stillNeeded);
    if (!sending) {
      link.state = 'pending';
      delete link.userId;
      return ephemeral(res, 'Hailwire is busy. Try the same code again in a few seconds.');
    }
    reply(res, 200, { type: 5, data: { flags: 64 } });
    let content;
    try {
      const channelId = await sending;
      if (stillNeeded()) {
        link.channelId = channelId;
        link.label = typeof user.username === 'string' ? user.username.toWellFormed().slice(0, 64) : 'Discord';
        link.state = 'linked';
        byCode.delete(code);
        dmUnavailable.delete(user.id);
        content = 'Test DM sent. Return to Hailstone to finish linking and enable tell forwarding.';
      } else {
        content = 'This Hailwire link expired or was cancelled. Start again in Hailstone.';
      }
    } catch (error) {
      if (stillNeeded()) {
        link.state = 'pending';
        delete link.userId;
      }
      content = error.code === 'discord_dm_unavailable'
        ? 'Discord rejected the test DM. Allow DMs from members of this server, then try the same code again.'
        : 'Hailwire could not send the test DM. Try again in a few seconds.';
    }
    try {
      await discord.respond(value.token, content);
    } catch {
      logger.warn('Could not update a Discord interaction response.');
    }
  }

  function discordInteraction(req, res, bytes, time) {
    verifySignature(req, bytes, time);
    const value = parseBody(bytes);
    if (value.type === 1) return reply(res, 200, { type: 1 });
    const user = value.member?.user;
    if (value.type !== 2 || value.application_id !== config.applicationId || value.guild_id !== config.guildId ||
        value.data?.name !== 'hailwire' || !isSnowflake(user?.id) || !isSnowflake(value.id) ||
        typeof value.token !== 'string' || !/^[A-Za-z0-9._~+/=-]{1,512}$/.test(value.token)) {
      return ephemeral(res, 'Run this command in the Discord server where Hailwire is installed.');
    }
    if (interactions.has(value.id)) return ephemeral(res, 'Hailwire already received this command.');
    if (interactions.size >= 8192) return ephemeral(res, 'Hailwire is busy. Try again shortly.');
    interactions.set(value.id, { expiresAt: time + LINK_TTL });
    const command = value.data.options?.[0];
    if (command?.name === 'unlink') {
      const removed = store.removeUser(user.id);
      for (const [key, link] of links) if (link.userId === user.id) forgetLink(key, link);
      dmUnavailable.delete(user.id);
      return ephemeral(res, removed ? 'All of your Hailwire clients are disconnected.' : 'No Hailwire clients are linked to this account.');
    }
    if (command?.name === 'link') {
      try { allow(`link:${user.id}`, 6, time); } catch {
        return ephemeral(res, 'Too many link attempts. Wait a minute and try again.');
      }
      return linkDiscord(value, res, user, command.options?.find((option) => option.name === 'code')?.value, time);
    }
    return ephemeral(res, 'Use `/hailwire link` or `/hailwire unlink`.');
  }

  function getDevice(req) {
    const supplied = req.headers.authorization?.match(/^Bearer ([A-Za-z0-9_-]{43})$/i)?.[1];
    const device = supplied && store.get(hash(supplied));
    if (!device) throw new HttpError(401, 'invalid_device');
    return device;
  }

  function forgetDevice(device) {
    store.remove(device.hash);
    for (const [key, link] of links) {
      if (link.deviceToken && hash(link.deviceToken) === device.hash) forgetLink(key, link);
    }
  }

  function sendTest(res, device, time) {
    allow(`test:${device.userId}`, 5, time);
    const sending = discord.enqueue(device.userId, () => discord.send(device.channelId, testMessage(false)),
      () => store.get(device.hash) === device);
    if (!sending) throw new HttpError(503, 'relay_busy', 2);
    return sending.then(() => {
      dmUnavailable.delete(device.userId);
      reply(res, 200, { status: 'sent' });
    }, (error) => {
      if (error.code === 'discord_dm_unavailable') dmUnavailable.set(device.userId, now());
      throw discordError(error);
    });
  }

  function sendTell(res, device, time, body) {
    const tell = parseTell(body);
    const key = hash(`${device.hash}:${tell.id}`);
    if ((accepted.get(key)?.expiresAt ?? 0) > time) return reply(res, 200, { status: 'duplicate' });
    if (dmUnavailable.has(device.userId)) throw new HttpError(409, 'discord_dm_unavailable');
    allow(`tell:${device.userId}`, 60, time);
    if (accepted.size >= 10_000) throw new HttpError(503, 'relay_busy', 15);
    const status = { expiresAt: time + LINK_TTL };
    const nonce = Buffer.from(key, 'hex').toString('base64url').slice(0, 25);
    const sending = discord.enqueue(device.userId, () => discord.send(device.channelId, tellMessage(tell, time, nonce)),
      () => store.get(device.hash) === device && !dmUnavailable.has(device.userId));
    if (!sending) throw new HttpError(503, 'relay_busy', 2);
    accepted.set(key, status);
    void sending.catch((error) => {
      if (accepted.get(key) === status) accepted.delete(key);
      if (error.code === 'discord_dm_unavailable') dmUnavailable.set(device.userId, now());
      if (!['link_revoked', 'discord_dm_unavailable'].includes(error.code)) logger.warn('Discord tell delivery failed.');
    });
    reply(res, 202, { status: 'queued' });
  }

  const apiPaths = new Set(['/v1/link/start', '/v1/link/poll', '/v1/tells', '/v1/test', '/v1/unlink']);
  async function route(req, res) {
    if (req.method === 'GET' && req.url === '/healthz') return reply(res, 200, { status: 'ok' });
    if (req.method !== 'POST' || (!apiPaths.has(req.url) && req.url !== '/discord/interactions')) {
      throw new HttpError(404, 'not_found');
    }
    const bytes = await readBody(req);
    const time = now();
    cleanup(time);
    if (req.url === '/discord/interactions') return discordInteraction(req, res, bytes, time);
    const body = parseBody(bytes);
    const address = clientAddress(req);
    if (req.url === '/v1/link/start') return startLink(res, time, address);
    if (req.url === '/v1/link/poll') return pollLink(res, body, time, address);
    const device = getDevice(req);
    if (req.url === '/v1/unlink') {
      forgetDevice(device);
      return reply(res, 200, { status: 'unlinked' });
    }
    if (req.url === '/v1/test') return sendTest(res, device, time);
    return sendTell(res, device, time, body);
  }

  const server = createServer({ requestTimeout: 15_000, headersTimeout: 8_000, connectionsCheckingInterval: 2_000 },
    (req, res) => {
      void route(req, res).catch((error) => {
        if (!(error instanceof HttpError)) logger.error('Hailwire relay request failed. Check storage and service permissions.');
        if (res.headersSent || res.destroyed) return;
        const status = error instanceof HttpError ? error.status : 500;
        const headers = {};
        if (error.retryAfter) headers['Retry-After'] = String(error.retryAfter);
        if (status === 413) headers.Connection = 'close';
        reply(res, status, { error: error instanceof HttpError ? error.message : 'relay_error' }, headers);
      });
    });
  server.maxConnections = 256;
  return server;
}
