import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { generateKeyPairSync, sign } from 'node:crypto';
import { once } from 'node:events';
import { mkdirSync, mkdtempSync, realpathSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { createInterface } from 'node:readline';
import { createRelay } from '../relay/server.mjs';

const [executable] = process.argv.slice(2);
assert.ok(executable, 'Pass the Hailwire add-on fixture executable');

const temporary = realpathSync(tmpdir());
const directory = realpathSync(mkdtempSync(join(temporary, 'hailwire-addon-test-')));
assert.equal(dirname(directory), temporary);
const profile = join(directory, 'player-data');
mkdirSync(profile);

const { publicKey, privateKey } = generateKeyPairSync('ed25519');
const userId = '111111111111111111';
const applicationId = '222222222222222222';
const guildId = '333333333333333333';
const channelId = '444444444444444444';
const botToken = 'fixture_discord_bot_token';
const messages = [];

const relay = createRelay({
  applicationId, guildId, botToken, dataDir: join(directory, 'relay-data'),
  publicKey: publicKey.export({ format: 'der', type: 'spki' }).subarray(-32).toString('hex'),
  guildUrl: 'https://discord.gg/hailwire-fixture',
}, {
  fetcher: async (url, options) => {
    const path = new URL(url).pathname;
    const payload = JSON.parse(options.body);
    if (path === '/api/v10/users/@me/channels') {
      assert.equal(payload.recipient_id, userId);
      assert.equal(options.headers.Authorization, `Bot ${botToken}`);
      return Response.json({ id: channelId });
    }
    if (path === `/api/v10/channels/${channelId}/messages`) {
      assert.equal(options.headers.Authorization, `Bot ${botToken}`);
      messages.push(payload);
      return Response.json({ id: '555555555555555555' });
    }
    if (path.endsWith('/messages/@original')) {
      assert.match(payload.content, /Test DM sent/);
      return Response.json({});
    }
    assert.fail(`Unexpected Discord API path ${path}.`);
  },
});

let child;
try {
  relay.listen(0, '127.0.0.1');
  await once(relay, 'listening');
  const origin = `http://127.0.0.1:${relay.address().port}`;
  child = spawn(executable, [origin, profile], { stdio: ['ignore', 'pipe', 'inherit'], timeout: 30_000 });
  const completion = once(child, 'close');
  const lines = createInterface({ input: child.stdout });
  const readCode = async () => {
    for await (const line of lines) {
      const code = /^(?:LINK_CODE=)?([A-Z2-9]{8})$/.exec(line.trim())?.[1];
      if (code) return code;
      process.stdout.write(`${line}\n`);
    }
    assert.fail('The host fixture exited without a Hailwire link code.');
  };
  const code = await Promise.race([readCode(), completion.then(() => undefined)]);
  assert.ok(code, 'The host fixture must print a link code');

  const timestamp = String(Math.floor(Date.now() / 1000));
  const body = JSON.stringify({
    type: 2, id: '666666666666666666', token: 'fixture_interaction_token',
    application_id: applicationId, guild_id: guildId,
    member: { user: { id: userId, username: 'Val' } },
    data: { name: 'hailwire', options: [{ type: 1, name: 'link', options: [{ type: 3, name: 'code', value: code }] }] },
  });
  const linking = await fetch(origin + '/discord/interactions', {
    method: 'POST',
    headers: {
      'Content-Type': 'application/json',
      'X-Signature-Timestamp': timestamp,
      'X-Signature-Ed25519': sign(null, Buffer.from(timestamp + body), privateKey).toString('hex'),
    },
    body,
  });
  assert.equal(linking.status, 200);
  assert.deepEqual(await linking.json(), { type: 5, data: { flags: 64 } });
  const [exitCode, signal] = await completion;
  assert.equal(exitCode, 0, `Host fixture exited with ${signal ?? exitCode}`);

  assert.deepEqual(messages.map((message) => message.embeds[0].title), ['Hailwire is linked', 'Incoming tell', 'Hailwire test']);
  const tell = messages[1];
  assert.deepEqual(tell.allowed_mentions, { parse: [] });
  assert.equal(tell.embeds[0].description, 'Ready for the raid?');
  assert.deepEqual(tell.embeds[0].fields.map(({ name, value }) => [name, value]),
    [['From', 'Mira'], ['To', 'Valorith'], ['Server', 'Bastion']]);
  assert.ok(Number.isFinite(Date.parse(tell.embeds[0].timestamp)));
  console.log('Hailwire add-on delivered exactly one player tell through the relay and passed link, test, and unlink.');
} finally {
  if (child && child.exitCode === null && !child.signalCode) child.kill();
  relay.closeAllConnections();
  await new Promise((resolve) => relay.close(resolve));
  rmSync(directory, { recursive: true, force: true });
}
