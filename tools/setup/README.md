# Muse Charm USB setup

Standalone local Web Serial page for `waveshare-s3-rlcd42`, firmware v0.8.1+.
Open `index.html` in desktop Chrome/Edge, or serve this directory on localhost.
No external scripts, fetch requests, analytics, browser storage or token readback.

The user must first flash the credential-free release firmware. This page does
not flash devices. Close other serial clients before connecting.

## USB protocol (115200 baud)

Each command is UTF-8, prefixed by `>` and terminated by a newline. One request
is active at a time. Replies are single-line JSON with the following prefixes:

| Command | Reply |
| --- | --- |
| `setup.status` | `@setup {"ok":true,"board":"waveshare-s3-rlcd42","protocol":2,"sdk_configured":false,"tts_configured":false}` |
| `sdk.setup={"token":"YOUR_MGST_TOKEN"}` | `@setup {"ok":true,"restart_required":true}` |
| `tts.setup={"provider":"minimax-cn","model":"speech-2.8-turbo","voice":"male-qn-qingse","key":"YOUR_API_KEY"}` | `@tts {"configured":true}` |
| `setup.restart` | `@setup {"ok":true,"restarting":true}` |

SDK input must start with `mgst_`, contain 16–63 ASCII letters/digits/underscores/
hyphens, and save successfully to NVS. Invalid inputs return `ok:false` without
changing the current token. The SDK cache is loaded once at boot so a save does
not alter credentials while pairing/network tasks hold its pointer. NVS takes
precedence over the optional compile-time token. TTS is configured separately.

Saving validates format and local persistence, not Muse/TTS service permissions.
If SDK save succeeds but TTS setup fails, the page explains the partial result
and allows an explicit restart. Inputs are cleared after submission. There is
no automatic erase, account reset or Wi-Fi provisioning through this page.

NVS is unencrypted in the current board configuration. Pairing reset preserves
device-level SDK/TTS keys. Do not redistribute Flash/NVS dumps.

Reference: [Chrome Web Serial](https://developer.chrome.com/docs/capabilities/serial).

## Providers

`qwen` uses the existing Ali Token Plan endpoint. `minimax-cn` uses
`https://api.minimax.cn/v1/t2a_v2`; `minimax-global` uses
`https://api.minimax.io/v1/t2a_v2`. MiniMax defaults to `speech-2.8-turbo` and
`male-qn-qingse`; the page allows supported models and a custom Voice ID.
Native HTTP requests use `stream:false`, `output_format:url`, 16kHz mono MP3.
The MP3 download never receives the API Authorization header. Service error
codes, missing/invalid URLs, oversized replies and HTTP errors fail the job.

Keys support up to 1535 printable non-whitespace ASCII characters, including
long MiniMax tokens. A single NVS `tts_config` JSON stores key and provider
options atomically. Older `tts_key` settings load as Qwen for compatibility.
The page detects protocol 2 and requires the matching public firmware.

[MiniMax native API](https://platform.minimax.io/docs/api-reference/speech-t2a-http).
MiniMax payload/response contract tests pass; actual paid synthesis has not been
verified without an authorized MiniMax key.
