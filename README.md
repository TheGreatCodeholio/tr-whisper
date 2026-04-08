<img src="https://raw.githubusercontent.com/TrunkRecorder/trunk-recorder/refs/heads/master/docs/media/trunk-recorder-header.png" width="75%" height="75%">

# Trunk Recorder Whisper Transcribe Plugin <!-- omit from toc -->

This is a plugin for Trunk Recorder that sends completed call audio to a Whisper-compatible transcription API and stores the transcription results in the call JSON for that call.

This plugin is a **blocking** plugin, not a deferred plugin. It should be configured with:

- `"pluginType": "blocking"`

This allows the plugin to run during Trunk Recorder's blocking `call_end()` stage so it can enrich the final call JSON before that call is finished processing.

The plugin supports:

- Blocking post-call transcription
- Top-level plugin enable/disable
- Per-system enable/disable
- Optional language and prompt values
- Optional segment output
- Talkgroup allow/deny filters using simple wildcard patterns
- Automatic use of the compressed `.m4a` file when `compressWav` is enabled and a converted file exists, otherwise the original `.wav` file is used

Requires Trunk Recorder 5.0 or later and a Whisper-compatible HTTP transcription endpoint.

- [Install](#install)
- [Configure](#configure)
- [How It Works](#how-it-works)
- [Talkgroup Filters](#talkgroup-filters)
- [Plugin Output](#plugin-output)
- [Example Config](#example-config)
- [Notes](#notes)

## Install

1. **Clone Trunk Recorder** source following the normal Trunk Recorder build instructions.

2. **Install libcurl development headers** if they are not already installed.

On Debian/Ubuntu:

```bash
sudo apt install libcurl4-openssl-dev
```

3. **Clone this plugin into the `user_plugins` directory** of the Trunk Recorder source tree.

```bash
cd [your trunk-recorder source directory]
cd user_plugins
git clone [your plugin repository URL]
```

4. **Build and install Trunk Recorder and the plugin**.

```bash
cd [your trunk-recorder build directory]
sudo make install
```

Plugins in `user_plugins` are built and installed along with Trunk Recorder.

## Configure

### Plugin options

| Key | Required | Default Value | Type | Description |
| --- | :---: | --- | --- | --- |
| `name` |  | `whisper_transcribe` | string | Friendly plugin name used in logs. |
| `library` | ✓ |  | string | Shared library filename or full path to the plugin, for example `libwhisper_transcribe.so`. |
| `enabled` |  | `true` | bool | Top-level plugin enable flag. If `false`, the plugin is not loaded by the plugin manager. |
| `pluginType` |  | `blocking` | string | Should be set to `blocking` for this plugin so it runs in the blocking `call_end()` stage and can enrich the final call JSON. |
| `server` | ✓ |  | string | Whisper-compatible transcription endpoint URL, for example `http://127.0.0.1:8000/v1/audio/transcriptions`. |
| `apiKey` |  |  | string | Optional bearer token used as `Authorization: Bearer ...` when calling the transcription API. |
| `model` |  | `whisper-1` | string | Model name sent to the transcription API. |
| `timeoutSeconds` |  | `300` | integer | Total request timeout in seconds. |
| `systems` | ✓ |  | array | List of system-specific settings. At least one enabled system must be configured. |

### Per-system options

Each entry in `systems` applies to a Trunk Recorder system identified by `shortName`. The plugin validates that each configured `shortName` exists in the loaded Trunk Recorder systems during initialization.

| Key | Required | Default Value | Type | Description |
| --- | :---: | --- | --- | --- |
| `shortName` | ✓ |  | string | Must match the Trunk Recorder system `shortName`. |
| `enabled` |  | `true` | bool | Enables or disables transcription for this specific system within the plugin. |
| `language` |  |  | string | Optional language hint passed to the transcription API. |
| `prompt` |  |  | string | Optional prompt passed to the transcription API. |
| `includeSegments` |  | `true` | bool | If the API returns `segments`, include them in the plugin output. |
| `talkgroupAllow` |  |  | array | Optional allow list of talkgroup patterns. If set, only matching talkgroups are transcribed. |
| `talkgroupDeny` |  |  | array | Optional deny list of talkgroup patterns. Matching talkgroups are skipped. |

## How It Works

When a call ends, the plugin:

1. Runs only if the top-level plugin `enabled` flag is `true`
2. Runs as a **blocking** plugin during Trunk Recorder's blocking `call_end()` stage
3. Looks up the matching configured system by `shortName`
4. Skips calls for systems that are not enabled
5. Skips encrypted calls
6. Applies optional talkgroup allow/deny filters
7. Chooses the audio file to submit:
   - uses the converted file such as `.m4a` if `compressWav` is enabled and a converted file exists
   - otherwise uses the original `.wav`
8. Sends the audio to the configured Whisper-compatible HTTP endpoint as multipart form data
9. Stores the returned transcript and optional segments in the plugin's section of the call JSON

The request includes:

- `file`
- `model`
- `response_format=verbose_json`

It also includes `language` and `prompt` when configured.

Trunk Recorder runs blocking plugins directly against `call_info.call_json[plugin->name]`, which is why this plugin can enrich the final JSON output before the call is completed. 

## Talkgroup Filters

The plugin supports simple wildcard matching for talkgroups.

Supported wildcards:

- `*` matches any number of characters
- `?` matches a single character

Examples:

| Pattern | Matches |
| --- | --- |
| `1234` | Only talkgroup `1234` |
| `12*` | Any talkgroup starting with `12` |
| `56??` | Any four-digit talkgroup starting with `56` |
| `1*3` | Any talkgroup starting with `1` and ending with `3` |

Filter behavior:

- If `talkgroupAllow` is not empty, the talkgroup must match at least one allow pattern
- If `talkgroupDeny` is not empty, any matching deny pattern causes the call to be skipped
- Deny rules are applied after allow rules

## Plugin Output

This plugin writes the following keys into its per-call JSON context:

| Key | Type | Description |
| --- | --- | --- |
| `transcript` | string | The returned transcription text, or an empty string if skipped or failed. |
| `segments` | array | Segment list from the transcription response when `includeSegments` is enabled and the API provides segments. |
| `process_time_seconds` | number | Time spent processing the transcription request. |
| `skipped` | bool | Whether transcription was skipped. |
| `skip_reason` | string | Reason transcription was skipped. |
| `success` | bool | `true` if the transcription request completed successfully. |
| `error` | string | Error identifier when transcription fails. |

Current skip reasons include:

- `system_not_enabled`
- `encrypted`
- `talkgroup_filter`

Current error value on request failure:

- `transcription_request_failed`

### Example successful plugin output

```json
{
  "Whisper Transcribe": {
    "transcript": "Engine 4 respond to 123 Main Street for a reported structure fire.",
    "segments": [
      {
        "id": 0,
        "start": 0.0,
        "end": 2.8,
        "text": "Engine 4 respond to"
      },
      {
        "id": 1,
        "start": 2.8,
        "end": 6.1,
        "text": "123 Main Street for a reported structure fire."
      }
    ],
    "process_time_seconds": 1.42,
    "skipped": false,
    "skip_reason": "",
    "success": true,
    "error": ""
  }
}
```

### Example skipped plugin output

```json
{
  "Whisper Transcribe": {
    "transcript": "",
    "segments": [],
    "process_time_seconds": 0.0,
    "skipped": true,
    "skip_reason": "encrypted",
    "success": false,
    "error": ""
  }
}
```

## Example Config

```json
{
  "plugins": [
    {
      "name": "Whisper Transcribe",
      "library": "libwhisper_transcribe.so",
      "enabled": true,
      "pluginType": "blocking",
      "server": "http://127.0.0.1:8000/v1/audio/transcriptions",
      "apiKey": "",
      "model": "whisper-1",
      "timeoutSeconds": 300,
      "systems": [
        {
          "shortName": "county-p25",
          "enabled": true,
          "language": "en",
          "prompt": "",
          "includeSegments": true
        },
        {
          "shortName": "county-fire",
          "enabled": true,
          "language": "en",
          "prompt": "Public safety radio traffic. Expect unit numbers, addresses, and brief dispatch phrases.",
          "includeSegments": true,
          "talkgroupAllow": ["31*", "4501", "4502"],
          "talkgroupDeny": ["3199"]
        }
      ]
    }
  ]
}
```

If the plugin cannot be found, use a full path:

```json
{
  "name": "Whisper Transcribe",
  "library": "/usr/local/lib/trunk-recorder/libwhisper_transcribe.so",
  "enabled": true,
  "pluginType": "blocking",
  "server": "http://127.0.0.1:8000/v1/audio/transcriptions",
  "systems": [
    {
      "shortName": "county-p25"
    }
  ]
}
```

## Notes

- This is a **blocking** plugin and should be configured with `"pluginType": "blocking"`. If omitted, the plugin manager defaults `pluginType` to `blocking`, but setting it explicitly is clearer.
- The plugin manager also supports a top-level `"enabled"` flag for each plugin. If that is set to `false`, the plugin will not be loaded.
- This plugin expects a Whisper-compatible transcription API that accepts multipart uploads.
- The plugin requests `verbose_json` format and will parse the returned `text` and optional `segments` fields.
- If the HTTP request fails or the response is not valid JSON, the plugin marks the call as unsuccessful.
- If `compressWav` is enabled in Trunk Recorder and a converted output file exists, the plugin submits that compressed audio file instead of the original `.wav`.
- Encrypted calls are always skipped.