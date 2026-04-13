# mod_klear

A FreeSWITCH module for high-quality, low-latency voice cleanup: acoustic echo cancellation (WebRTC AEC3) and neural noise suppression (DeepFilterNet). Designed for phone audio destined for AI agents and for human-to-human calls.

**Status: under active development. Not yet ready for production.**

## Goals

- Serve AI-agent builders who need clean phone audio before ASR / LLM inference.
- Drop-in for any FreeSWITCH deployment: enable per channel via dialplan variable or app, hot-toggle mid-call from ESL.
- Measurable quality — everything we ship is backed by reproducible offline benchmarks against the Microsoft AEC Challenge corpus.
- Target budget: ≤30 ms added algorithmic delay, AEC + NS combined.

## Status

See `test/reports/` for current benchmark numbers.

## Build

Instructions will appear here once the first build-and-run path is green.

## License

Apache-2.0. See `LICENSE`.
