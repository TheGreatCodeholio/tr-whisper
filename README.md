<img src="https://raw.githubusercontent.com/TrunkRecorder/trunk-recorder/refs/heads/master/docs/media/trunk-recorder-header.png" width="75%" height="75%">

# Trunk Recorder Whisper Transcribe Plugin <!-- omit from toc -->

This is a plugin for Trunk Recorder that sends completed call audio to a Whisper-compatible or OpenAI-compatible transcription API and stores the transcription results in the call JSON for that call.

This plugin is a **blocking** plugin. It should be configured with:

- `"pluginType": "blocking"`

This allows the plugin to run during Trunk Recorder's blocking `call_end()` stage so it can enrich the final call JSON before the call is fully completed.

The plugin supports:

- Blocking post-call transcription
- Top-level plugin enable/disable
- Per-system enable/disable
- Optional language and prompt values
- Optional segment output
- Talkgroup allow/deny filters using simple wildcard patterns
- Selectable audio source for transcription (`wav` or `m4a`)
- Default transcription input of the original `.wav` file
- Automatic fallback to `.wav` if `audioSource` is invalid
- Automatic fallback to `.wav` if `audioSource` is `m4a` but no converted file is available
- Configurable `responseFormat`
- Model-aware default `responseFormat` selection
- Safe fallback to a supported `responseFormat` for known OpenAI models
- Backward-compatible handling for unknown custom/local models

Requires Trunk Recorder 5.0 or later and a Whisper-compatible or OpenAI-compatible HTTP transcription endpoint.

