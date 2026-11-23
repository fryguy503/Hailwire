import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { createServer } from 'node:http';

const [executable] = process.argv.slice(2);
assert.ok(executable, 'Pass the transport test executable');

let redirectedRequests = 0;
const fixture = createServer(async (request, response) => {
    const chunks = [];
    for await (const chunk of request) chunks.push(chunk);
    if (request.url === '/v1/inspect') {
        const valid = request.method === 'POST' && request.headers['content-type'] === 'application/json' &&
            request.headers.authorization === 'Bearer fixture-device_token' &&
            Buffer.concat(chunks).toString() === '{"tell":"Hello from EverQuest!"}';
        response.writeHead(valid ? 200 : 400, { 'Content-Type': 'application/json' });
        return response.end(JSON.stringify({ received: valid }));
    }
    if (request.url === '/v1/redirect') {
        response.writeHead(307, { Location: '/v1/redirect-target' });
        return response.end();
    }
    if (request.url === '/v1/redirect-target') ++redirectedRequests;
    if (request.url === '/v1/throttle') {
        response.writeHead(429, { 'Retry-After': '17' });
        return response.end();
    }
    if (request.url === '/v1/excessive') return response.end('x'.repeat(65537));
    response.writeHead(404);
    response.end();
});

fixture.listen(0, '127.0.0.1');
await once(fixture, 'listening');
try {
    const args = [`http://127.0.0.1:${fixture.address().port}/`];
    const child = spawn(executable, args, { stdio: 'inherit', timeout: 20000 });
    const [exitCode] = await once(child, 'exit');
    assert.equal(exitCode, 0);
    assert.equal(redirectedRequests, 0);
} finally {
    fixture.closeAllConnections();
    await new Promise((resolve) => fixture.close(resolve));
}
