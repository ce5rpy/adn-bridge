# Changelog

All notable changes to **ysf2dmrcon** are documented here.

<!-- version list -->

## v0.3.0 (2026-07-18)

### Bug Fixes

- Add EchoLink PCM gain for EL to DMR/YSF ([#10](https://github.com/ce5rpy/ysf2dmrcon/pull/10),
  [`eddf134`](https://github.com/ce5rpy/ysf2dmrcon/commit/eddf1341f1a36c9f55e212394d0ea0f2394beba9))

- Align EchoLink-YSF callsign path with DMR-YSF ([#7](https://github.com/ce5rpy/ysf2dmrcon/pull/7),
  [`a32c44b`](https://github.com/ce5rpy/ysf2dmrcon/commit/a32c44bcf4289bdf187ac77a9df908ef1578d90a))

- Clear sticky EchoLink talker and re-HEADER on late SDES
  ([#9](https://github.com/ce5rpy/ysf2dmrcon/pull/9),
  [`7b97f3c`](https://github.com/ce5rpy/ysf2dmrcon/commit/7b97f3cb8090b07a1e02e60aaa0e7fa8995bb0e0))

- Keep full EchoLink callsign on YSF wire ([#7](https://github.com/ce5rpy/ysf2dmrcon/pull/7),
  [`a32c44b`](https://github.com/ce5rpy/ysf2dmrcon/commit/a32c44bcf4289bdf187ac77a9df908ef1578d90a))

- Link EchoLink conferences and document setup guides
  ([#11](https://github.com/ce5rpy/ysf2dmrcon/pull/11),
  [`15f67d1`](https://github.com/ce5rpy/ysf2dmrcon/commit/15f67d1ab792323e794691d8c084ec07513495e0))

- Parse EchoLink SDES talker only when it looks like a callsign
  ([#9](https://github.com/ce5rpy/ysf2dmrcon/pull/9),
  [`7b97f3c`](https://github.com/ce5rpy/ysf2dmrcon/commit/7b97f3cb8090b07a1e02e60aaa0e7fa8995bb0e0))

- Resolve EchoLink talker to DMR id like YSF-DMR ([#9](https://github.com/ce5rpy/ysf2dmrcon/pull/9),
  [`7b97f3c`](https://github.com/ce5rpy/ysf2dmrcon/commit/7b97f3cb8090b07a1e02e60aaa0e7fa8995bb0e0))

- Stabilize EchoLink-DMR bridge audio and directory path
  ([#7](https://github.com/ce5rpy/ysf2dmrcon/pull/7),
  [`a32c44b`](https://github.com/ce5rpy/ysf2dmrcon/commit/a32c44bcf4289bdf187ac77a9df908ef1578d90a))

- Use inbound EchoLink SDES talker on DMR/YSF ([#9](https://github.com/ce5rpy/ysf2dmrcon/pull/9),
  [`7b97f3c`](https://github.com/ce5rpy/ysf2dmrcon/commit/7b97f3cb8090b07a1e02e60aaa0e7fa8995bb0e0))

- Use only callsign token from EchoLink SDES NAME
  ([#9](https://github.com/ce5rpy/ysf2dmrcon/pull/9),
  [`7b97f3c`](https://github.com/ce5rpy/ysf2dmrcon/commit/7b97f3cb8090b07a1e02e60aaa0e7fa8995bb0e0))

### Documentation

- Add YSF-DMR example INI and refresh default template
  ([`bd7f4fc`](https://github.com/ce5rpy/ysf2dmrcon/commit/bd7f4fc35070a88f2b5631e5d04ac2837e3352ac))

### Features

- Add EchoLink bridge support (DMR and YSF) ([#7](https://github.com/ce5rpy/ysf2dmrcon/pull/7),
  [`a32c44b`](https://github.com/ce5rpy/ysf2dmrcon/commit/a32c44bcf4289bdf187ac77a9df908ef1578d90a))

- Add optional EchoLink Proxy for NAT clients ([#12](https://github.com/ce5rpy/ysf2dmrcon/pull/12),
  [`81bff62`](https://github.com/ce5rpy/ysf2dmrcon/commit/81bff62aa17a5dc01adaf62415e5095da894903b))

- EchoLink <-> DMR bridge with remote AMBE vocoder
  ([#7](https://github.com/ce5rpy/ysf2dmrcon/pull/7),
  [`a32c44b`](https://github.com/ce5rpy/ysf2dmrcon/commit/a32c44bcf4289bdf187ac77a9df908ef1578d90a))


## v0.2.1 (2026-07-17)

### Bug Fixes

- Strip DMR RPTC callsign suffix ([#5](https://github.com/ce5rpy/ysf2dmrcon/pull/5),
  [`e7d5390`](https://github.com/ce5rpy/ysf2dmrcon/commit/e7d53907b5981fd23cba5e45d5bf1fa9a7e4f9d3))


## v0.2.0 (2026-07-16)

### Bug Fixes

- Alias download reload, blake2b verify, and runtime refresh
  ([#3](https://github.com/ce5rpy/ysf2dmrcon/pull/3),
  [`488ae1e`](https://github.com/ce5rpy/ysf2dmrcon/commit/488ae1e7937ae22b3a807e57eec7f270deba6e49))

- Harden shared alias download with lock, retries, and jitter
  ([#3](https://github.com/ce5rpy/ysf2dmrcon/pull/3),
  [`488ae1e`](https://github.com/ce5rpy/ysf2dmrcon/commit/488ae1e7937ae22b3a807e57eec7f270deba6e49))

### Documentation

- Drop Releases section from READMEs
  ([`1e49546`](https://github.com/ce5rpy/ysf2dmrcon/commit/1e495465466f7d9c7c87e6b5b6bce4958d20cc51))

### Features

- Alias download reload, blake2b verify, and multi-instance coordination
  ([#3](https://github.com/ce5rpy/ysf2dmrcon/pull/3),
  [`488ae1e`](https://github.com/ce5rpy/ysf2dmrcon/commit/488ae1e7937ae22b3a807e57eec7f270deba6e49))

### Performance Improvements

- Open-addressing alias tables to cut RAM
  ([`9bcc8d7`](https://github.com/ce5rpy/ysf2dmrcon/commit/9bcc8d7feae2baa4d136ef067d43a725311ac780))


## v0.1.0 (2026-07-16)

- Initial Release