- [Install](#install)
- [Configure](#configure)
- [Response Format Behavior](#response-format-behavior)
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
| `server` | ✓ |  | string | Whisper-compatible or OpenAI-compatible transcription endpoint URL. |
| `apiKey` |  |  | string | Optional bearer token used as `Authorization: Bearer ...` when calling the transcription API. |
| `model` |  | `whisper-1` | string | Model name sent to the transcription API. This may be a standard OpenAI model or a custom/local model string. |
| `responseFormat` |  | model-dependent | string | Response format requested from the transcription API. If omitted, the plugin chooses a default based on the configured model. |
| `audioSource` |  | `wav` | string | Selects which audio file is sent to the transcription API. Supported values are `wav` and `m4a`. Defaults to `wav`. If invalid, the plugin logs a warning and falls back to `wav`. If set to `m4a` but no converted file is available, the plugin logs a warning and falls back to `wav`. |
| `timeoutSeconds` |  | `300` | integer | Total request timeout in seconds. |
| `systems` | ✓ |  | array | List of system-specific settings. At least one enabled system must be configured. |

### Per-system options

Each entry in `systems` applies to a Trunk Recorder system identified by `shortName`. The plugin validates that each configured `shortName` exists in the loaded Trunk Recorder systems during initialization.

| Key | Required | Default Value | Type | Description |
| --- | :---: | --- | --- | --- |
| `shortName` | ✓ |  | string | Must match the Trunk Recorder system `shortName`. |
| `enabled` |  | `true` | bool | Enables or disables transcription for this specific system within the plugin. |
| `language` |  |  | string | Optional language hint passed to the transcription API. |
| `prompt` |  |  | string | Optional prompt passed to the transcription API. For diarization models, prompt is ignored and the plugin logs a warning. |
| `includeSegments` |  | `true` | bool | If the API returns `segments`, include them in the plugin output. |
| `talkgroupAllow` |  |  | array | Optional allow list of talkgroup patterns. If set, only matching talkgroups are transcribed. |
| `talkgroupDeny` |  |  | array | Optional deny list of talkgroup patterns. Matching talkgroups are skipped. |

## Response Format Behavior

The plugin now chooses `responseFormat` in a model-aware way.

### Default response format selection

If `responseFormat` is **not** set in the config, the plugin chooses:

| Model | Default `responseFormat` |
| --- | --- |
| `whisper-1` | `verbose_json` |
| `gpt-4o-transcribe` | `json` |
| `gpt-4o-mini-transcribe` | `json` |
| `gpt-4o-transcribe-diarize` | `diarized_json` |
| Unknown/custom model | `verbose_json` |

This keeps OpenAI models on a safer default while preserving backward compatibility for local/custom Whisper-like APIs.

### Supported formats for known OpenAI models

For known OpenAI models, the plugin checks `responseFormat` and falls back if needed.

| Model | Supported `responseFormat` values |
| --- | --- |
| `whisper-1` | `json`, `text`, `srt`, `verbose_json`, `vtt` |
| `gpt-4o-transcribe` | `json`, `text` |
| `gpt-4o-mini-transcribe` | `json`, `text` |
| `gpt-4o-transcribe-diarize` | `json`, `text`, `diarized_json` |

If an unsupported format is configured for one of those known models, the plugin logs a warning and falls back to that model’s default format.

For unknown/custom models, the plugin does **not** hard-reject `responseFormat`. It keeps the configured value so that local APIs with custom behavior continue to work.

### Prompt handling

For `gpt-4o-transcribe-diarize`, prompts are not supported. If a prompt is configured for that model, the plugin logs a warning and ignores it.

## How It Works

When a call ends, the plugin:

1. Runs only if the top-level plugin `enabled` flag is `true`
2. Runs as a **blocking** plugin during Trunk Recorder's blocking `call_end()` stage
3. Looks up the matching configured system by `shortName`
4. Skips calls for systems that are not enabled
5. Skips encrypted calls
6. Applies optional talkgroup allow/deny filters
7. Chooses the audio file to submit:
   - uses the original `.wav` file by default
   - uses the converted `.m4a` file only when `audioSource` is set to `m4a` and a converted file is available
   - falls back to `.wav` if `audioSource` is invalid
   - falls back to `.wav` if `audioSource` is `m4a` but no converted file is available
8. Sends the audio to the configured transcription endpoint as multipart form data
9. Stores the returned transcript and optional structured data in the plugin's section of the call JSON

The request always includes:

- `file`
- `model`
- `response_format`

It also includes:

- `language` when configured
- `prompt` when configured and supported by the selected model

Trunk Recorder runs blocking plugins directly against `call_info.call_json[plugin->name]`, which is why this plugin can enrich the final JSON output before the call is completed.

### Response parsing

The plugin parses responses differently depending on `responseFormat`:

- `json`, `verbose_json`, `diarized_json`
  - parses the response as JSON
  - reads `text` into `transcript`
  - stores the full parsed response in `raw_response`
  - reads `segments` when present and `includeSegments` is enabled
  - includes `speaker` in segments when present
- `text`, `srt`, `vtt`
  - stores the raw response body directly in `transcript`
  - leaves `segments` empty

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
| `transcript` | string | The returned transcription text, or an empty string if skipped or failed. For `text`, `srt`, or `vtt`, this is the raw response body. |
| `segments` | array | Segment list from the transcription response when available and `includeSegments` is enabled. |
| `process_time_seconds` | number | Time spent processing the transcription request. |
| `skipped` | bool | Whether transcription was skipped. |
| `skip_reason` | string | Reason transcription was skipped. |
| `success` | bool | `true` if the transcription request completed successfully. |
| `error` | string | Error identifier when transcription fails. |
| `audio_source_requested` | string | The configured `audioSource` value after normalization and fallback handling. |
| `audio_source_used` | string | The actual audio type used for the request, either `wav` or `m4a`. |
| `audio_path` | string | The audio file path submitted to the transcription endpoint. |
| `response_format` | string | The response format actually requested. |
| `model` | string | The model used for transcription. |
| `raw_response` | object | The parsed raw JSON response when `responseFormat` is a JSON-based format. |

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
    "error": "",
    "audio_source_requested": "wav",
    "audio_source_used": "wav",
    "audio_path": "/path/to/audio.wav",
    "response_format": "verbose_json",
    "model": "whisper-1",
    "raw_response": {
      "text": "Engine 4 respond to 123 Main Street for a reported structure fire."
    }
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
    "error": "",
    "response_format": "verbose_json",
    "model": "whisper-1"
  }
}
```

## Example Config

### Default WAV transcription with `whisper-1`

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
      "audioSource": "wav",
      "timeoutSeconds": 300,
      "systems": [
        {
          "shortName": "county-p25",
          "enabled": true,
          "language": "en",
          "prompt": "Public safety radio traffic. Expect unit numbers, addresses, and dispatch phrasing.",
          "includeSegments": true
        }
      ]
    }
  ]
}
```

### OpenAI `gpt-4o-mini-transcribe`

```json
{
  "plugins": [
    {
      "name": "Whisper Transcribe",
      "library": "libwhisper_transcribe.so",
      "enabled": true,
      "pluginType": "blocking",
      "server": "https://api.openai.com/v1/audio/transcriptions",
      "apiKey": "YOUR_OPENAI_API_KEY",
      "model": "gpt-4o-mini-transcribe",
      "responseFormat": "json",
      "audioSource": "wav",
      "timeoutSeconds": 300,
      "systems": [
        {
          "shortName": "county-p25",
          "enabled": true,
          "language": "en",
          "prompt": "",
          "includeSegments": true
        }
      ]
    }
  ]
}
```

### OpenAI diarization

```json
{
  "plugins": [
    {
      "name": "Whisper Transcribe",
      "library": "libwhisper_transcribe.so",
      "enabled": true,
      "pluginType": "blocking",
      "server": "https://api.openai.com/v1/audio/transcriptions",
      "apiKey": "YOUR_OPENAI_API_KEY",
      "model": "gpt-4o-transcribe-diarize",
      "responseFormat": "diarized_json",
      "audioSource": "wav",
      "timeoutSeconds": 300,
      "systems": [
        {
          "shortName": "county-p25",
          "enabled": true,
          "language": "en",
          "prompt": "",
          "includeSegments": true
        }
      ]
    }
  ]
}
```

### Local/custom API preserving old behavior

```json
{
  "plugins": [
    {
      "name": "Whisper Transcribe",
      "library": "libwhisper_transcribe.so",
      "pluginType": "blocking",
      "enabled": true,
      "server": "https://stt.example.com/v1/audio/transcriptions",
      "apiKey": "YOUR_LOCAL_API_KEY",
      "model": "largev3-gpu|analog_audio",
      "responseFormat": "verbose_json",
      "audioSource": "wav",
      "timeoutSeconds": 300,
      "systems": [
        {
          "shortName": "county-p25",
          "enabled": true,
          "language": "en",
          "prompt": "",
          "includeSegments": true
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
  "apiKey": "",
  "model": "whisper-1",
  "audioSource": "wav",
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
- The default transcription input is the original `.wav` file.
- If `audioSource` is invalid, the plugin logs a warning and falls back to `wav`.
- If `audioSource` is `m4a` but no converted file is available, the plugin logs a warning and falls back to `wav`.
- For known OpenAI models, unsupported `responseFormat` values are automatically replaced with a safe default.
- For unknown custom/local models, the plugin keeps the configured `responseFormat` to preserve compatibility.
- For diarization models, prompts are ignored.
- If the HTTP request fails or the response cannot be parsed as expected for the configured format, the plugin marks the call as unsuccessful.
- Encrypted calls are always skipped.