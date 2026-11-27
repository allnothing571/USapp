#!/usr/bin/env node
// =====================================================================
// deploy/scripts/gen-secrets.mjs
// Zero-dependency secret generator for USapp production `.env`.
// Usage: node deploy/scripts/gen-secrets.mjs ["YourStrongAdminPassword"]
// =====================================================================

import crypto from 'node:crypto';

function randomToken(bytes = 36) {
  return crypto.randomBytes(bytes).toString('base64url');
}

function randomPassword(length = 24) {
  const alphabet = 'ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789!@#$%^&*_-';
  const out = [];
  const raw = crypto.randomBytes(length * 2);
  for (let i = 0; i < raw.length && out.length < length; i += 1) {
    out.push(alphabet[raw[i] % alphabet.length]);
  }
  return out.join('');
}

function hashAdminPassword(password) {
  const salt = crypto.randomBytes(16);
  const derived = crypto.pbkdf2Sync(password, salt, 600000, 32, 'sha256');
  return `pbkdf2-sha256$600000$${salt.toString('base64')}$${derived.toString('base64')}`;
}

function generateVapidKeys() {
  const ecdh = crypto.createECDH('prime256v1');
  ecdh.generateKeys();
  const pubRaw = ecdh.getPublicKey(null, 'uncompressed');
  const privRaw = ecdh.getPrivateKey();
  const privPadded = privRaw.length >= 32
    ? privRaw.subarray(privRaw.length - 32)
    : Buffer.concat([Buffer.alloc(32 - privRaw.length), privRaw]);

  // Self-verify against push-worker loadConfig invariants (65-byte 0x04 P-256 point & 32-byte private key)
  if (pubRaw.length !== 65 || pubRaw[0] !== 0x04) {
    throw new Error('Generated VAPID public key is not an uncompressed P-256 point');
  }
  crypto.ECDH.convertKey(pubRaw, 'prime256v1');
  if (privPadded.length !== 32) {
    throw new Error('Generated VAPID private key is not 32 bytes');
  }

  return {
    publicKey: pubRaw.toString('base64url'),
    privateKey: privPadded.toString('base64url'),
  };
}

const suppliedAdminPass = process.argv[2]?.trim() ?? '';
const generatedAdminPass = suppliedAdminPass === '' ? randomPassword(20) : suppliedAdminPass;
if (generatedAdminPass.length < 12) {
  console.error('Error: admin password must be at least 12 characters.');
  process.exit(1);
}

const adminHash = hashAdminPassword(generatedAdminPass);
const vapid = generateVapidKeys();

console.log('# Paste these generated secrets into your VPS `.env`:');
console.log(`POSTGRES_PASSWORD=${randomToken(24)}`);
console.log(`POSTGRES_SUPERUSER_PASSWORD=${randomToken(24)}`);
console.log(`POSTGRES_PUSH_PASSWORD=${randomToken(24)}`);
console.log(`SESSION_SECRET=${randomToken(36)}`);
console.log(`PUSH_WORKER_TOKEN=${randomToken(36)}`);
console.log(`VAPID_PUBLIC_KEY=${vapid.publicKey}`);
console.log(`VAPID_PRIVATE_KEY=${vapid.privateKey}`);
console.log(`ADMIN_BOOTSTRAP_PASSWORD_HASH=${adminHash}`);
console.log(`BACKUP_PASSPHRASE=${randomToken(27)}`);
if (suppliedAdminPass === '') {
  console.error(`# Generated initial admin password (save safely; NOT written to .env): ${generatedAdminPass}`);
}
