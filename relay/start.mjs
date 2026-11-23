import { readConfig } from './config.mjs';
import { createRelay } from './server.mjs';

try {
  const config = readConfig();
  const server = createRelay(config);
  server.on('error', (error) => {
    console.error(`Hailwire could not start listening (${error.code ?? 'unknown error'}).`);
    process.exitCode = 1;
  });
  server.listen(config.port, config.bind, () => console.log(`Hailwire relay listening on ${config.bind}:${config.port}`));
  for (const signal of ['SIGINT', 'SIGTERM']) process.once(signal, () => server.close());
} catch (error) {
  console.error(error.message);
  process.exitCode = 1;
}
