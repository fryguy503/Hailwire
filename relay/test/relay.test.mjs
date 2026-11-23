import assert from 'node:assert/strict';
import { generateKeyPairSync, sign } from 'node:crypto';
import { once } from 'node:events';
import { mkdtempSync, readFileSync, realpathSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { setTimeout as sleep } from 'node:timers/promises';
import test from 'node:test';
import { readConfig } from '../config.mjs';
import { Discord } from '../discord.mjs';
import { createRelay } from '../server.mjs';

const userId = '111111111111111111';
const appId = '222222222222222222';
const guildId = '333333333333333333';
const channelId = '444444444444444444';

function json(data, status = 200, headers) {
  return new Response(JSON.stringify(data), { status, headers });
}

async function until(fn) {
  for (let attempt = 0; attempt < 100; attempt++) {
    const value = fn();
    if (value) return value;
    await sleep(10);
  }
  assert.fail('Expected Discord call did not arrive.');
}

async function fixture(t) {
  const { publicKey, privateKey } = generateKeyPairSync('ed25519');
  const temporary = realpathSync(tmpdir());
  const directory = realpathSync(mkdtempSync(join(temporary, 'hailwire-test-')));
  assert.equal(dirname(directory), temporary);
  const config = readConfig({
    DISCORD_PUBLIC_KEY: publicKey.export({ format: 'der', type: 'spki' }).subarray(-32).toString('hex'),
    DISCORD_APPLICATION_ID: appId,
    DISCORD_GUILD_ID: guildId,
    DISCORD_BOT_TOKEN: 'bot_TestFakeToken_1234567890',
    HAILWIRE_DATA_DIR: directory,
    HAILWIRE_GUILD_URL: 'https://discord.gg/hailwire-test',
  });
  const outgoing = [];
  let timestamp = Date.now();
  let allowDM = true;
  let id = 555555555555555555n;
  const fetcher = async (url, options) => {
    const pathname = new URL(url).pathname;
    outgoing.push({ pathname, headers: options.headers, payload: JSON.parse(options.body) });
    if (pathname.endsWith('/users/@me/channels')) return json({ id: channelId });
    if (pathname.endsWith(`/channels/${channelId}/messages`)) {
      return allowDM ? json({ id: '666666666666666666' }) : json({ code: 50007 }, 403);
    }
    if (pathname.endsWith('/messages/@original')) return json({});
    assert.fail(`Unexpected Discord API path ${pathname}.`);
  };
  let server;
  let base;

  async function restart() {
    if (server) await new Promise((resolve) => server.close(resolve));
    server = createRelay(config, { fetcher, now: () => timestamp });
    server.listen(0, '127.0.0.1');
    await once(server, 'listening');
    base = `http://127.0.0.1:${server.address().port}`;
  }

  t.after(async () => {
    if (server) await new Promise((resolve) => server.close(resolve));
    rmSync(directory, { recursive: true, force: true });
  });
  await restart();

  async function post(path, body = {}, bearer) {
    const headers = { 'Content-Type': 'application/json' };
    if (bearer) headers.Authorization = `Bearer ${bearer}`;
    const response = await fetch(base + path, { method: 'POST', headers, body: JSON.stringify(body) });
    return { status: response.status, body: await response.json(), retryAfter: response.headers.get('Retry-After') };
  }

  async function interact(body, valid = true, seconds = Math.floor(timestamp / 1000)) {
    const raw = JSON.stringify(body);
    const time = String(seconds);
    const bytes = Buffer.from(time + raw + (valid ? '' : 'tampered'));
    const response = await fetch(base + '/discord/interactions', {
      method: 'POST',
      headers: {
        'Content-Type': 'application/json',
        'X-Signature-Timestamp': time,
        'X-Signature-Ed25519': sign(null, bytes, privateKey).toString('hex'),
      },
      body: raw,
    });
    return { status: response.status, body: await response.json() };
  }

  const command = (name, code) => ({
    type: 2,
    id: String(id++),
    token: 'signed_interaction_token+/=',
    application_id: appId,
    guild_id: guildId,
    member: { user: { id: userId, username: 'Val' } },
    data: { name: 'hailwire', options: [{ type: 1, name, options: [{ type: 3, name: 'code', value: code }] }] },
  });
  const discordMessages = () => outgoing.filter(({ pathname }) => pathname.endsWith(`/channels/${channelId}/messages`));
  const responses = () => outgoing.filter(({ pathname }) => pathname.endsWith('/messages/@original'));
  const setDms = (allowed) => { allowDM = allowed; };
  const advance = (milliseconds) => { timestamp += milliseconds; };

  return { advance, command, config, directory, discordMessages, interact, outgoing, post, responses, restart, setDms };
}

test('link, deliver tell embeds, deduplicate, test after restart, and unlink', async (t) => {
  const f = await fixture(t);
  const start = await f.post('/v1/link/start');
  assert.equal(start.status, 200);
  const { code, poll_token, expires_in, guild_url } = start.body;
  assert.match(code, /^[A-Z2-9]{8}$/);
  assert.equal(expires_in, 300);
  assert.equal(guild_url, 'https://discord.gg/hailwire-test');
  assert.equal((await f.post('/v1/link/poll', { poll_token })).body.status, 'pending');

  const command = f.command('link', code.toLowerCase());
  assert.deepEqual((await f.interact(command)).body, { type: 5, data: { flags: 64 } });
  await until(() => f.responses().length);
  const linked = await f.post('/v1/link/poll', { poll_token });
  assert.equal(linked.body.status, 'linked');
  assert.equal(linked.body.discord_label, 'Val');
  const device = linked.body.device_token;
  assert.match(device, /^[a-zA-Z0-9_-]{43}$/);
  assert.equal((await f.post('/v1/link/poll', { poll_token })).body.device_token, device);
  assert.match((await f.interact(command)).body.data.content, /already received/);
  assert.match((await f.interact(f.command('link', code))).body.data.content, /invalid, expired, or already/);

  const tell = {
    id: 'tell-1', server: 'Bastion', character: 'Valorith', sender: 'Mira',
    message: '**Ready?** <@111111111111111111>\nMeet at the lift.', timestamp: '2026-11-22T22:30:00.000Z',
  };
  assert.equal((await f.post('/v1/tells', tell)).status, 401);
  assert.equal((await f.post('/v1/tells', { ...tell, timestamp: 'invalid' }, device)).status, 400);
  assert.equal((await f.post('/v1/tells', tell, device)).status, 202);
  assert.equal((await f.post('/v1/tells', tell, device)).body.status, 'duplicate');
  await until(() => f.discordMessages().length === 2);
  const dm = f.discordMessages().at(-1);
  assert.equal(dm.headers.Authorization, `Bot ${f.config.botToken}`);
  assert.ok(!f.responses()[0].headers.Authorization);
  assert.deepEqual(dm.payload.allowed_mentions, { parse: [] });
  assert.equal(dm.payload.enforce_nonce, true);
  const embed = dm.payload.embeds[0];
  assert.equal(embed.description, '\\*\\*Ready?\\*\\* \\<@111111111111111111\\>\nMeet at the lift.');
  assert.equal(embed.timestamp, tell.timestamp);
  assert.deepEqual(embed.fields.map((field) => field.value), ['Mira', 'Valorith', 'Bastion']);
  const saved = readFileSync(join(f.directory, 'devices.json'), 'utf8');
  assert.ok(!saved.includes(device) && !saved.includes(poll_token) && !saved.includes('Meet at the lift.'));
  assert.match(JSON.parse(saved).devices[0].hash, /^[0-9a-f]{64}$/);

  await f.restart();
  assert.equal((await f.post('/v1/test', {}, device)).body.status, 'sent');
  assert.equal((await f.post('/v1/unlink', {}, device)).body.status, 'unlinked');
  assert.equal((await f.post('/v1/tells', tell, device)).status, 401);
  await f.restart();
  assert.equal((await f.post('/v1/test', {}, device)).status, 401);
});

test('verify Discord signatures, pairing expiry, and scope signed commands to the configured server', async (t) => {
  const f = await fixture(t);
  assert.deepEqual(await f.interact({ type: 1 }), { status: 200, body: { type: 1 } });
  assert.equal((await f.interact({ type: 1 }, false)).status, 401);
  assert.equal((await f.interact({ type: 1 }, true, 1)).status, 401);
  const start = (await f.post('/v1/link/start')).body;
  const wrongGuild = { ...f.command('link', start.code), guild_id: userId };
  assert.match((await f.interact(wrongGuild)).body.data.content, /where Hailwire is installed/);
  f.advance(301_000);
  assert.equal((await f.post('/v1/link/poll', { poll_token: start.poll_token })).body.status, 'expired');
  assert.match((await f.interact(f.command('link', start.code))).body.data.content, /invalid, expired, or already/);
  assert.equal(f.outgoing.length, 0);
});

test('DM privacy failures can be corrected and Discord unlink revokes every device', async (t) => {
  const f = await fixture(t);
  const start = (await f.post('/v1/link/start')).body;
  f.setDms(false);
  await f.interact(f.command('link', start.code));
  await until(() => f.responses().length === 1);
  assert.match(f.responses()[0].payload.content, /Discord rejected the test DM/);
  assert.equal((await f.post('/v1/link/poll', { poll_token: start.poll_token })).body.status, 'pending');
  f.setDms(true);
  await f.interact(f.command('link', start.code));
  await until(() => f.responses().length === 2);
  const device1 = (await f.post('/v1/link/poll', { poll_token: start.poll_token })).body.device_token;
  const second = (await f.post('/v1/link/start')).body;
  await f.interact(f.command('link', second.code));
  await until(() => f.responses().length === 3);
  const device2 = (await f.post('/v1/link/poll', { poll_token: second.poll_token })).body.device_token;
  assert.notEqual(device1, device2);
  f.setDms(false);
  const failed = await f.post('/v1/test', {}, device1);
  assert.equal(failed.status, 409);
  assert.equal(failed.body.error, 'discord_dm_unavailable');
  assert.match((await f.interact(f.command('unlink'))).body.data.content, /All of your Hailwire clients are disconnected/);
  assert.equal((await f.post('/v1/link/poll', { poll_token: second.poll_token })).body.status, 'expired');
  assert.equal((await f.post('/v1/test', {}, device1)).status, 401);
  assert.equal((await f.post('/v1/test', {}, device2)).status, 401);
});

test('the relay throttles unauthenticated link creation', async (t) => {
  const f = await fixture(t);
  for (let count = 0; count < 12; count++) assert.equal((await f.post('/v1/link/start')).status, 200);
  const throttled = await f.post('/v1/link/start');
  assert.equal(throttled.status, 429);
  assert.equal(throttled.retryAfter, '60');
  f.advance(61_000);
  assert.equal((await f.post('/v1/link/start')).status, 200);
});

test('the Discord queue waits for Retry-After and retries once', async () => {
  let attempts = 0;
  const discord = new Discord({ botToken: 'bot' }, async () => {
    if (++attempts === 1) return json({ retry_after: 1 }, 429, { 'Retry-After': '1' });
    return json({ id: channelId });
  });
  assert.deepEqual(await discord.enqueue(userId, () => discord.send(channelId, { content: 'test' }), () => true), { id: channelId });
  assert.equal(attempts, 2);
});

test('guild registration provides private-account link and unlink commands', async () => {
  const discord = new Discord({ botToken: 'test-token', applicationId: appId, guildId }, async (url, options) => {
    assert.equal(url, `https://discord.com/api/v10/applications/${appId}/guilds/${guildId}/commands`);
    assert.equal(options.method, 'POST');
    assert.equal(options.headers.Authorization, 'Bot test-token');
    const command = JSON.parse(options.body);
    assert.equal(command.name, 'hailwire');
    assert.deepEqual(command.options.map(({ name }) => name), ['link', 'unlink']);
    assert.deepEqual(command.options[0].options.map(({ name, type, required }) => ({ name, type, required })),
      [{ name: 'code', type: 3, required: true }]);
    return json({ id: '555555555555555555' });
  });
  assert.equal((await discord.register()).id, '555555555555555555');
});
