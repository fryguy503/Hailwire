import { randomUUID } from 'node:crypto';
import { mkdirSync, readFileSync, renameSync, rmSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { isSnowflake } from './config.mjs';

const MAX_DEVICES = 5000;
const MAX_PER_USER = 16;

export class DeviceStore {
  constructor(directory) {
    mkdirSync(directory, { recursive: true, mode: 0o700 });
    this.path = join(directory, 'devices.json');
    let saved;
    try {
      saved = JSON.parse(readFileSync(this.path, 'utf8'));
    } catch (error) {
      if (error.code !== 'ENOENT') throw new Error('Device storage could not be read. Check the Hailwire data directory.');
      this.devices = new Map();
      return;
    }
    if (saved?.version !== 1 || !Array.isArray(saved.devices) || saved.devices.length > MAX_DEVICES ||
        saved.devices.some((device) => !/^[a-f0-9]{64}$/.test(device?.hash ?? '') ||
          !isSnowflake(device.userId) || !isSnowflake(device.channelId))) {
      throw new Error('Unsupported or invalid Hailwire device storage.');
    }
    this.devices = new Map(saved.devices.map((device) => [device.hash, device]));
  }

  get(hash) { return this.devices.get(hash); }

  hasRoom(userId) {
    if (this.devices.size >= MAX_DEVICES) return false;
    let count = 0;
    for (const device of this.devices.values()) {
      if (device.userId === userId && ++count >= MAX_PER_USER) return false;
    }
    return true;
  }

  add(device) {
    if (!this.hasRoom(device.userId)) return false;
    const next = new Map(this.devices).set(device.hash, device);
    this.save(next);
    return true;
  }

  remove(hash) {
    const next = new Map(this.devices);
    if (next.delete(hash)) this.save(next);
  }

  removeUser(userId) {
    const next = new Map(this.devices);
    for (const [hash, device] of next) {
      if (device.userId === userId) next.delete(hash);
    }
    const removed = this.devices.size - next.size;
    if (removed) this.save(next);
    return removed;
  }

  save(next) {
    const temporary = `${this.path}.${randomUUID()}.tmp`;
    try {
      const data = JSON.stringify({ version: 1, devices: [...next.values()] });
      writeFileSync(temporary, data + '\n', { mode: 0o600, flag: 'wx', flush: true });
      renameSync(temporary, this.path);
    } finally {
      rmSync(temporary, { force: true });
    }
    this.devices = next;
  }
}
